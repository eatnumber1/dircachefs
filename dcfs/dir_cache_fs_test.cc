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
// syscall. It wraps syncfs the same way, so that a test can run requests
// while a sync point (backing::SyncBacking) waits on its syncfs, and
// name_to_handle_at, which a probe (ProbeObject) calls after it has opened
// the name it probes: a hook there runs after a resolve read its answer
// and before it is recorded (or acted on). A hook
// can process a whole second request, or begin a mutation and leave it in
// flight, while the request under test is "suspended" there. The rule that
// transactions never span a syscall is what makes this safe: the hook never
// runs inside one. No production code knows about any of this.
//
// Trace validation (formal/README.md): a test that calls StartTrace() has
// its protocol events recorded (dcfs/testonly/trace_recorder.h) to stdout,
// which is the guest's serial log; //dcfs:dir_cache_fs_trace_test runs this
// binary and checks every trace against the model (formal/Trace.tla). The
// interleavings the hooks make are the reason to: they are the ones the
// model checks and today's single thread never produces.

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
#include <cstdio>
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
#include "dcfs/protocol_events.h"
#include "dcfs/status.h"
#include "dcfs/testonly/trace_recorder.h"
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

// The hook the next open_by_handle_at runs (once).
std::function<void()> &OpenByHandleHook() {
  static auto *hook = new std::function<void()>();
  return *hook;
}

// The hook the next name_to_handle_at runs (once).
std::function<void()> &NameToHandleHook() {
  static auto *hook = new std::function<void()>();
  return *hook;
}

// The hook the next syncfs runs (once).
std::function<void()> &SyncfsHook() {
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
int __real_name_to_handle_at(int dirfd, const char *pathname,
                             struct file_handle *handle, int *mount_id,
                             int flags);
int __wrap_name_to_handle_at(int dirfd, const char *pathname,
                             struct file_handle *handle, int *mount_id,
                             int flags) {
  std::function<void()> hook = std::exchange(dcfs::NameToHandleHook(), {});
  if (hook) hook();
  return __real_name_to_handle_at(dirfd, pathname, handle, mount_id, flags);
}
int __real_syncfs(int fd);
int __wrap_syncfs(int fd) {
  std::function<void()> hook = std::exchange(dcfs::SyncfsHook(), {});
  if (hook) hook();
  return __real_syncfs(fd);
}
}  // extern "C"

namespace dcfs {
namespace {

namespace fs = std::filesystem;

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Not;
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

// A write the kernel would make through a passthrough fd, which dcfs never
// sees.
void AppendToFile(const std::string &path, std::string_view data) {
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  ASSERT_GE(fd, 0) << path << ": " << std::strerror(errno);
  EXPECT_EQ(::write(fd, data.data(), data.size()),
            static_cast<ssize_t>(data.size()));
  ::close(fd);
}

uint64_t InoOf(const std::string &path) {
  struct stat st {};
  EXPECT_EQ(::lstat(path.c_str(), &st), 0) << path << ": "
                                          << std::strerror(errno);
  return st.st_ino;
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
    ctx_.events = &NoProtocolEvents();
    OpenByHandleHook() = {};
    NameToHandleHook() = {};
    SyncfsHook() = {};
    if (se_ != nullptr) fuse_session_destroy(se_);
    current_ = nullptr;
    fs_.reset();
    std::error_code ec;
    if (!source_.empty()) fs::remove_all(source_, ec);
  }

  std::string Path(std::string_view rel) const {
    return absl::StrCat(source_, "/", rel);
  }

  // Records this test's protocol events from here on (see the top of this
  // file), as the trace "<suite>.<test>". Call it once the test's setup is
  // done: its writes to the cache are the state each directory's trace
  // begins in.
  void StartTrace() {
    const ::testing::TestInfo *info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    std::fflush(stdout);
    recorder_ = std::make_unique<testonly::TraceRecorder>(
        STDOUT_FILENO,
        absl::StrCat(info->test_suite_name(), ".", info->name()));
    ctx_.events = recorder_.get();
    recorder_->BeginAll(ctx_);
  }

  // Runs `fn(i)` (i counting down from `times`) inside each of the next
  // `times` fills that open a node and then probe a name (a resolve or a
  // population: OpenNode, the fill snapshot, then each probe): at the first
  // probe after each open, so after the fill's snapshot. Not inside `fn`
  // itself, and not again until the next open.
  void MutateDuringFills(int times, std::function<void(int)> fn) {
    struct State {
      int remaining;
      bool opened = false;
      bool busy = false;
      std::function<void(int)> fn;
    };
    auto state = std::make_shared<State>(State{.remaining = times, .fn = fn});
    auto arm_open = std::make_shared<std::function<void()>>();
    auto arm_probe = std::make_shared<std::function<void()>>();
    *arm_open = [state, arm_open] {
      if (state->remaining == 0) return;
      OpenByHandleHook() = [state, arm_open] {
        if (!state->busy) state->opened = true;
        (*arm_open)();
      };
    };
    *arm_probe = [state, arm_probe] {
      if (state->remaining == 0) return;
      NameToHandleHook() = [state, arm_probe] {
        if (state->opened && !state->busy) {
          state->opened = false;
          state->busy = true;
          state->fn(state->remaining--);
          state->busy = false;
        }
        (*arm_probe)();
      };
    };
    (*arm_open)();
    (*arm_probe)();
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

  // An open of `id` with `flags`, and the file handle it returned (0 on
  // error).
  std::pair<Reply, uint64_t> Open(InodeId id, int flags) {
    struct fuse_open_in in = {};
    in.flags = static_cast<uint32_t>(flags);
    std::string body;
    AppendBytes(body, in);
    Reply reply = Send(FUSE_OPEN, static_cast<uint64_t>(id), body);
    struct fuse_open_out out {};
    if (reply.error != 0 || reply.payload.size() < sizeof(out)) {
      return {reply, 0};
    }
    std::memcpy(&out, reply.payload.data(), sizeof(out));
    return {reply, out.fh};
  }

  // A create of `name` in `parent` with `flags`, and the new inode and file
  // handle it returned (0 on error).
  struct Created {
    Reply reply;
    InodeId id = 0;
    uint64_t fh = 0;
  };
  Created Create(InodeId parent, std::string_view name, int flags) {
    struct fuse_create_in in = {};
    in.flags = static_cast<uint32_t>(flags);
    in.mode = S_IFREG | 0644;
    std::string body;
    AppendBytes(body, in);
    body.append(name);
    body.push_back('\0');
    Created created{.reply = Send(FUSE_CREATE, static_cast<uint64_t>(parent),
                                  body)};
    struct fuse_entry_out entry {};
    struct fuse_open_out open {};
    if (created.reply.error != 0 ||
        created.reply.payload.size() < sizeof(entry) + sizeof(open)) {
      return created;
    }
    std::memcpy(&entry, created.reply.payload.data(), sizeof(entry));
    std::memcpy(&open, created.reply.payload.data() + sizeof(entry),
                sizeof(open));
    created.id = static_cast<InodeId>(entry.nodeid);
    created.fh = open.fh;
    return created;
  }

  // A mkdir of `name` in `parent`, and the new inode (0 on error).
  std::pair<Reply, InodeId> Mkdir(InodeId parent, std::string_view name) {
    struct fuse_mkdir_in in = {};
    in.mode = 0755;
    std::string body;
    AppendBytes(body, in);
    body.append(name);
    body.push_back('\0');
    Reply reply = Send(FUSE_MKDIR, static_cast<uint64_t>(parent), body);
    struct fuse_entry_out entry {};
    if (reply.error != 0 || reply.payload.size() < sizeof(entry)) {
      return {reply, 0};
    }
    std::memcpy(&entry, reply.payload.data(), sizeof(entry));
    return {reply, static_cast<InodeId>(entry.nodeid)};
  }

  Reply Release(InodeId id, uint64_t fh) {
    struct fuse_release_in in = {};
    in.fh = fh;
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_RELEASE, static_cast<uint64_t>(id), body);
  }

  Reply Unlink(InodeId parent, std::string_view name) {
    std::string body(name);
    body.push_back('\0');
    return Send(FUSE_UNLINK, static_cast<uint64_t>(parent), body);
  }

  // An fsync of directory `id`: the fsync itself, then a sync point.
  Reply Fsyncdir(InodeId id) {
    struct fuse_fsync_in in = {};
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_FSYNCDIR, static_cast<uint64_t>(id), body);
  }

  std::vector<InodeId> Dirty() {
    absl::StatusOr<std::vector<InodeId>> dirty = cache::ListDirty(ctx_);
    EXPECT_THAT(dirty, IsOk());
    return dirty.ok() ? *std::move(dirty) : std::vector<InodeId>{};
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
  std::unique_ptr<testonly::TraceRecorder> recorder_;
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
    absl::StatusOr<cache::Mutation> m =
        cache::BeginRemove(ctx_, d, "x", x, cache::BeginFill(ctx_));
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
    absl::StatusOr<cache::Mutation> m =
        cache::BeginRemove(ctx_, d, "x", x, cache::BeginFill(ctx_));
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
                       cache::BeginRemove(ctx_, kRootInode, "a", a,
                                          cache::BeginFill(ctx_)));
  EXPECT_EQ(ErrnoOf(List(kRootInode, true).status()), EAGAIN);
  EXPECT_EQ(ErrnoOf(List(kRootInode, false).status()), EAGAIN);
  unlink.End();
  EXPECT_THAT(List(kRootInode, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "a", "b")));
}

// --- formal/ finding rename_stale_source ---------------------------------
//
// Rename resolves its source (and destination) before its phase 1, and
// phase 3 links the new name to the resolved source. Mutation::Owns only
// notices mutations that overlap from phase 1 on, so phase 1 must verify
// that nothing the rename names changed since it resolved them.
//
// The interleaving, in a directory d (the root would be opened through its
// mount fd, which the hook does not see): "a" and "link_a" are hard links
// to one file, "c" is another. A rename of a over b begins; b has no row
// and d's listing is incomplete, so resolving b populates d, and at that
// population's first syscall (OpenNode of d) a rename of c over a runs to
// completion. Then the first rename's renameat2 moves c's file to b.
class RenameStaleSourceTest : public DirCacheFSTest {
 protected:
  void Build(bool second_link) {
    ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
    WriteFile(Path("d/a"));
    if (second_link) {
      ASSERT_EQ(::link(Path("d/a").c_str(), Path("d/link_a").c_str()), 0);
    }
    WriteFile(Path("d/c"));
    ino_a_ = InoOf(Path("d/a"));
    ino_c_ = InoOf(Path("d/c"));
    Start();
    ASSERT_OK_AND_ASSIGN(d_, Id("d"));
    ASSERT_THAT(Id("a", d_), IsOk());  // Populates d: rows for all.
    ASSERT_THAT(Id("c", d_), IsOk());
    ASSERT_THAT(cache::MarkDirComplete(ctx_, d_, false), IsOk());
    ASSERT_EQ(Cached(d_, "b").first, LookupResult::kUnknown);
  }

  // Runs the interleaving; both renames must succeed.
  void RunRenames() {
    StartTrace();
    std::optional<Reply> concurrent;
    OpenByHandleHook() = [&] { concurrent = Rename(d_, "c", d_, "a"); };
    Reply reply = Rename(d_, "a", d_, "b");
    ASSERT_TRUE(concurrent.has_value()) << "the hook did not run";
    EXPECT_EQ(concurrent->error, 0);
    EXPECT_EQ(reply.error, 0);
    // On the backing filesystem, c's file ended up as b.
    ASSERT_EQ(InoOf(Path("d/b")), ino_c_);
    ASSERT_NE(::access(Path("d/a").c_str(), F_OK), 0);
    ASSERT_NE(::access(Path("d/c").c_str(), F_OK), 0);
  }

  InodeId d_ = 0;
  uint64_t ino_a_ = 0;
  uint64_t ino_c_ = 0;
};

// With a second link the old file's row survives the concurrent rename, so
// the old phase 3 recorded b -> the old file, which b is not.
TEST_F(RenameStaleSourceTest, OldObjectWithAnotherLinkIsNotLinked) {
  Build(/*second_link=*/true);
  RunRenames();
  EXPECT_EQ(Cached(d_, "b"), std::make_pair(LookupResult::kFound, ino_c_));
  EXPECT_NE(Cached(d_, "a").first, LookupResult::kFound);
  EXPECT_NE(Cached(d_, "c").first, LookupResult::kFound);
  EXPECT_EQ(Cached(d_, "link_a"),
            std::make_pair(LookupResult::kFound, ino_a_));
}

// Without one the old file's row is gone; the old phase 3's LinkDentry
// failed and rolled back, leaving b unknown. Now the rename re-resolves
// and records what b really is.
TEST_F(RenameStaleSourceTest, OldObjectWithoutAnotherLinkIsNotLinked) {
  Build(/*second_link=*/false);
  RunRenames();
  EXPECT_EQ(Cached(d_, "b"), std::make_pair(LookupResult::kFound, ino_c_));
  EXPECT_NE(Cached(d_, "a").first, LookupResult::kFound);
  EXPECT_NE(Cached(d_, "c").first, LookupResult::kFound);
}

// The other side: while a mutation of the parent stays in flight, a
// rename's phase 1 cannot verify what it resolved; after a few attempts
// the rename fails with EAGAIN (TODO(coroutines): wait instead), and
// nothing has been renamed.
TEST_F(DirCacheFSTest, RenameIsRefusedWhileItsParentKeepsChanging) {
  WriteFile(Path("a"));
  WriteFile(Path("x"));
  Start();
  ASSERT_THAT(Id("a"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::Mutation other,
                       cache::BeginCreate(ctx_, kRootInode, "x2"));
  EXPECT_EQ(Rename(kRootInode, "a", kRootInode, "b").error, -EAGAIN);
  EXPECT_EQ(::access(Path("a").c_str(), F_OK), 0);
  EXPECT_NE(::access(Path("b").c_str(), F_OK), 0);
  other.End();
  EXPECT_EQ(Rename(kRootInode, "a", kRootInode, "b").error, 0);
  EXPECT_EQ(Cached(kRootInode, "b"),
            std::make_pair(LookupResult::kFound, InoOf(Path("b"))));
}

// --- Writable opens vs. sync points (review of R4, finding 1) -----------
//
// The kernel writes a file open for writing through passthrough, which
// dcfs never sees, from the writable open (DirCacheFS::BeginWriting, a
// durable phase 1) until the last writable RELEASE. A sync point may clear
// the file's dirty row only if its syncfs began after the last of those
// writes, i.e. after that RELEASE: before, the writes may not be durable,
// and a power loss could keep the attributes the RELEASE recorded and lose
// the writes, with nothing left for recovery to forget.

// The last RELEASE arrives (after one more write) while the sync point
// waits on its syncfs. The old code built the set of inodes to keep from
// the writable opens outstanding at the clear, when the file had none any
// more, and nothing told the fill guards the writes had ended.
TEST_F(DirCacheFSTest, ReleaseDuringASyncPointKeepsTheDirtyRow) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_THAT(Dirty(), Contains(f));

  StartTrace();
  std::optional<Reply> release;
  SyncfsHook() = [&] {
    AppendToFile(Path("f"), "late");
    release = Release(f, fh);
  };
  EXPECT_EQ(Fsyncdir(kRootInode).error, 0);
  ASSERT_TRUE(release.has_value()) << "the hook did not run";
  EXPECT_EQ(release->error, 0);
  EXPECT_THAT(Dirty(), Contains(f));

  // The next sync point began after the last write: now it may go.
  EXPECT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_THAT(Dirty(), Not(Contains(f)));
}

// The same event as seen by a fill: attributes read while the file was
// open for writing (the kernel may write at any moment) must not be
// recorded as current once the last RELEASE is past, since the RELEASE
// recorded fresher ones.
TEST_F(DirCacheFSTest, ReleaseEndsTheWritesForAFillThatBeganBefore) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);

  // A fill begins and reads f's attributes; then a write, and the RELEASE.
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx_);
  struct statx stale {};
  ASSERT_EQ(::statx(AT_FDCWD, Path("f").c_str(), AT_SYMLINK_NOFOLLOW,
                    STATX_BASIC_STATS | STATX_BTIME, &stale),
            0);
  AppendToFile(Path("f"), "late");
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr fresh, cache::GetAttr(ctx_, f));
  ASSERT_TRUE(fresh.valid);
  ASSERT_EQ(fresh.st.st_size, 4);

  EXPECT_THAT(cache::FillAttr(ctx_, snapshot, f, stale), IsOkAndHolds(false));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr after, cache::GetAttr(ctx_, f));
  EXPECT_EQ(after.st.st_size, 4);
}

// A writable CREATE: its new row is dirty only through phase 3's MarkDirty,
// which no fill guard sees, and the old code added the inode to the
// writable opens only after more suspension points (here MakeBackingFile's
// open_by_handle_at), at which a whole sync point could clear that row.
// The writes after the reply then had no dirty row.
TEST_F(DirCacheFSTest, WritableCreateIsDirtyWhenReplied) {
  Start();
  StartTrace();
  std::optional<Reply> fsync;
  OpenByHandleHook() = [&] { fsync = Fsyncdir(kRootInode); };
  Created created = Create(kRootInode, "new", O_RDWR | O_CREAT | O_EXCL);
  ASSERT_TRUE(fsync.has_value()) << "the hook did not run";
  EXPECT_EQ(fsync->error, 0);
  ASSERT_EQ(created.reply.error, 0);
  EXPECT_THAT(Dirty(), Contains(created.id));

  // Written, released, synced: the row goes.
  AppendToFile(Path("new"), "data");
  ASSERT_EQ(Release(created.id, created.fh).error, 0);
  EXPECT_THAT(Dirty(), Contains(created.id));
  EXPECT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_THAT(Dirty(), Not(Contains(created.id)));
}

// The failure paths: a writable OPEN or CREATE whose phase 1
// (BeginWriting) fails -- here the database refuses writes from the
// moment MakeBackingFile opens the file -- replies the error and undoes
// its registration: no BackingFile, not open for writing.
TEST_F(DirCacheFSTest, WritableOpenWhosePhase1FailsIsUndone) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  OpenByHandleHook() = [&] {
    ASSERT_THAT(db_.Exec("PRAGMA query_only = 1"), IsOk());
  };
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_THAT(db_.Exec("PRAGMA query_only = 0"), IsOk());
  EXPECT_NE(open.error, 0);
  EXPECT_FALSE(fs_->HasOpenFiles(f));
  EXPECT_FALSE(ctx_.open_for_write->contains(f));
}

TEST_F(DirCacheFSTest, WritableCreateWhosePhase1FailsIsUndone) {
  Start();
  OpenByHandleHook() = [&] {
    ASSERT_THAT(db_.Exec("PRAGMA query_only = 1"), IsOk());
  };
  Created created = Create(kRootInode, "new", O_RDWR | O_CREAT | O_EXCL);
  ASSERT_THAT(db_.Exec("PRAGMA query_only = 0"), IsOk());
  EXPECT_NE(created.reply.error, 0);
  // The file was created (phase 2 succeeded before the open failed).
  ASSERT_OK_AND_ASSIGN(InodeId id, Id("new"));
  EXPECT_FALSE(fs_->HasOpenFiles(id));
  EXPECT_FALSE(ctx_.open_for_write->contains(id));
}

// --- Unlink's stale resolve (review of R4, finding 2) --------------------
//
// RemoveChild resolves the child (which may take syscalls) before its
// phase 1, and phase 1 marks the attributes of that child unknown, while
// the unlinkat removes whatever the name holds when it runs. As for
// Rename, phase 1 must verify that nothing it names changed since the
// resolve began.
//
// The interleaving, in a directory d: "a" and "link_a" are hard links to
// one file X, "b" and "link_b" to another, Y. An unlink of a resolves it
// (its row is unknown), and right after the probe opened X (at
// name_to_handle_at) a rename of b over a runs to completion; the probe's
// answer cannot be recorded, but it answers X. Then the unlink's unlinkat
// removes Y's link a. The old code had marked X unknown instead, so Y's
// row stayed current with the link count from before (2), and the unlink
// settled X rather than Y.
TEST_F(DirCacheFSTest, UnlinkMarksWhatItRemovesUnknown) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/a"));
  ASSERT_EQ(::link(Path("d/a").c_str(), Path("d/link_a").c_str()), 0);
  WriteFile(Path("d/b"));
  ASSERT_EQ(::link(Path("d/b").c_str(), Path("d/link_b").c_str()), 0);
  const uint64_t ino_x = InoOf(Path("d/a"));
  const uint64_t ino_y = InoOf(Path("d/b"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_OK_AND_ASSIGN(InodeId x, Id("a", d));  // Populates d.
  ASSERT_OK_AND_ASSIGN(InodeId y, Id("b", d));
  ASSERT_THAT(cache::UnlinkDentry(ctx_, d, "a"), IsOk());
  ASSERT_THAT(cache::ChildrenComplete(ctx_, d), IsOkAndHolds(true));

  StartTrace();
  std::optional<Reply> concurrent;
  NameToHandleHook() = [&] { concurrent = Rename(d, "b", d, "a"); };
  Reply reply = Unlink(d, "a");
  ASSERT_TRUE(concurrent.has_value()) << "the hook did not run";
  EXPECT_EQ(concurrent->error, 0);
  EXPECT_EQ(reply.error, 0);
  // On the backing filesystem, Y's link a is what the unlink removed.
  ASSERT_NE(::access(Path("d/a").c_str(), F_OK), 0);
  ASSERT_NE(::access(Path("d/b").c_str(), F_OK), 0);
  ASSERT_EQ(InoOf(Path("d/link_a")), ino_x);
  ASSERT_EQ(InoOf(Path("d/link_b")), ino_y);

  EXPECT_EQ(Cached(d, "a").first, LookupResult::kNegative);
  // Each file's row is unknown or right: one link left each.
  for (InodeId id : {x, y}) {
    SCOPED_TRACE(id);
    ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, id));
    if (attr.valid) {
      EXPECT_EQ(attr.st.st_nlink, 1u);
    }
  }
}

// The other side: while a mutation of the parent stays in flight, an
// unlink's phase 1 cannot verify what it resolved; after a few attempts
// it fails with EAGAIN (TODO(coroutines): wait instead), and nothing has
// been removed.
TEST_F(DirCacheFSTest, UnlinkIsRefusedWhileItsParentKeepsChanging) {
  WriteFile(Path("a"));
  Start();
  ASSERT_THAT(Id("a"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::Mutation other,
                       cache::BeginCreate(ctx_, kRootInode, "x2"));
  EXPECT_EQ(Unlink(kRootInode, "a").error, -EAGAIN);
  EXPECT_EQ(::access(Path("a").c_str(), F_OK), 0);
  EXPECT_EQ(Cached(kRootInode, "a").first, LookupResult::kFound);
  other.End();
  EXPECT_EQ(Unlink(kRootInode, "a").error, 0);
  EXPECT_NE(::access(Path("a").c_str(), F_OK), 0);
  EXPECT_EQ(Cached(kRootInode, "a").first, LookupResult::kNegative);
}

// --- formal/ finding sync_during_mutation --------------------------------
//
// The model's counterexample, through the real request path: the root is
// dirty; an FSYNCDIR's sync point takes its snapshot and waits on syncfs;
// meanwhile a whole MKDIR in the root runs (phase 1, mkdirat, phase 3, its
// end). The syncfs may have begun before the mkdirat, so the clear must
// keep the root's row (in the snapshot, but mutated since) and the new
// directory's (added after the snapshot); only what was dirty and
// untouched (the first directory) may go.
TEST_F(DirCacheFSTest, MkdirDuringASyncPointKeepsItsDirtyRows) {
  Start();
  StartTrace();
  auto [first, first_id] = Mkdir(kRootInode, "first");
  ASSERT_EQ(first.error, 0);
  ASSERT_THAT(Dirty(), UnorderedElementsAre(kRootInode, first_id));

  std::optional<std::pair<Reply, InodeId>> mkdir;
  SyncfsHook() = [&] { mkdir = Mkdir(kRootInode, "new"); };
  EXPECT_EQ(Fsyncdir(kRootInode).error, 0);
  ASSERT_TRUE(mkdir.has_value()) << "the hook did not run";
  ASSERT_EQ(mkdir->first.error, 0);
  EXPECT_THAT(Dirty(), UnorderedElementsAre(kRootInode, mkdir->second));

  // The next sync point began after the mkdirat: now both may go.
  EXPECT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_THAT(Dirty(), ::testing::IsEmpty());
}

// --- Trace validation's own scenarios ------------------------------------
//
// Requests whose traces (StartTrace) reach model actions the scenarios above
// do not: the retries and the EAGAIN of a phase 1 whose verification keeps
// failing, and of a readdir whose listing keeps being invalidated, each
// through real requests (a mutation of the directory inside every resolve or
// listing); and the common requests one after another.

// A rename resolves its source (unknown, in a complete listing: one probe)
// three times, and a mkdir in the same directory runs during each resolve
// (after its fill snapshot, so the resolve cannot record the name, which
// stays unknown), so phase 1's verification fails every time: EAGAIN.
TEST_F(DirCacheFSTest, RenameGivesUpWhileItsParentKeepsChanging) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/a"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_THAT(Id("a", d), IsOk());  // Populates d.
  ASSERT_THAT(cache::UnlinkDentry(ctx_, d, "a"), IsOk());

  StartTrace();
  int mkdirs = 0;
  MutateDuringFills(3, [&](int i) {
    EXPECT_EQ(Mkdir(d, absl::StrCat("m", i)).first.error, 0);
    ++mkdirs;
  });
  EXPECT_EQ(Rename(d, "a", d, "b").error, -EAGAIN);
  EXPECT_EQ(mkdirs, 3);
  EXPECT_EQ(::access(Path("d/a").c_str(), F_OK), 0);
}

// The same for an unlink.
TEST_F(DirCacheFSTest, UnlinkGivesUpWhileItsParentKeepsChanging) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/a"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_THAT(Id("a", d), IsOk());  // Populates d.
  ASSERT_THAT(cache::UnlinkDentry(ctx_, d, "a"), IsOk());

  StartTrace();
  int mkdirs = 0;
  MutateDuringFills(3, [&](int i) {
    EXPECT_EQ(Mkdir(d, absl::StrCat("m", i)).first.error, 0);
    ++mkdirs;
  });
  EXPECT_EQ(Unlink(d, "a").error, -EAGAIN);
  EXPECT_EQ(mkdirs, 3);
  EXPECT_EQ(::access(Path("d/a").c_str(), F_OK), 0);
}

// A readdir of an incomplete directory populates it three times, and a
// mkdir in it runs during each population (after its fill snapshot), so no
// listing can be recorded: EAGAIN.
TEST_F(DirCacheFSTest, ReaddirGivesUpWhileItsDirectoryKeepsChanging) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/a"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_THAT(Id("a", d), IsOk());  // Populates d.
  ASSERT_THAT(cache::MarkDirComplete(ctx_, d, false), IsOk());

  StartTrace();
  int mkdirs = 0;
  MutateDuringFills(3, [&](int i) {
    EXPECT_EQ(Mkdir(d, absl::StrCat("m", i)).first.error, 0);
    ++mkdirs;
  });
  EXPECT_EQ(ErrnoOf(List(d, false).status()), EAGAIN);
  EXPECT_EQ(mkdirs, 3);
  // Nothing in flight any more: now it is listed.
  EXPECT_THAT(List(d, true), IsOkAndHolds(UnorderedElementsAre(
                                 ".", "..", "a", "m1", "m2", "m3")));
}

// The common requests in a directory, one at a time: lookups (served, and
// resolved), readdir and readdirplus, getattr, a mkdir that fails (EEXIST)
// and one that succeeds, a rename and an unlink (and both again, ENOENT),
// and a sync point.
TEST_F(DirCacheFSTest, CommonRequestsMatchTheModel) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/a"));
  WriteFile(Path("d/b"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));

  StartTrace();
  EXPECT_THAT(List(d, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "a", "b")));
  EXPECT_THAT(List(d, false),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "a", "b")));
  struct fuse_getattr_in getattr = {};
  std::string body;
  AppendBytes(body, getattr);
  EXPECT_EQ(Send(FUSE_GETATTR, static_cast<uint64_t>(d), body).error, 0);
  EXPECT_EQ(Mkdir(d, "a").first.error, -EEXIST);
  EXPECT_EQ(Mkdir(d, "c").first.error, 0);
  EXPECT_EQ(Rename(d, "a", d, "e").error, 0);
  EXPECT_EQ(Unlink(d, "b").error, 0);
  EXPECT_EQ(Unlink(d, "b").error, -ENOENT);
  EXPECT_EQ(Rename(d, "b", d, "f").error, -ENOENT);
  EXPECT_THAT(List(d, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "c", "e")));
  EXPECT_EQ(Fsyncdir(d).error, 0);
  EXPECT_THAT(Dirty(), Not(Contains(d)));
}

// A getattr and a readdirplus of directories whose attributes are unknown
// (left so by the setup): each refreshes them (its statx, then a fill).
TEST_F(DirCacheFSTest, UnknownAttributesAreRefreshed) {
  ASSERT_EQ(::mkdir(Path("d1").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(Path("d2").c_str(), 0755), 0);
  WriteFile(Path("d2/a"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d1, Id("d1"));
  ASSERT_OK_AND_ASSIGN(InodeId d2, Id("d2"));
  ASSERT_THAT(Id("a", d2), IsOk());  // Populates d2.
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, d1), IsOk());
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, d2), IsOk());

  StartTrace();
  struct fuse_getattr_in getattr = {};
  std::string body;
  AppendBytes(body, getattr);
  EXPECT_EQ(Send(FUSE_GETATTR, static_cast<uint64_t>(d1), body).error, 0);
  EXPECT_THAT(List(d2, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "a")));
  for (InodeId id : {d1, d2}) {
    ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, id));
    EXPECT_TRUE(attr.valid);
  }
}

// A mkdir in a directory whose listing is complete. Phase 1 marks the new
// name unknown, and the trace shows it. Trace validation's fault-injection
// test (//dcfs:trace_fault_injection_test) runs this test alone in a build
// whose phase 1 skips that write (testonly/skip_mark_unknown.cc), and
// requires validation to reject its trace at that phase 1.
TEST_F(DirCacheFSTest, CreateMarksItsNameUnknown) {
  WriteFile(Path("a"));
  Start();
  ASSERT_THAT(Id("a"), IsOk());  // Populates the root.
  ASSERT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));

  StartTrace();
  EXPECT_EQ(Mkdir(kRootInode, "new").first.error, 0);
  EXPECT_EQ(Cached(kRootInode, "new").first, LookupResult::kFound);
}

}  // namespace
}  // namespace dcfs
