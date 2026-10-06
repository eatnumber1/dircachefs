// DirCacheFS driven through forged FUSE requests, for interleavings that
// only coroutines can produce in the daemon itself.
//
// The requests go through a real libfuse session with custom I/O (as
// fuse_request_channel_test.cc explains: fuse_session_custom_io() plus
// fuse_session_process_buf() with hand-built buffers, no kernel and no
// mount) and through fuse_ops.cc's real dispatch table, so each op runs
// exactly as it does in the daemon, against a real backing directory.
//
// Interleaving: today dcfs serves one request at a time, but under the
// planned coroutines every backing syscall is a suspension point at which
// other requests run (docs/design.md, "Concurrency, today and with
// coroutines"). This binary links with -Wl,--wrap=open_by_handle_at (see
// BUILD.bazel), the syscall behind every backing::OpenNode: a test arms a
// hook, and the next open_by_handle_at call runs it first, before the real
// syscall. The hook can process a whole second request, or begin a
// mutation and leave it in flight, while the request under test is
// "suspended" there. The rule that transactions never span a syscall is
// what makes this safe: the hook never runs inside one. No production code
// knows about any of this.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include "dcfs/dir_cache_fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_ops.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// As in dcfs/backing_test.cc: this Abseil has no ASSERT_OK_AND_ASSIGN.
#define DCFS_TEST_CONCAT_INNER(x, y) x##y
#define DCFS_TEST_CONCAT(x, y) DCFS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                        \
  ASSERT_OK_AND_ASSIGN_IMPL(                                    \
      DCFS_TEST_CONCAT(_status_or_value_, __LINE__), lhs, rexpr)
#define ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());        \
  lhs = std::move(statusor).value()

namespace dcfs {
namespace {

// The hook the next open_by_handle_at runs (once), and how many calls it
// has seen while armed or not.
std::function<void()> &OpenByHandleHook() {
  static auto *hook = new std::function<void()>();
  return *hook;
}

}  // namespace
}  // namespace dcfs

extern "C" {
int __real_open_by_handle_at(int mount_fd, struct file_handle *handle,
                             int flags);
int __wrap_open_by_handle_at(int mount_fd, struct file_handle *handle,
                             int flags) {
  std::function<void()> hook = std::exchange(dcfs::OpenByHandleHook(), {});
  if (hook) hook();
  return __real_open_by_handle_at(mount_fd, handle, flags);
}
}  // extern "C"

namespace dcfs {
namespace {

namespace fs = std::filesystem;

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;
using cache::InodeId;
using cache::kRootInode;
using cache::LookupResult;

MATCHER_P(IsLookup, kind, "") { return arg.kind == kind; }

int ErrnoOf(const absl::Status &status) {
  return GetErrnoFromStatus(status).value_or(0);
}

template <typename T>
void AppendBytes(std::string &out, const T &value) {
  out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void WriteFile(const std::string &path) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0) << path << ": " << std::strerror(errno);
  ::close(fd);
}

// A reply as the kernel would receive it.
struct Reply {
  int error = 0;  // fuse_out_header.error: 0 or a negative errno.
  std::string payload;
};

class DirCacheFSTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    std::string templ = absl::StrCat(tmpdir, "/dcfs_XXXXXX");
    ASSERT_NE(::mkdtemp(templ.data()), nullptr) << std::strerror(errno);
    source_ = templ;
  }

  // Starts the filesystem over the source tree built so far: a fresh
  // cache, a DirCacheFS, and a libfuse session that has seen FUSE_INIT.
  void Start() {
    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
    int source_fd =
        ::open(source_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(source_fd, 0);
    FileDescriptor owned(source_fd);
    ASSERT_OK_AND_ASSIGN(RootIdentity root,
                         backing::ProbeRoot(ctx_, source_fd));
    ASSERT_THAT(Migrate(db_, root), IsOk());
    ASSERT_THAT(backing::InitRoot(ctx_, std::move(owned)), IsOk());

    // No periodic sync point in the middle of a test.
    fs_ = std::make_unique<DirCacheFS>(
        ctx_, DirCacheFS::Options{.sync_interval = absl::Hours(24)});
    ops_ = MakeFuseOps();
    char arg0[] = "dir_cache_fs_test";
    char *argv[] = {arg0};
    struct fuse_args args = FUSE_ARGS_INIT(1, argv);
    se_ = fuse_session_new(&args, &ops_, sizeof(ops_), fs_.get());
    ASSERT_NE(se_, nullptr);
    struct fuse_custom_io io = {};
    io.read = [](int, void *, size_t, void *) -> ssize_t {
      errno = ENOSYS;  // Never called: requests come in through Send().
      return -1;
    };
    // The I/O callbacks get the session's userdata, which is the
    // DirCacheFS (fuse_ops.cc reaches it through fuse_req_userdata), so
    // they reach this test through `current_`.
    io.writev = [](int, struct iovec *iov, int count, void *) -> ssize_t {
      return current_->Capture(iov, count);
    };
    int dummy_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    ASSERT_GE(dummy_fd, 0);
    current_ = this;
    ASSERT_EQ(fuse_session_custom_io(se_, &io, sizeof(io), dummy_fd), 0);

    struct fuse_init_in init_in = {};
    init_in.major = FUSE_KERNEL_VERSION;
    init_in.minor = FUSE_KERNEL_MINOR_VERSION;
    // What DirCacheFS::Init requires (it refuses the INIT without POSIX
    // ACLs and DONT_MASK) and what it uses.
    init_in.flags = FUSE_POSIX_ACL | FUSE_DONT_MASK | FUSE_DO_READDIRPLUS |
                    FUSE_EXPORT_SUPPORT;
    std::string body;
    AppendBytes(body, init_in);
    Reply reply = Send(FUSE_INIT, 0, body);
    ASSERT_EQ(reply.error, 0);
  }

  void TearDown() override {
    OpenByHandleHook() = {};
    if (se_ != nullptr) fuse_session_destroy(se_);
    current_ = nullptr;
    fs_.reset();
    std::error_code ec;
    if (!source_.empty()) fs::remove_all(source_, ec);
  }

  std::string Path(std::string_view rel) const {
    return absl::StrCat(source_, "/", rel);
  }

  // Processes one request and returns its reply (requests processed by a
  // hook meanwhile get their own).
  Reply Send(uint32_t opcode, uint64_t nodeid, std::string_view body) {
    const uint64_t unique = next_unique_++;
    std::string buf;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + body.size());
    hdr.opcode = opcode;
    hdr.unique = unique;
    hdr.nodeid = nodeid;
    // Root, from pid 0 (whose groups cannot be read: none, see
    // FuseRequest::Caller).
    AppendBytes(buf, hdr);
    buf.append(body);
    struct fuse_buf fbuf = {};
    fbuf.mem = buf.data();
    fbuf.size = buf.size();
    fuse_session_process_buf(se_, &fbuf);
    auto it = replies_.find(unique);
    EXPECT_NE(it, replies_.end()) << "no reply to request " << unique;
    if (it == replies_.end()) return Reply{.error = -EIO};
    Reply reply = std::move(it->second);
    replies_.erase(it);
    return reply;
  }

  Reply Rename(InodeId parent, std::string_view name, InodeId newparent,
               std::string_view newname) {
    struct fuse_rename_in in = {};
    in.newdir = static_cast<uint64_t>(newparent);
    std::string body;
    AppendBytes(body, in);
    body.append(name);
    body.push_back('\0');
    body.append(newname);
    body.push_back('\0');
    return Send(FUSE_RENAME, static_cast<uint64_t>(parent), body);
  }

  // The names a READDIRPLUS (plus = true) or READDIR of `dir` from offset 0
  // returns, in order; an error reply as its errno.
  absl::StatusOr<std::vector<std::string>> List(InodeId dir, bool plus) {
    struct fuse_read_in in = {};
    in.size = 64 * 1024;
    std::string body;
    AppendBytes(body, in);
    Reply reply = Send(plus ? FUSE_READDIRPLUS : FUSE_READDIR,
                       static_cast<uint64_t>(dir), body);
    if (reply.error != 0) {
      return dcfs::ErrnoToStatus(-reply.error, "readdir reply");
    }
    std::vector<std::string> names;
    const std::string &p = reply.payload;
    size_t pos = 0;
    while (pos < p.size()) {
      const size_t dirent_at =
          plus ? pos + offsetof(struct fuse_direntplus, dirent) : pos;
      struct fuse_dirent d {};
      if (dirent_at + FUSE_NAME_OFFSET > p.size()) break;
      std::memcpy(&d, p.data() + dirent_at, FUSE_NAME_OFFSET);
      names.emplace_back(p.substr(dirent_at + FUSE_NAME_OFFSET, d.namelen));
      pos += plus ? FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET_DIRENTPLUS + d.namelen)
                  : FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + d.namelen);
    }
    return names;
  }

  // The cached answer for (dir, name), and the backing inode number of the
  // row it points at (0 if not kFound).
  std::pair<LookupResult::Kind, uint64_t> Cached(InodeId dir,
                                                 std::string_view name) {
    absl::StatusOr<LookupResult> result = cache::Lookup(ctx_, dir, name);
    EXPECT_THAT(result, IsOk());
    if (!result.ok()) return {LookupResult::kUnknown, 0};
    if (result->kind != LookupResult::kFound) return {result->kind, 0};
    absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, result->id);
    EXPECT_THAT(attr, IsOk());
    return {result->kind, attr.ok() ? attr->backing_ino : 0};
  }

  absl::StatusOr<InodeId> Id(std::string_view name, InodeId dir = kRootInode) {
    absl::StatusOr<LookupResult> result =
        backing::LookupOrPopulate(ctx_, dir, name);
    if (!result.ok()) return result.status();
    if (result->kind != LookupResult::kFound) {
      return absl::NotFoundError(absl::StrCat(name, " not found"));
    }
    return result->id;
  }

  std::string source_;
  sqlite3::Connection db_;
  MountFds mounts_;
  absl::BitGen bitgen_{std::seed_seq{4, 10}};
  Context ctx_{db_, mounts_, bitgen_};
  std::unique_ptr<DirCacheFS> fs_;
  fuse_lowlevel_ops ops_{};
  struct fuse_session *se_ = nullptr;

 private:
  ssize_t Capture(struct iovec *iov, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
      out.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
    }
    struct fuse_out_header hdr {};
    if (out.size() < sizeof(hdr)) {
      ADD_FAILURE() << "short reply";
      errno = EINVAL;
      return -1;
    }
    std::memcpy(&hdr, out.data(), sizeof(hdr));
    replies_[hdr.unique] =
        Reply{.error = hdr.error, .payload = out.substr(sizeof(hdr))};
    return static_cast<ssize_t>(out.size());
  }

  static inline DirCacheFSTest *current_ = nullptr;
  std::map<uint64_t, Reply> replies_;
  uint64_t next_unique_ = 1;
};

// --- formal/ finding readdirplus_unlocked --------------------------------
//
// A listing is served from the cache only while the directory is complete
// (cache::IsDirComplete: complete, and no row unknown); ListDir itself
// skips every row that is not present. So the completeness check and the
// listing it vouches for must not have a suspension point between them: a
// name made unknown in between (a mutation's phase 1) would be left out
// although it may still exist.

// Readdirplus answers "." with EntryFor, which refreshes the directory's
// attributes from the backing filesystem when they are unknown: a
// suspension point (OpenNode; the root would be reached through its mount
// fd instead, so this uses a subdirectory). An unlink of "x" begins there
// and is still in flight (its unlinkat has not run) when the old code built
// the listing.
TEST_F(DirCacheFSTest, ReaddirplusListsWhatWasCompleteWhenChecked) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/x"));
  WriteFile(Path("d/y"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_OK_AND_ASSIGN(InodeId x, Id("x", d));
  ASSERT_THAT(cache::IsDirComplete(ctx_, d), IsOkAndHolds(true));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, d), IsOk());

  std::optional<cache::Mutation> unlink;
  OpenByHandleHook() = [&] {
    absl::StatusOr<cache::Mutation> m = cache::BeginRemove(ctx_, d, "x", x);
    ASSERT_THAT(m, IsOk());
    unlink.emplace(*std::move(m));
  };
  absl::StatusOr<std::vector<std::string>> names = List(d, true);
  ASSERT_TRUE(unlink.has_value()) << "the hook did not run";
  // "x" still exists on the backing filesystem.
  EXPECT_THAT(names, IsOkAndHolds(UnorderedElementsAre(".", "..", "x", "y")));
  unlink.reset();
}

// Readdir of a directory whose own dentry is unknown resolves ".." from the
// backing filesystem (backing::ParentOf): a suspension point too.
TEST_F(DirCacheFSTest, ReaddirListsWhatWasCompleteWhenChecked) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/x"));
  WriteFile(Path("d/y"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_OK_AND_ASSIGN(InodeId x, Id("x", d));
  ASSERT_THAT(cache::IsDirComplete(ctx_, d), IsOkAndHolds(true));
  ASSERT_THAT(cache::UnlinkDentry(ctx_, kRootInode, "d"), IsOk());

  std::optional<cache::Mutation> unlink;
  OpenByHandleHook() = [&] {
    absl::StatusOr<cache::Mutation> m = cache::BeginRemove(ctx_, d, "x", x);
    ASSERT_THAT(m, IsOk());
    unlink.emplace(*std::move(m));
  };
  absl::StatusOr<std::vector<std::string>> names = List(d, false);
  ASSERT_TRUE(unlink.has_value()) << "the hook did not run";
  EXPECT_THAT(names, IsOkAndHolds(UnorderedElementsAre(".", "..", "x", "y")));
  unlink.reset();
}

// The other side: while a mutation of the directory is in flight, its
// listing cannot be recorded, so it is never served; after a few attempts
// the request fails with EAGAIN (TODO(coroutines): wait instead).
TEST_F(DirCacheFSTest, ReaddirplusIsNotServedWhileAMutationIsInFlight) {
  WriteFile(Path("a"));
  WriteFile(Path("b"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId a, Id("a"));
  ASSERT_OK_AND_ASSIGN(cache::Mutation unlink,
                       cache::BeginRemove(ctx_, kRootInode, "a", a));
  EXPECT_EQ(ErrnoOf(List(kRootInode, true).status()), EAGAIN);
  EXPECT_EQ(ErrnoOf(List(kRootInode, false).status()), EAGAIN);
  unlink.End();
  EXPECT_THAT(List(kRootInode, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "a", "b")));
}

}  // namespace
}  // namespace dcfs
