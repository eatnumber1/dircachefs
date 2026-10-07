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
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
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

// Inode numbers every statx reports differently while set: a backing inode
// number (the key) is reported as another (the value). For backing inode
// numbers no supported filesystem hands out (>= 2^63).
std::map<uint64_t, uint64_t> &FakeInodeNumbers() {
  static auto *fake = new std::map<uint64_t, uint64_t>();
  return *fake;
}

// If nonzero, the next statx fails with this errno (once).
int &StatxFailure() {
  static int err = 0;
  return err;
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
int __real_statx(int dirfd, const char *path, int flags, unsigned int mask,
                 struct statx *buf);
int __wrap_statx(int dirfd, const char *path, int flags, unsigned int mask,
                 struct statx *buf) {
  if (int err = std::exchange(dcfs::StatxFailure(), 0); err != 0) {
    errno = err;
    return -1;
  }
  int ret = __real_statx(dirfd, path, flags, mask, buf);
  if (ret == 0) {
    auto it = dcfs::FakeInodeNumbers().find(buf->stx_ino);
    if (it != dcfs::FakeInodeNumbers().end()) buf->stx_ino = it->second;
  }
  return ret;
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
std::string ReadWholeFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

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

    fs_ = std::make_unique<DirCacheFS>(ctx_, options_);
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
                    FUSE_EXPORT_SUPPORT | FUSE_IOCTL_DIR;
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
    FakeInodeNumbers().clear();
    StatxFailure() = 0;
    for (const std::string &mount : mounts_below_) {
      ::umount2(mount.c_str(), MNT_DETACH);
    }
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
        absl::StrCat(info->test_suite_name(), ".", info->name()),
        /*files=*/true);
    ctx_.events = recorder_.get();
    recorder_->BeginAll(ctx_);
  }

  // Tells the trace, if one is recording, that this test changed the
  // backing file of `id` behind dcfs's back (formal/reval.tla's
  // out-of-band change).
  void OutOfBand(InodeId id) {
    if (recorder_ != nullptr) recorder_->NoteOutOfBand(id);
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

  // Opens `id` (the file at `rel`) read-only while its backing file is
  // immutable (set and cleared behind dcfs's back), so that the shared
  // backing descriptor is read-only. Returns the handle (0 on failure).
  uint64_t OpenReadOnlySharedFd(const std::string &rel, InodeId id) {
    const int raw = ::open(Path(rel).c_str(), O_RDONLY | O_CLOEXEC);
    if (raw < 0) return 0;
    int flags = 0;
    uint64_t fh = 0;
    if (::ioctl(raw, FS_IOC_GETFLAGS, &flags) == 0) {
      const int immutable = flags | FS_IMMUTABLE_FL;
      if (::ioctl(raw, FS_IOC_SETFLAGS, &immutable) == 0) {
        OutOfBand(id);
        fh = Open(id, O_RDONLY).second;
        if (::ioctl(raw, FS_IOC_SETFLAGS, &flags) != 0) fh = 0;
        OutOfBand(id);
      }
    }
    ::close(raw);
    return fh;
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

  // A LOOKUP of `name` in `parent`, and the entry it returned (zeroed on
  // error).
  std::pair<Reply, struct fuse_entry_out> Lookup(InodeId parent,
                                                 std::string_view name) {
    std::string body(name);
    body.push_back('\0');
    Reply reply = Send(FUSE_LOOKUP, static_cast<uint64_t>(parent), body);
    struct fuse_entry_out entry {};
    if (reply.error == 0 && reply.payload.size() >= sizeof(entry)) {
      std::memcpy(&entry, reply.payload.data(), sizeof(entry));
    }
    return {reply, entry};
  }

  // A GETATTR of `id`, and the attributes it returned (zeroed on error).
  std::pair<Reply, struct fuse_attr> Getattr(InodeId id) {
    struct fuse_getattr_in in = {};
    std::string body;
    AppendBytes(body, in);
    Reply reply = Send(FUSE_GETATTR, static_cast<uint64_t>(id), body);
    struct fuse_attr_out out {};
    if (reply.error == 0 && reply.payload.size() >= sizeof(out)) {
      std::memcpy(&out, reply.payload.data(), sizeof(out));
    }
    return {reply, out.attr};
  }

  // A FORGET of `n` of the kernel's lookups of `id` (no reply).
  void Forget(InodeId id, uint64_t n) {
    struct fuse_forget_in in = {};
    in.nlookup = n;
    std::string body;
    AppendBytes(body, in);
    const uint64_t unique = next_unique_++;
    std::string buf;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + body.size());
    hdr.opcode = FUSE_FORGET;
    hdr.unique = unique;
    hdr.nodeid = static_cast<uint64_t>(id);
    AppendBytes(buf, hdr);
    buf.append(body);
    struct fuse_buf fbuf = {};
    fbuf.mem = buf.data();
    fbuf.size = buf.size();
    fuse_session_process_buf(se_, &fbuf);
  }

  // A BATCH_FORGET of (id, n) pairs (no reply).
  void BatchForget(std::vector<std::pair<InodeId, uint64_t>> forgets) {
    struct fuse_batch_forget_in in = {};
    in.count = static_cast<uint32_t>(forgets.size());
    std::string body;
    AppendBytes(body, in);
    for (auto [id, n] : forgets) {
      struct fuse_forget_one one = {};
      one.nodeid = static_cast<uint64_t>(id);
      one.nlookup = n;
      AppendBytes(body, one);
    }
    const uint64_t unique = next_unique_++;
    std::string buf;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + body.size());
    hdr.opcode = FUSE_BATCH_FORGET;
    hdr.unique = unique;
    AppendBytes(buf, hdr);
    buf.append(body);
    struct fuse_buf fbuf = {};
    fbuf.mem = buf.data();
    fbuf.size = buf.size();
    fuse_session_process_buf(se_, &fbuf);
  }

  // A TMPFILE in `parent` with `flags`: the new inode and file handle (0
  // on error).
  Created Tmpfile(InodeId parent, int flags) {
    struct fuse_create_in in = {};
    in.flags = static_cast<uint32_t>(flags);
    in.mode = S_IFREG | 0640;
    std::string body;
    AppendBytes(body, in);
    body.append("/");
    body.push_back('\0');
    Created created{
        .reply = Send(FUSE_TMPFILE, static_cast<uint64_t>(parent), body)};
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

  // A COPY_FILE_RANGE of `len` bytes from the start of open file (in,
  // fh_in) to the start of (out, fh_out); the reply's count, or its errno
  // negated.
  int64_t CopyFileRange(InodeId in, uint64_t fh_in, InodeId out,
                        uint64_t fh_out, uint64_t len) {
    struct fuse_copy_file_range_in arg = {};
    arg.fh_in = fh_in;
    arg.nodeid_out = static_cast<uint64_t>(out);
    arg.fh_out = fh_out;
    arg.len = len;
    std::string body;
    AppendBytes(body, arg);
    Reply reply = Send(FUSE_COPY_FILE_RANGE, static_cast<uint64_t>(in), body);
    if (reply.error != 0) return reply.error;
    struct fuse_write_out written {};
    if (reply.payload.size() < sizeof(written)) return -EIO;
    std::memcpy(&written, reply.payload.data(), sizeof(written));
    return written.size;
  }

  // An IOCTL of `id` with `cmd`, input `in` and `out_size` bytes of output:
  // the reply (its payload a fuse_ioctl_out and the output).
  Reply Ioctl(InodeId id, unsigned int cmd, std::string_view in,
              uint32_t out_size, uint32_t flags = 0) {
    struct fuse_ioctl_in arg = {};
    arg.cmd = cmd;
    arg.flags = flags;
    arg.in_size = static_cast<uint32_t>(in.size());
    arg.out_size = out_size;
    std::string body;
    AppendBytes(body, arg);
    body.append(in);
    return Send(FUSE_IOCTL, static_cast<uint64_t>(id), body);
  }

  Reply Opendir(InodeId id) {
    struct fuse_open_in in = {};
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_OPENDIR, static_cast<uint64_t>(id), body);
  }

  Reply Link(InodeId id, InodeId newparent, std::string_view newname) {
    struct fuse_link_in in = {};
    in.oldnodeid = static_cast<uint64_t>(id);
    std::string body;
    AppendBytes(body, in);
    body.append(newname);
    body.push_back('\0');
    return Send(FUSE_LINK, static_cast<uint64_t>(newparent), body);
  }

  Reply Setxattr(InodeId id, std::string_view name, std::string_view value) {
    struct fuse_setxattr_in in = {};
    in.size = static_cast<uint32_t>(value.size());
    // The INIT did not ask for FUSE_SETXATTR_EXT: the old, short header.
    std::string body(reinterpret_cast<const char *>(&in),
                     FUSE_COMPAT_SETXATTR_IN_SIZE);
    body.append(name);
    body.push_back('\0');
    body.append(value);
    return Send(FUSE_SETXATTR, static_cast<uint64_t>(id), body);
  }

  // A GETXATTR of `name` asking for its size (size 0).
  Reply Getxattr(InodeId id, std::string_view name) {
    struct fuse_getxattr_in in = {};
    std::string body;
    AppendBytes(body, in);
    body.append(name);
    body.push_back('\0');
    return Send(FUSE_GETXATTR, static_cast<uint64_t>(id), body);
  }

  // A SETATTR of `id`'s mode (as chmod(2) sends it).
  Reply Chmod(InodeId id, mode_t mode) {
    struct fuse_setattr_in in = {};
    in.valid = FATTR_MODE;
    in.mode = mode;
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_SETATTR, static_cast<uint64_t>(id), body);
  }

  // Mounts a tmpfs on `rel` (a directory of the source): a filesystem
  // boundary below the source. Unmounted at TearDown.
  void MountBelow(std::string_view rel) {
    const std::string path = Path(rel);
    ASSERT_EQ(::mount("tmpfs", path.c_str(), "tmpfs", 0, "mode=0751"), 0)
        << path << ": " << std::strerror(errno);
    mounts_below_.push_back(path);
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
  std::vector<std::string> mounts_below_;
  sqlite3::Connection db_;
  MountFds mounts_;
  absl::BitGen bitgen_{std::seed_seq{4, 10}};
  Context ctx_{db_, mounts_, bitgen_};
  // What Start() makes the DirCacheFS with (a test may change it first).
  // No periodic sync point in the middle of a test; held descriptors
  // (written_) up to a fixed cap, not the default derived from the test
  // process's descriptor limit (0 at the usual 1024).
  DirCacheFS::Options options_{.sync_interval = absl::Hours(24),
                               .max_held_fds = 64};
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

// --- Scenarios for trace validation's fault builds ------------------------
//
// Requests with no expectations of their own: a fault build
// (dcfs/BUILD.bazel's DIR_CACHE_FS_FAULTS) breaks what they do, and only
// trace validation is to notice. The normal build validates their traces
// with the others.

// An unlink of a cached name in the root.
TEST_F(DirCacheFSTest, TraceScenarioUnlink) {
  WriteFile(Path("a"));
  WriteFile(Path("b"));
  Start();
  ASSERT_THAT(Id("a"), IsOk());  // Populates the root.

  StartTrace();
  Unlink(kRootInode, "a");
}

// A mkdir in the root during a sync point's syncfs (as
// MkdirDuringASyncPointKeepsItsDirtyRows, without its expectations).
TEST_F(DirCacheFSTest, TraceScenarioMkdirDuringSync) {
  Start();
  StartTrace();
  Mkdir(kRootInode, "first");
  SyncfsHook() = [&] { Mkdir(kRootInode, "new"); };
  Fsyncdir(kRootInode);
}

// A mkdir in d during a readdir's population of d (at its first probe,
// after its fill snapshot): the recorder holds d's lines meanwhile.
TEST_F(DirCacheFSTest, TraceScenarioMkdirDuringListing) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/a"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_EQ(d, 2);  // The fault test names its trace by d's inode.
  ASSERT_THAT(Id("a", d), IsOk());  // Populates d.
  ASSERT_THAT(cache::MarkDirComplete(ctx_, d, false), IsOk());

  StartTrace();
  NameToHandleHook() = [&] { Mkdir(d, "m"); };
  List(d, false).IgnoreError();
}

// A mkdir in a directory whose listing is complete. Phase 1 marks the new
// name unknown, and the trace shows it. Trace validation's fault-injection
// test (//dcfs:trace_fault_skip_mark_unknown_test) runs this test alone in a build
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


// --- copy_file_range, ioctls, O_TMPFILE (step 23.4) -----------------------

// A writable open that shares a backing fd opened before the file became
// immutable is refused, as a fresh open by the backing filesystem is. The
// flag is set through dcfs (FUSE_IOCTL, as chattr does): flags change only
// that way under exclusive access, and only after such a change does a
// writable open sharing a read-write fd ask the backing filesystem again.
TEST_F(DirCacheFSTest, WritableOpenOfAnImmutableFileIsRefused) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  StartTrace();  // and the file's (formal/reval.tla)
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  int flags = 0;
  const int raw = ::open(Path("f").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(raw, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_GETFLAGS, &flags), 0);
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &immutable), 0);
  OutOfBand(f);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [ro, ro_fh] = Open(f, O_RDONLY);
  EXPECT_EQ(ro.error, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &flags), 0);
  OutOfBand(f);
  auto [rw, rw_fh] = Open(f, O_WRONLY);
  EXPECT_EQ(rw.error, 0);
  ::close(raw);
  for (uint64_t h : {fh, ro_fh, rw_fh}) {
    if (h != 0) {
      EXPECT_EQ(Release(f, h).error, 0);
    }
  }
}

// The same with the flag set through dcfs (FUSE_IOCTL, as chattr on the
// mount does).
TEST_F(DirCacheFSTest, WritableOpenAfterChattrThroughDcfsIsRefused) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  StartTrace();  // and the file's (formal/reval.tla)
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  Reply get = Ioctl(f, FS_IOC_GETFLAGS, "", sizeof(int));
  ASSERT_EQ(get.error, 0);
  int flags = 0;
  std::memcpy(&flags, get.payload.data() + sizeof(struct fuse_ioctl_out),
              sizeof(flags));
  const int immutable = flags | FS_IMMUTABLE_FL;
  auto as_bytes = [](const int &v) {
    return std::string(reinterpret_cast<const char *>(&v), sizeof(v));
  };
  ASSERT_EQ(Ioctl(f, FS_IOC_SETFLAGS, as_bytes(immutable), 0).error, 0);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [ro, ro_fh] = Open(f, O_RDONLY);
  EXPECT_EQ(ro.error, 0);
  ASSERT_EQ(Ioctl(f, FS_IOC_SETFLAGS, as_bytes(flags), 0).error, 0);
  auto [rw, rw_fh] = Open(f, O_WRONLY);
  EXPECT_EQ(rw.error, 0);
  for (uint64_t h : {fh, ro_fh, rw_fh}) {
    if (h != 0) {
      EXPECT_EQ(Release(f, h).error, 0);
    }
  }
}

// Append-only behind dcfs's back, with a read-write shared fd: a writable
// open without O_APPEND is refused, one with it allowed, as the backing
// filesystem decides (review L-b).
TEST_F(DirCacheFSTest, WritableOpenOfAnAppendOnlyFileNeedsOAppend) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  StartTrace();  // and the file's (formal/reval.tla)
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  int flags = 0;
  const int raw = ::open(Path("f").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(raw, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_GETFLAGS, &flags), 0);
  const int append_only = flags | FS_APPEND_FL;
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &append_only), 0);
  OutOfBand(f);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [app, app_fh] = Open(f, O_WRONLY | O_APPEND);
  EXPECT_EQ(app.error, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &flags), 0);
  OutOfBand(f);
  ::close(raw);
  for (uint64_t h : {fh, app_fh}) {
    if (h != 0) {
      EXPECT_EQ(Release(f, h).error, 0);
    }
  }
}

// For trace validation against the revalidation model (formal/reval.tla):
// a chmod through dcfs of a file open for writing, chattr +a through dcfs,
// a chmod the backing filesystem refuses (the file is append-only), the
// writable opens the flag decides, chattr -a and a chmod again.
TEST_F(DirCacheFSTest, ChmodAndChattrOfAnOpenFileMatchTheRevalModel) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  StartTrace();
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  EXPECT_EQ(Chmod(f, 0600).error, 0);
  Reply get = Ioctl(f, FS_IOC_GETFLAGS, "", sizeof(int));
  ASSERT_EQ(get.error, 0);
  int flags = 0;
  std::memcpy(&flags, get.payload.data() + sizeof(struct fuse_ioctl_out),
              sizeof(flags));
  auto as_bytes = [](const int &v) {
    return std::string(reinterpret_cast<const char *>(&v), sizeof(v));
  };
  const int append_only = flags | FS_APPEND_FL;
  ASSERT_EQ(Ioctl(f, FS_IOC_SETFLAGS, as_bytes(append_only), 0).error, 0);
  EXPECT_EQ(Chmod(f, 0644).error, -EPERM);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [app, app_fh] = Open(f, O_WRONLY | O_APPEND);
  EXPECT_EQ(app.error, 0);
  ASSERT_EQ(Ioctl(f, FS_IOC_SETFLAGS, as_bytes(flags), 0).error, 0);
  EXPECT_EQ(Chmod(f, 0644).error, 0);
  for (uint64_t h : {fh, app_fh}) {
    if (h != 0) {
      EXPECT_EQ(Release(f, h).error, 0);
    }
  }
}

// For trace validation against the revalidation model (formal/reval.tla):
// a flag change the backing filesystem refuses (FS_IOC_FSSETXATTR of a
// project id, which an ext4 without the project feature answers with
// EOPNOTSUPP, changing nothing), and the append-only flag set and cleared
// through FS_IOC_FSSETXATTR, with the writable opens it decides in between.
TEST_F(DirCacheFSTest, RefusedAndFsxattrFlagChangesMatchTheRevalModel) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  StartTrace();
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  Reply getx = Ioctl(f, FS_IOC_FSGETXATTR, "", sizeof(struct fsxattr));
  ASSERT_EQ(getx.error, 0);
  struct fsxattr fsx {};
  std::memcpy(&fsx, getx.payload.data() + sizeof(struct fuse_ioctl_out),
              sizeof(fsx));
  auto as_bytes = [](const struct fsxattr &v) {
    return std::string(reinterpret_cast<const char *>(&v), sizeof(v));
  };
  struct fsxattr project = fsx;
  project.fsx_projid = 7;
  EXPECT_EQ(Ioctl(f, FS_IOC_FSSETXATTR, as_bytes(project), 0).error,
            -EOPNOTSUPP);
  struct fsxattr append_only = fsx;
  append_only.fsx_xflags |= FS_XFLAG_APPEND;
  ASSERT_EQ(Ioctl(f, FS_IOC_FSSETXATTR, as_bytes(append_only), 0).error, 0);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [app, app_fh] = Open(f, O_WRONLY | O_APPEND);
  EXPECT_EQ(app.error, 0);
  ASSERT_EQ(Ioctl(f, FS_IOC_FSSETXATTR, as_bytes(fsx), 0).error, 0);
  auto [rw, rw_fh] = Open(f, O_WRONLY);
  EXPECT_EQ(rw.error, 0);
  for (uint64_t h : {fh, app_fh, rw_fh}) {
    if (h != 0) {
      EXPECT_EQ(Release(f, h).error, 0);
    }
  }
}

// An unnamed file (O_TMPFILE) has a row but no name; linking it into a
// name is, to the directory, a create (the model's "linkcreate", which
// trace validation checks here: the second link is EEXIST).
TEST_F(DirCacheFSTest, TmpfileLinkedIntoANameIsACreate) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/taken"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_THAT(Id("taken", d), IsOk());  // Populates d.
  StartTrace();
  Created tmp = Tmpfile(d, O_RDWR);
  ASSERT_EQ(tmp.reply.error, 0);
  EXPECT_TRUE(fs_->IsUnnamedTmpfile(static_cast<fuse_ino_t>(tmp.id)));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, tmp.id));
  EXPECT_FALSE(attr.valid);  // Open for writing, and nlink 0.
  EXPECT_THAT(Dirty(), Contains(tmp.id));
  EXPECT_THAT(List(d, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "taken")));

  EXPECT_EQ(Link(tmp.id, d, "taken").error, -EEXIST);
  EXPECT_TRUE(fs_->IsUnnamedTmpfile(static_cast<fuse_ino_t>(tmp.id)));
  EXPECT_EQ(Link(tmp.id, d, "named").error, 0);
  EXPECT_FALSE(fs_->IsUnnamedTmpfile(static_cast<fuse_ino_t>(tmp.id)));
  ASSERT_OK_AND_ASSIGN(attr, cache::GetAttr(ctx_, tmp.id));
  EXPECT_EQ(Cached(d, "named"),
            std::make_pair(LookupResult::kFound, attr.backing_ino));
  EXPECT_EQ(Release(tmp.id, tmp.fh).error, 0);
  ASSERT_OK_AND_ASSIGN(attr, cache::GetAttr(ctx_, tmp.id));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_nlink, 1u);
  EXPECT_EQ(InoOf(Path("d/named")), attr.backing_ino);
}

// One never linked: its last release retires its row (nlink 0), and the
// kernel's reference keeps it readable until the last FORGET.
TEST_F(DirCacheFSTest, TmpfileNeverLinkedLeavesNoRow) {
  Start();
  Created tmp = Tmpfile(kRootInode, O_WRONLY | O_EXCL);
  ASSERT_EQ(tmp.reply.error, 0);
  EXPECT_EQ(Release(tmp.id, tmp.fh).error, 0);
  EXPECT_THAT(cache::GetAttr(ctx_, tmp.id).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_FALSE(fs_->IsUnnamedTmpfile(static_cast<fuse_ino_t>(tmp.id)));
  auto [getattr, attr] = Getattr(tmp.id);
  ASSERT_EQ(getattr.error, 0);
  EXPECT_EQ(attr.nlink, 0u);
  Forget(tmp.id, 1);
  EXPECT_EQ(Getattr(tmp.id).first.error, -ESTALE);
  EXPECT_THAT(List(kRootInode, false),
              IsOkAndHolds(UnorderedElementsAre(".", "..")));
}

// An unnamed file left by a crash (no release ever came) is forgotten at
// the next start after an unclean shutdown, as the row-lifetime rule would
// have at its last release; so is any non-directory row with no link and
// no name (review L5). Rows with a name, or with links, stay.
TEST_F(DirCacheFSTest, StartupAfterACrashForgetsUnnamedRows) {
  WriteFile(Path("named"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId named, Id("named"));
  Created tmp = Tmpfile(kRootInode, O_RDWR);
  ASSERT_EQ(tmp.reply.error, 0);
  ASSERT_THAT(cache::GetAttr(ctx_, tmp.id), IsOk());
  // The daemon "crashes": no release; the next start finds an unclean
  // shutdown.
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  ASSERT_THAT(backing::StartRun(ctx_, "boot"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, tmp.id).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::GetAttr(ctx_, named), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, kRootInode), IsOk());
}

// The sweep of unnamed rows is best effort: if it fails, startup goes on
// (the rows cost only a re-probe), logged, not a failed mount.
TEST_F(DirCacheFSTest, StartupGoesOnIfTheUnnamedRowSweepFails) {
  WriteFile(Path("named"));
  Start();
  Created tmp = Tmpfile(kRootInode, O_RDWR);
  ASSERT_EQ(tmp.reply.error, 0);
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_delete BEFORE DELETE ON inodes "
                       "BEGIN SELECT RAISE(ABORT, 'no deletes'); END"),
              IsOk());
  EXPECT_THAT(backing::StartRun(ctx_, "boot"), IsOk());
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_delete"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, tmp.id), IsOk());  // Left for next time.
}

// A tmpfile whose create fails after its row was recorded (here its first
// attribute read) takes the row back with it (review L5).
TEST_F(DirCacheFSTest, TmpfileUndoForgetsItsRow) {
  Start();
  // RecordTmpfile's probe calls name_to_handle_at after its own statx; the
  // next statx is the reply's attribute refresh, which fails.
  NameToHandleHook() = [] { StatxFailure() = EIO; };
  Created tmp = Tmpfile(kRootInode, O_RDWR);
  EXPECT_EQ(tmp.reply.error, -EIO);
  ASSERT_OK_AND_ASSIGN(int64_t rows,
                       [&]() -> absl::StatusOr<int64_t> {
                         ABSL_ASSIGN_OR_RETURN(
                             sqlite3::Statement * stmt,
                             db_.Prepared("SELECT COUNT(*) FROM inodes"));
                         ABSL_ASSIGN_OR_RETURN(bool row, stmt->Step());
                         if (!row) return absl::InternalError("no row");
                         int64_t n = stmt->Column<int64_t>(0);
                         ABSL_RETURN_IF_ERROR(stmt->Reset());
                         return n;
                       }());
  EXPECT_EQ(rows, 1);  // The root only.
}

// A tmpfile in a directory whose backing open fails, and in a stub.
TEST_F(DirCacheFSTest, TmpfileFailuresAreReplied) {
  ASSERT_EQ(::mkdir(Path("mp").c_str(), 0755), 0);
  Start();
  MountBelow("mp");
  auto [lookup, entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(lookup.error, 0);
  EXPECT_EQ(Tmpfile(static_cast<InodeId>(entry.nodeid), O_RDWR).reply.error,
            -ENOTSUP);
  EXPECT_EQ(Tmpfile(12345, O_RDWR).reply.error, -ESTALE);
}

TEST_F(DirCacheFSTest, CopyFileRangeCopiesOnTheBackingFiles) {
  WriteFile(Path("src"));
  AppendToFile(Path("src"), "0123456789");
  WriteFile(Path("dst"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId src, Id("src"));
  ASSERT_OK_AND_ASSIGN(InodeId dst, Id("dst"));
  auto [in, in_fh] = Open(src, O_RDONLY);
  auto [out, out_fh] = Open(dst, O_WRONLY);
  ASSERT_EQ(in.error, 0);
  ASSERT_EQ(out.error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_EQ(CopyFileRange(src, in_fh, dst, out_fh, 100), 10);
  EXPECT_EQ(std::filesystem::file_size(Path("dst")), 10u);
  EXPECT_THAT(Dirty(), Contains(dst));  // The copy's phase 1.
  EXPECT_EQ(Release(dst, out_fh).error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, dst));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 10);

  // A failure the backing filesystem reports is replied, and leaves the
  // attributes right: here a destination whose shared backing fd is
  // read-only (the file was immutable when it was opened).
  int flags = 0;
  const int raw = ::open(Path("dst").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(raw, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_GETFLAGS, &flags), 0);
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &immutable), 0);
  auto [ro, ro_fh] = Open(dst, O_RDONLY);
  ASSERT_EQ(ro.error, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &flags), 0);
  ::close(raw);
  EXPECT_EQ(CopyFileRange(src, in_fh, dst, ro_fh, 100), -EBADF);
  EXPECT_EQ(std::filesystem::file_size(Path("dst")), 10u);
  EXPECT_EQ(Release(dst, ro_fh).error, 0);
  EXPECT_EQ(Release(src, in_fh).error, 0);
}

TEST_F(DirCacheFSTest, IoctlForwardsItsAllowlist) {
  WriteFile(Path("f"));
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  int flags = 0;
  {
    const int raw = ::open(Path("f").c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(raw, 0);
    ASSERT_EQ(::ioctl(raw, FS_IOC_GETFLAGS, &flags), 0);
    ::close(raw);
  }
  auto output = [](const Reply &reply) {
    return reply.payload.substr(sizeof(struct fuse_ioctl_out));
  };
  // A get: the backing flags (no open needed: by handle).
  Reply get = Ioctl(f, FS_IOC_GETFLAGS, "", sizeof(int));
  ASSERT_EQ(get.error, 0);
  int got = 0;
  ASSERT_EQ(output(get).size(), sizeof(got));
  std::memcpy(&got, output(get).data(), sizeof(got));
  EXPECT_EQ(got, flags);
  // A set: the flags, and the attributes (ctime) refreshed after.
  const int nodump = flags | FS_NODUMP_FL;
  std::string in(reinterpret_cast<const char *>(&nodump), sizeof(nodump));
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_EQ(Ioctl(f, FS_IOC_SETFLAGS, in, 0).error, 0);
  EXPECT_THAT(Dirty(), Contains(f));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(attr.valid);
  struct stat st {};
  ASSERT_EQ(::stat(Path("f").c_str(), &st), 0);
  EXPECT_EQ(attr.st.st_ctim.tv_sec, st.st_ctim.tv_sec);
  EXPECT_EQ(attr.st.st_ctim.tv_nsec, st.st_ctim.tv_nsec);
  // A directory's (FUSE_IOCTL_DIR), and the generation.
  EXPECT_EQ(Ioctl(d, FS_IOC_GETFLAGS, "", sizeof(int), FUSE_IOCTL_DIR).error,
            0);
  Reply version = Ioctl(f, FS_IOC_GETVERSION, "", sizeof(long));
  ASSERT_EQ(version.error, 0);
  ASSERT_OK_AND_ASSIGN(attr, cache::GetAttr(ctx_, f));
  uint32_t gen = 0;
  std::memcpy(&gen, output(version).data(), sizeof(gen));
  EXPECT_EQ(gen, attr.backing_gen);
  // Not forwarded: ENOTTY; a set the backing filesystem refuses: its errno.
  EXPECT_EQ(Ioctl(f, FS_IOC_GETFSLABEL, "", FSLABEL_MAX).error, -ENOTTY);
  EXPECT_EQ(Ioctl(f, FS_IOC_GETFLAGS, "", sizeof(int), FUSE_IOCTL_COMPAT)
                .error,
            -ENOTTY);
  const int bogus = -1;
  std::string bad(reinterpret_cast<const char *>(&bogus), sizeof(bogus));
  EXPECT_NE(Ioctl(f, FS_IOC_SETFLAGS, bad, 0).error, 0);
  ASSERT_OK_AND_ASSIGN(attr, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(Ioctl(12345, FS_IOC_GETFLAGS, "", sizeof(int)).error, -ESTALE);
  // A change of the casefold flag is refused before the backing
  // filesystem sees it (review M1), whatever it would say.
  const int casefold = got | FS_CASEFOLD_FL;
  std::string cf(reinterpret_cast<const char *>(&casefold), sizeof(casefold));
  EXPECT_EQ(Ioctl(d, FS_IOC_SETFLAGS, cf, 0, FUSE_IOCTL_DIR).error,
            -EOPNOTSUPP);
}

// --- relatime (step 23.3) ---------------------------------------------------
//
// Reads go through passthrough: dcfs never sees them, the backing
// filesystem updates the access time by its mount's rule. A read open
// records in the cache what that rule gives, with no backing I/O.

// Sets path's atime and mtime (seconds before now).
void SetTimes(const std::string &path, int64_t atime_ago, int64_t mtime_ago) {
  struct timespec now {};
  clock_gettime(CLOCK_REALTIME, &now);
  const struct timespec times[2] = {
      {.tv_sec = now.tv_sec - atime_ago, .tv_nsec = 0},
      {.tv_sec = now.tv_sec - mtime_ago, .tv_nsec = 0},
  };
  ASSERT_EQ(::utimensat(AT_FDCWD, path.c_str(), times, 0), 0)
      << path << ": " << std::strerror(errno);
}

class RelatimeTest : public DirCacheFSTest {
 protected:
  // The cached atime of `id` (seconds), which must be current.
  int64_t CachedAtime(InodeId id) {
    absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
    EXPECT_THAT(attr, IsOk());
    if (!attr.ok()) return -1;
    EXPECT_TRUE(attr->valid);
    return attr->st.st_atim.tv_sec;
  }

  // Opens `id` with `flags` and releases it.
  void OpenAndRelease(InodeId id, int flags) {
    auto [open, fh] = Open(id, flags);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(id, fh).error, 0);
  }

  // Remounts the disk the source is on with `flag` (MS_RELATIME,
  // MS_STRICTATIME, MS_NOATIME), and back to relatime at TearDown.
  void RemountSource(unsigned long flag) {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_EQ(::mount(nullptr, tmpdir, nullptr, MS_REMOUNT | flag, nullptr),
              0)
        << std::strerror(errno);
    remounted_ = true;
  }

  void TearDown() override {
    if (remounted_) {
      ::mount(nullptr, std::getenv("TEST_TMPDIR"), nullptr,
              MS_REMOUNT | MS_RELATIME, nullptr);
    }
    DirCacheFSTest::TearDown();
  }

  bool remounted_ = false;
};

TEST_F(RelatimeTest, ReadOpenFollowsTheRelatimeRule) {
  // Setting the times makes the ctime now, and an atime not after the
  // ctime is updated too: "recent" needs one after it (in the future), and
  // within the day.
  WriteFile(Path("old"));     // atime 2 days ago, mtime 3 days ago
  WriteFile(Path("recent"));  // atime in an hour, mtime 2 hours ago
  WriteFile(Path("stale"));   // atime 2 hours ago, mtime 1 hour ago
  SetTimes(Path("old"), 2 * 86400, 3 * 86400);
  SetTimes(Path("recent"), -3600, 7200);
  SetTimes(Path("stale"), 7200, 3600);
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId old, Id("old"));
  ASSERT_OK_AND_ASSIGN(InodeId recent, Id("recent"));
  ASSERT_OK_AND_ASSIGN(InodeId stale, Id("stale"));
  const int64_t recent_before = CachedAtime(recent);
  struct timespec now {};
  clock_gettime(CLOCK_REALTIME, &now);

  OpenAndRelease(old, O_RDONLY);
  OpenAndRelease(recent, O_RDONLY);
  OpenAndRelease(stale, O_RDONLY);
  EXPECT_GE(CachedAtime(old), now.tv_sec);        // A day old.
  EXPECT_EQ(CachedAtime(recent), recent_before);  // After m/ctime, recent.
  EXPECT_GE(CachedAtime(stale), now.tv_sec);      // Not after mtime.

  // Once updated, a second open within the day changes nothing.
  const int64_t first = CachedAtime(old);
  OpenAndRelease(old, O_RDONLY);
  EXPECT_EQ(CachedAtime(old), first);
}

// O_NOATIME, and a writable open (its attributes are unknown until its
// last release, which re-reads them, atime included), record nothing.
TEST_F(RelatimeTest, NoatimeAndWritableOpensLeaveTheAtime) {
  WriteFile(Path("f"));
  SetTimes(Path("f"), 2 * 86400, 3 * 86400);
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const int64_t before = CachedAtime(f);
  OpenAndRelease(f, O_RDONLY | O_NOATIME);
  EXPECT_EQ(CachedAtime(f), before);
  OpenAndRelease(f, O_WRONLY);
  EXPECT_EQ(CachedAtime(f), before);  // The backing's, re-read at release.
}

TEST_F(RelatimeTest, StrictatimeUpdatesOnEveryReadOpen) {
  WriteFile(Path("f"));
  SetTimes(Path("f"), 60, 3600);
  RemountSource(MS_STRICTATIME);
  Start();
  ASSERT_EQ(ctx_.atime, AtimePolicy::kStrict);
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const int64_t before = CachedAtime(f);
  OpenAndRelease(f, O_RDONLY);
  EXPECT_GT(CachedAtime(f), before);
}

TEST_F(RelatimeTest, NoatimeBackingNeverUpdates) {
  WriteFile(Path("f"));
  SetTimes(Path("f"), 2 * 86400, 3 * 86400);
  RemountSource(MS_NOATIME);
  Start();
  ASSERT_EQ(ctx_.atime, AtimePolicy::kNever);
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const int64_t before = CachedAtime(f);
  OpenAndRelease(f, O_RDONLY);
  EXPECT_EQ(CachedAtime(f), before);
}

// copy_file_range into, and ioctls of, a removed object (no row: no
// bookkeeping, through its descriptor).
TEST_F(DirCacheFSTest, RemovedFileCopyAndIoctl) {
  WriteFile(Path("src"));
  AppendToFile(Path("src"), "abc");
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId src, Id("src"));
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  const int held = ::open(Path("f").c_str(), O_PATH | O_CLOEXEC);
  ASSERT_GE(held, 0);
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);

  Reply get = Ioctl(f, FS_IOC_GETFLAGS, "", sizeof(int));
  EXPECT_EQ(get.error, 0);
  auto [in, in_fh] = Open(src, O_RDONLY);
  auto [out, out_fh] = Open(f, O_RDWR);
  ASSERT_EQ(in.error, 0);
  ASSERT_EQ(out.error, 0);
  EXPECT_EQ(CopyFileRange(src, in_fh, f, out_fh, 100), 3);
  struct stat st {};
  ASSERT_EQ(::fstat(held, &st), 0);
  EXPECT_EQ(st.st_size, 3);
  // Set the flags it has (ext4 refuses to clear its extents flag).
  const std::string set = get.payload.substr(sizeof(struct fuse_ioctl_out));
  ASSERT_EQ(set.size(), sizeof(int));
  EXPECT_EQ(Ioctl(f, FS_IOC_SETFLAGS, set, 0).error, 0);
  EXPECT_EQ(Release(f, out_fh).error, 0);
  EXPECT_EQ(Release(src, in_fh).error, 0);
  ::close(held);
}

// A writable open that shares a backing fd opened read-only (the file was
// immutable then) and passes the writability check gets a descriptor that
// can write: fallocate and copy_file_range into it work, as on the backing
// filesystem (review L1; they got EBADF from the read-only shared fd).
TEST_F(DirCacheFSTest, WritableOpenAfterChattrMinusIWritesThroughItsFd) {
  WriteFile(Path("f"));
  WriteFile(Path("src"));
  AppendToFile(Path("src"), "abc");
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId src, Id("src"));
  StartTrace();  // and the files' (formal/reval.tla)
  int flags = 0;
  const int raw = ::open(Path("f").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(raw, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_GETFLAGS, &flags), 0);
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &immutable), 0);
  auto [ro, ro_fh] = Open(f, O_RDONLY);  // Shared fd: read-only.
  ASSERT_EQ(ro.error, 0);
  ASSERT_EQ(::ioctl(raw, FS_IOC_SETFLAGS, &flags), 0);
  OutOfBand(f);
  ::close(raw);
  auto [rw, rw_fh] = Open(f, O_WRONLY);
  ASSERT_EQ(rw.error, 0);

  struct fuse_fallocate_in falloc = {};
  falloc.fh = rw_fh;
  falloc.length = 4096;
  std::string body;
  AppendBytes(body, falloc);
  EXPECT_EQ(Send(FUSE_FALLOCATE, static_cast<uint64_t>(f), body).error, 0);
  EXPECT_EQ(std::filesystem::file_size(Path("f")), 4096u);
  auto [in, in_fh] = Open(src, O_RDONLY);
  ASSERT_EQ(in.error, 0);
  EXPECT_EQ(CopyFileRange(src, in_fh, f, rw_fh, 3), 3);
  for (auto [id, fh] : {std::pair{f, rw_fh}, std::pair{f, ro_fh},
                        std::pair{src, in_fh}}) {
    EXPECT_EQ(Release(id, fh).error, 0);
  }
}

// The descriptors this process has open.
int OpenFdCount() {
  int n = 0;
  for ([[maybe_unused]] const auto &entry :
       std::filesystem::directory_iterator("/proc/self/fd")) {
    ++n;
  }
  return n;
}


// Writable opens sharing a read-only backing descriptor (review L-a): the
// write fd is kept from the first, and replaced only by one without
// O_APPEND, so a later O_APPEND writer does not make the first writer's
// copy_file_range fail (EBADF: the kernel refuses an O_APPEND destination)
// or move its fallback writes to the end of the file.
TEST_F(DirCacheFSTest, AnAppendingWriterKeepsTheFirstWritersFd) {
  WriteFile(Path("f"));
  WriteFile(Path("src"));
  AppendToFile(Path("src"), "abc");
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId src, Id("src"));
  StartTrace();  // and the files' (formal/reval.tla)
  const uint64_t ro_fh = OpenReadOnlySharedFd("f", f);
  ASSERT_NE(ro_fh, 0u);
  auto [a, a_fh] = Open(f, O_WRONLY);
  ASSERT_EQ(a.error, 0);
  auto [b, b_fh] = Open(f, O_WRONLY | O_APPEND);
  ASSERT_EQ(b.error, 0);
  auto [in, in_fh] = Open(src, O_RDONLY);
  ASSERT_EQ(in.error, 0);
  EXPECT_EQ(CopyFileRange(src, in_fh, f, a_fh, 3), 3);
  struct fuse_write_in write = {};
  write.fh = a_fh;
  write.offset = 1;
  write.size = 1;
  std::string body;
  AppendBytes(body, write);
  body.append("x");
  EXPECT_EQ(Send(FUSE_WRITE, static_cast<uint64_t>(f), body).error, 0);
  EXPECT_EQ(ReadWholeFile(Path("f")), "axc");
  for (auto [id, fh] : {std::pair{f, a_fh}, std::pair{f, b_fh},
                        std::pair{f, ro_fh}, std::pair{src, in_fh}}) {
    EXPECT_EQ(Release(id, fh).error, 0);
  }
}

// The write fd goes with the last writable open (review L-a), at its
// release or when the open fails after it was made, while the read-only
// shared descriptor stays for the reader.
TEST_F(DirCacheFSTest, TheWriteFdGoesWithTheLastWriter) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const uint64_t ro_fh = OpenReadOnlySharedFd("f", f);
  ASSERT_NE(ro_fh, 0u);
  const int base = OpenFdCount();
  auto [w, w_fh] = Open(f, O_WRONLY);
  ASSERT_EQ(w.error, 0);
  EXPECT_EQ(OpenFdCount(), base + 1);
  ASSERT_EQ(Release(f, w_fh).error, 0);
  EXPECT_EQ(OpenFdCount(), base) << "after the last writer's release";

  ASSERT_THAT(db_.Exec("PRAGMA query_only = 1"), IsOk());  // Phase 1 fails.
  auto [failed, failed_fh] = Open(f, O_WRONLY);
  ASSERT_THAT(db_.Exec("PRAGMA query_only = 0"), IsOk());
  EXPECT_NE(failed.error, 0);
  EXPECT_EQ(OpenFdCount(), base) << "after a writable open failed";
  EXPECT_EQ(Release(f, ro_fh).error, 0);
}

// --- FORGET reconciliation (step 23.1) ------------------------------------
//
// A store through a shared writable mapping after the last close reaches
// the backing file with no request at all (passthrough's mapping holds
// only the backing file), after the last RELEASE recorded the attributes.
// When the kernel lets go of such an inode (its last FORGET), dcfs re-reads
// its attributes; if they changed, as a mutation (durably dirty first), so
// that a power loss cannot keep the new attributes and lose the stores.
// The harness stands for the stores with writes to the backing file.

TEST_F(DirCacheFSTest, LastForgetOfAWrittenFileReconcilesItsAttributes) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);  // A sync point: f is clean.
  ASSERT_THAT(Dirty(), Not(Contains(f)));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr before, cache::GetAttr(ctx_, f));
  ASSERT_TRUE(before.valid);
  ASSERT_EQ(before.st.st_size, 0);

  // Trace validation (formal/README.md): the reconciliation is a file's
  // mutation and refresh, outside any request, which no directory's trace
  // may show a change for.
  StartTrace();
  AppendToFile(Path("f"), "stored");  // The mapping's stores.
  Forget(f, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr after, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(after.valid);
  EXPECT_EQ(after.st.st_size, 6);
  // Recorded as a mutation records: dirty until a sync point covers it.
  EXPECT_THAT(Dirty(), Contains(f));
  EXPECT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_THAT(Dirty(), Not(Contains(f)));

  // Once is enough: forgotten again (after a new lookup) with no writable
  // open meanwhile, it is not looked at again.
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  AppendToFile(Path("f"), "more");  // Behind dcfs's back now.
  Forget(f, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr later, cache::GetAttr(ctx_, f));
  EXPECT_EQ(later.st.st_size, 6);
  EXPECT_THAT(Dirty(), Not(Contains(f)));
}

// Nothing changed since the last release: one statx, no mutation (the
// inode stays clean), whether the FORGET comes alone or in a batch.
TEST_F(DirCacheFSTest, LastForgetOfAnUnchangedFileChangesNothing) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  auto [lf, ef] = Lookup(kRootInode, "f");
  auto [lg, eg] = Lookup(kRootInode, "g");
  ASSERT_EQ(lf.error, 0);
  ASSERT_EQ(lg.error, 0);
  const InodeId f = static_cast<InodeId>(ef.nodeid);
  const InodeId g = static_cast<InodeId>(eg.nodeid);
  for (InodeId id : {f, g}) {
    auto [open, fh] = Open(id, O_WRONLY);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(id, fh).error, 0);
  }
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
  AppendToFile(Path("g"), "stored");

  BatchForget({{f, 1}, {g, 1}});
  EXPECT_THAT(Dirty(), Not(Contains(f)));
  EXPECT_THAT(Dirty(), Contains(g));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr_g, cache::GetAttr(ctx_, g));
  EXPECT_TRUE(attr_g.valid);
  EXPECT_EQ(attr_g.st.st_size, 6);
}

// A file that is gone from the backing filesystem by the time of its last
// FORGET: the re-read finds its handle stale and forgets the row, as any
// open by handle would.
TEST_F(DirCacheFSTest, LastForgetOfARemovedWrittenFileForgetsItsRow) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_EQ(::unlink(Path("f").c_str()), 0);  // Behind dcfs's back.
  Forget(f, 1);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

// Without a held descriptor (none could be opened at its last close: here
// the descriptor limit), the last FORGET still makes the attributes
// unknown and the inode durably dirty, with no backing I/O; the next
// access re-reads them.
TEST_F(DirCacheFSTest, LastForgetWithoutAHeldDescriptorMarksUnknown) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  struct rlimit saved {};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
  const int lowest_free = ::dup(0);
  ASSERT_GE(lowest_free, 0);
  ::close(lowest_free);
  struct rlimit tight = saved;
  tight.rlim_cur = static_cast<rlim_t>(lowest_free);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &tight), 0);
  Reply release = Release(f, fh);  // Cannot hold a descriptor now.
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);
  ASSERT_EQ(release.error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);  // A sync point: f is clean.
  ASSERT_THAT(Dirty(), Not(Contains(f)));
  Forget(f, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid);
  EXPECT_THAT(Dirty(), Contains(f));
}

// Held descriptors stop at Options::max_held_fds (review M-1), leaving the
// rest of the descriptor limit to requests: with the limit a few above the
// cap, opens of other files still succeed after more files than that were
// written. A written file beyond the cap holds nothing (its last FORGET is
// the phase 1 alone), and a FORGET gives its held descriptor's place back.
TEST_F(DirCacheFSTest, HeldDescriptorsStopAtTheCap) {
  constexpr int kCap = 2;
  constexpr int kWritten = kCap + 6;
  constexpr int kReaders = 3;
  for (int i = 0; i < kWritten; ++i) WriteFile(Path(absl::StrCat("w", i)));
  for (int i = 0; i < kReaders; ++i) WriteFile(Path(absl::StrCat("r", i)));
  WriteFile(Path("later"));
  options_.max_held_fds = kCap;
  Start();
  auto lookup = [&](std::string_view name) {
    auto [reply, entry] = Lookup(kRootInode, name);
    EXPECT_EQ(reply.error, 0) << name;
    return static_cast<InodeId>(entry.nodeid);
  };
  std::vector<InodeId> written, readers;
  for (int i = 0; i < kWritten; ++i) {
    written.push_back(lookup(absl::StrCat("w", i)));
  }
  for (int i = 0; i < kReaders; ++i) {
    readers.push_back(lookup(absl::StrCat("r", i)));
  }
  const InodeId later = lookup("later");

  struct rlimit saved {};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
  const int lowest_free = ::dup(0);
  ASSERT_GE(lowest_free, 0);
  ::close(lowest_free);
  struct rlimit tight = saved;
  tight.rlim_cur = static_cast<rlim_t>(lowest_free + kCap + kReaders + 2);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &tight), 0);
  std::vector<std::pair<InodeId, uint64_t>> opened;
  for (InodeId id : written) {
    auto [open, fh] = Open(id, O_RDWR);
    EXPECT_EQ(open.error, 0);
    if (fh != 0) {
      EXPECT_EQ(Release(id, fh).error, 0);
    }
  }
  for (InodeId id : readers) {
    auto [open, fh] = Open(id, O_RDONLY);
    EXPECT_EQ(open.error, 0) << "open of reader " << id << " with "
                             << kWritten << " files written";
    if (fh != 0) opened.emplace_back(id, fh);
  }
  for (auto [id, fh] : opened) EXPECT_EQ(Release(id, fh).error, 0);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);

  // Within the cap: held, so its FORGET re-reads through it.
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
  Forget(written[0], 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr held, cache::GetAttr(ctx_, written[0]));
  EXPECT_TRUE(held.valid);
  EXPECT_THAT(Dirty(), Not(Contains(written[0])));
  // Beyond it: the phase 1 alone.
  Forget(written[kCap], 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr unheld,
                       cache::GetAttr(ctx_, written[kCap]));
  EXPECT_FALSE(unheld.valid);
  EXPECT_THAT(Dirty(), Contains(written[kCap]));
  // written[0]'s FORGET gave its place back: a file written now is held.
  auto [open, fh] = Open(later, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(later, fh).error, 0);
  AppendToFile(Path("later"), "stored");
  Forget(later, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr refreshed, cache::GetAttr(ctx_, later));
  EXPECT_TRUE(refreshed.valid);
  EXPECT_EQ(refreshed.st.st_size, 6);
}

// The reserve is half the limit, but at least 16Ki and at most 64Ki: a
// container's 64Ki limit still holds half of it.
TEST(DefaultMaxHeldFdsTest, LeavesAReserve) {
  EXPECT_EQ(DirCacheFS::DefaultMaxHeldFds(1024), 0u);
  EXPECT_EQ(DirCacheFS::DefaultMaxHeldFds(16384), 0u);
  EXPECT_EQ(DirCacheFS::DefaultMaxHeldFds(20000), 3616u);
  EXPECT_EQ(DirCacheFS::DefaultMaxHeldFds(65536), 32768u);
  EXPECT_EQ(DirCacheFS::DefaultMaxHeldFds(131072), 65536u);
  EXPECT_EQ(DirCacheFS::DefaultMaxHeldFds(1048576), 983040u);
}

// Collects the log lines at WARNING and above while it lives.
class WarningCapture : public absl::LogSink {
 public:
  WarningCapture() { absl::AddLogSink(this); }
  ~WarningCapture() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    if (entry.log_severity() >= absl::LogSeverity::kWarning) {
      lines.emplace_back(entry.text_message());
    }
  }
  std::vector<std::string> lines;
};

// Holding no descriptor at all (cap 0) disables the workaround: said once,
// at WARNING, when dcfs starts, with what it costs; not again at each
// release.
TEST_F(DirCacheFSTest, NoHeldDescriptorsIsAWarningAtStartup) {
  WriteFile(Path("f"));
  options_.max_held_fds = 0;
  WarningCapture capture;
  Start();
  auto said = [&](std::string_view text) {
    return std::count_if(capture.lines.begin(), capture.lines.end(),
                         [&](const std::string &line) {
                           return absl::StrContains(line, text);
                         });
  };
  EXPECT_EQ(said("no descriptors on written files"), 1)
      << absl::StrJoin(capture.lines, "\n");
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  for (int i = 0; i < 2; ++i) {
    auto [open, fh] = Open(f, O_RDWR);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(f, fh).error, 0);
  }
  EXPECT_EQ(said("hold a descriptor"), 0)
      << absl::StrJoin(capture.lines, "\n");
}

// A held descriptor is closed at the file's last FORGET, when dcfs removes
// the file (the removed record holds its own until the last FORGET), and at
// DESTROY: none outlives what it is held for.
TEST_F(DirCacheFSTest, HeldDescriptorsAreClosed) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  WriteFile(Path("h"));
  Start();
  std::map<std::string, InodeId> ids;
  for (const char *name : {"f", "g", "h"}) {
    auto [reply, entry] = Lookup(kRootInode, name);
    ASSERT_EQ(reply.error, 0);
    ids[name] = static_cast<InodeId>(entry.nodeid);
  }
  auto write = [&](InodeId id) {
    auto [open, fh] = Open(id, O_RDWR);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(id, fh).error, 0);
  };
  const int base = OpenFdCount();
  write(ids["f"]);
  EXPECT_EQ(OpenFdCount(), base + 1);
  Forget(ids["f"], 1);
  EXPECT_EQ(OpenFdCount(), base) << "after the last FORGET";

  write(ids["g"]);
  ASSERT_EQ(Unlink(kRootInode, "g").error, 0);
  EXPECT_EQ(OpenFdCount(), base + 1) << "the removed record's alone";
  Forget(ids["g"], 1);
  EXPECT_EQ(OpenFdCount(), base) << "after the removed file's last FORGET";

  write(ids["h"]);
  EXPECT_EQ(OpenFdCount(), base + 1);
  EXPECT_EQ(Send(FUSE_DESTROY, 0, "").error, 0);
  EXPECT_EQ(OpenFdCount(), base) << "after DESTROY";
}

// Counts phase 1s: each is a transaction, durable (a WAL fsync) unless
// every inode it names is already durably dirty.
class CountMutations : public ProtocolEvents {
 public:
  void MutationBegun(Context &, events::IdsFn ids, bool synced) override {
    ++begun;
    if (synced) ++synced_begun;
    ids([&](InodeId) { ++ids_named; });
  }
  int begun = 0;
  int synced_begun = 0;
  int ids_named = 0;
};

// The written files of one FORGET batch (or of DESTROY) that hold no
// descriptor -- or whose attributes changed -- share one phase 1, not one
// durable transaction each (review M-1).
TEST_F(DirCacheFSTest, ForgetBatchReconcilesInOnePhase1) {
  options_.max_held_fds = 0;
  const std::vector<std::string> names = {"a", "b", "c", "d", "e", "f"};
  for (const std::string &name : names) WriteFile(Path(name));
  Start();
  std::vector<InodeId> ids;
  for (const std::string &name : names) {
    auto [reply, entry] = Lookup(kRootInode, name);
    ASSERT_EQ(reply.error, 0);
    const InodeId id = static_cast<InodeId>(entry.nodeid);
    ids.push_back(id);
    auto [open, fh] = Open(id, O_RDWR);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(id, fh).error, 0);
  }
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);  // A sync point: all clean.
  CountMutations count;
  ctx_.events = &count;
  BatchForget({{ids[0], 1}, {ids[1], 1}, {ids[2], 1}});
  EXPECT_EQ(count.begun, 1);
  EXPECT_EQ(count.synced_begun, 1);
  EXPECT_EQ(count.ids_named, 3);
  // DESTROY likewise, for the rest.
  EXPECT_EQ(Send(FUSE_DESTROY, 0, "").error, 0);
  EXPECT_EQ(count.begun, 2);
  EXPECT_EQ(count.ids_named, 6);
  ctx_.events = &NoProtocolEvents();
  for (InodeId id : ids) {
    ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, id));
    EXPECT_FALSE(attr.valid) << id;
    EXPECT_THAT(Dirty(), Contains(id));
  }
}

// A file written again after its last close keeps the descriptor it
// holds (no reopen): one that cannot be opened now (EMFILE) does not cost
// the one it has.
TEST_F(DirCacheFSTest, WrittenAgainKeepsItsHeldDescriptor) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [first, first_fh] = Open(f, O_RDWR);
  ASSERT_EQ(first.error, 0);
  ASSERT_EQ(Release(f, first_fh).error, 0);  // Held from here.
  auto [again, again_fh] = Open(f, O_RDWR);
  ASSERT_EQ(again.error, 0);
  struct rlimit saved {};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
  const int lowest_free = ::dup(0);
  ASSERT_GE(lowest_free, 0);
  ::close(lowest_free);
  struct rlimit tight = saved;
  tight.rlim_cur = static_cast<rlim_t>(lowest_free);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &tight), 0);
  Reply release = Release(f, again_fh);  // No new descriptor possible.
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);
  ASSERT_EQ(release.error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
  AppendToFile(Path("f"), "stored");
  Forget(f, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 6);
}

// At unmount the kernel sends no FORGETs: a written file the kernel still
// held is reconciled at DESTROY instead.
TEST_F(DirCacheFSTest, DestroyReconcilesWrittenFiles) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);
  AppendToFile(Path("f"), "stored");
  EXPECT_EQ(Send(FUSE_DESTROY, 0, "").error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr after, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(after.valid);
  EXPECT_EQ(after.st.st_size, 6);
  EXPECT_THAT(Dirty(), Contains(f));
}

// --- Changing removed objects (step 23.2) ---------------------------------
//
// An object removed while the kernel still holds its nodeid (an O_PATH
// descriptor on an unlinked file, a removed working directory) has no row;
// its removed_ record answers the kernel. Changes to it are applied through
// the descriptor the record holds, and every read of it goes to that
// descriptor, so nothing cached can be stale after one.

TEST_F(DirCacheFSTest, RemovedFileCanBeChanged) {
  WriteFile(Path("f"));
  AppendToFile(Path("f"), "hello");
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  // Keep the object alive as the kernel's reference would (the record
  // holds its own descriptor; this one lets the test look at the object).
  const int held = ::open(Path("f").c_str(), O_PATH | O_CLOEXEC);
  ASSERT_GE(held, 0);
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);
  ASSERT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));

  EXPECT_EQ(Chmod(f, S_IFREG | 0600).error, 0);
  struct fuse_setattr_in truncate = {};
  truncate.valid = FATTR_SIZE;
  truncate.size = 2;
  std::string body;
  AppendBytes(body, truncate);
  EXPECT_EQ(Send(FUSE_SETATTR, static_cast<uint64_t>(f), body).error, 0);
  EXPECT_EQ(Setxattr(f, "user.k", "v").error, 0);
  struct stat st {};
  ASSERT_EQ(::fstat(held, &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 0600u);
  EXPECT_EQ(st.st_size, 2);
  EXPECT_EQ(st.st_nlink, 0u);
  auto [getattr, attr] = Getattr(f);
  ASSERT_EQ(getattr.error, 0);
  EXPECT_EQ(attr.mode & 07777, 0600u);
  EXPECT_EQ(attr.size, 2u);
  Reply value = Getxattr(f, "user.k");
  ASSERT_EQ(value.error, 0);
  struct fuse_getxattr_out size {};
  ASSERT_GE(value.payload.size(), sizeof(size));
  std::memcpy(&size, value.payload.data(), sizeof(size));
  EXPECT_EQ(size.size, 1u);

  std::string name = "user.k";
  name.push_back('\0');
  EXPECT_EQ(Send(FUSE_REMOVEXATTR, static_cast<uint64_t>(f), name).error, 0);
  EXPECT_EQ(Getxattr(f, "user.k").error, -ENODATA);

  // A change the backing filesystem refuses is replied as it refused it.
  struct fuse_setxattr_in replace = {};
  replace.size = 1;
  replace.flags = XATTR_REPLACE;
  std::string replace_body(reinterpret_cast<const char *>(&replace),
                           FUSE_COMPAT_SETXATTR_IN_SIZE);
  replace_body.append("user.absent");
  replace_body.push_back('\0');
  replace_body.append("v");
  EXPECT_EQ(Send(FUSE_SETXATTR, static_cast<uint64_t>(f), replace_body).error,
            -ENODATA);

  // Reopened (an open of /proc/<pid>/fd/<n>): through the held descriptor.
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  EXPECT_TRUE(fs_->HasOpenFiles(f));
  struct fuse_fsync_in fsync_in = {};
  fsync_in.fh = fh;
  std::string fsync_body;
  AppendBytes(fsync_body, fsync_in);
  EXPECT_EQ(Send(FUSE_FSYNC, static_cast<uint64_t>(f), fsync_body).error, 0);
  EXPECT_EQ(Release(f, fh).error, 0);
  EXPECT_FALSE(fs_->HasOpenFiles(f));
  // Still answered after the release, until the kernel forgets it.
  EXPECT_EQ(Getattr(f).first.error, 0);
  Forget(f, 1);
  EXPECT_EQ(Getattr(f).first.error, -ESTALE);
  EXPECT_EQ(Chmod(f, S_IFREG | 0644).error, -ESTALE);
  ::close(held);
}

TEST_F(DirCacheFSTest, RemovedDirectoryCanBeChanged) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "d");
  ASSERT_EQ(lookup.error, 0);
  const InodeId d = static_cast<InodeId>(entry.nodeid);
  const int held = ::open(Path("d").c_str(), O_RDONLY | O_DIRECTORY);
  ASSERT_GE(held, 0);
  std::string body = "d";
  body.push_back('\0');
  ASSERT_EQ(Send(FUSE_RMDIR, kRootInode, body).error, 0);

  EXPECT_EQ(Chmod(d, S_IFDIR | 0700).error, 0);
  EXPECT_EQ(Setxattr(d, "user.k", "v").error, 0);
  struct stat st {};
  ASSERT_EQ(::fstat(held, &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 0700u);
  auto [getattr, attr] = Getattr(d);
  ASSERT_EQ(getattr.error, 0);
  EXPECT_EQ(attr.mode & 07777, 0700u);
  // A truncate of a directory: the backing filesystem's EISDIR.
  struct fuse_setattr_in truncate = {};
  truncate.valid = FATTR_SIZE;
  std::string truncate_body;
  AppendBytes(truncate_body, truncate);
  EXPECT_EQ(Send(FUSE_SETATTR, static_cast<uint64_t>(d), truncate_body).error,
            -EISDIR);
  // fsync of the removed directory (through an OPENDIR'd handle).
  ASSERT_EQ(Opendir(d).error, 0);
  EXPECT_EQ(Fsyncdir(d).error, 0);
  ::close(held);
}

// --- Boundary stubs (step 23.5) -------------------------------------------
//
// A mount point or subvolume boundary below the source is served as a stub
// directory: listed, looked up as a directory with a nodeid at or above
// 2^63 (as its inode number too), and anything inside it ENOTSUP; renaming
// it, or a rename or link into it, EXDEV.

constexpr uint64_t kFirstStubNodeid = uint64_t{1} << 63;

TEST_F(DirCacheFSTest, BoundaryIsAStubDirectory) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(Path("d/mp").c_str(), 0755), 0);
  WriteFile(Path("d/f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  MountBelow("d/mp");  // After d's row, before its listing.
  WriteFile(Path("d/mp/inside"));

  EXPECT_THAT(List(d, false),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "f", "mp")));
  EXPECT_THAT(List(d, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "f", "mp")));
  auto [lookup, entry] = Lookup(d, "mp");
  ASSERT_EQ(lookup.error, 0);
  EXPECT_GE(entry.nodeid, kFirstStubNodeid);
  EXPECT_EQ(entry.attr.ino, entry.nodeid);
  EXPECT_NE(entry.generation, 0u);
  EXPECT_TRUE(S_ISDIR(entry.attr.mode));
  EXPECT_EQ(entry.attr.mode & 07777, 0751u);  // The tmpfs root's.
  const InodeId stub = static_cast<InodeId>(entry.nodeid);

  // The same stub every time.
  auto [again, entry_again] = Lookup(d, "mp");
  ASSERT_EQ(again.error, 0);
  EXPECT_EQ(entry_again.nodeid, entry.nodeid);
  EXPECT_EQ(entry_again.generation, entry.generation);
  auto [getattr, attr] = Getattr(stub);
  ASSERT_EQ(getattr.error, 0);
  EXPECT_EQ(attr.ino, entry.nodeid);
  EXPECT_TRUE(S_ISDIR(attr.mode));
  auto [dot, dot_entry] = Lookup(stub, ".");
  ASSERT_EQ(dot.error, 0);
  EXPECT_EQ(dot_entry.nodeid, entry.nodeid);
  auto [dotdot, dotdot_entry] = Lookup(stub, "..");
  ASSERT_EQ(dotdot.error, 0);
  EXPECT_EQ(dotdot_entry.nodeid, static_cast<uint64_t>(d));

  // Anything inside: ENOTSUP.
  EXPECT_EQ(Lookup(stub, "inside").first.error, -ENOTSUP);
  EXPECT_EQ(Opendir(stub).error, -ENOTSUP);
  EXPECT_EQ(Mkdir(stub, "x").first.error, -ENOTSUP);
  EXPECT_EQ(Create(stub, "x", O_RDWR | O_CREAT).reply.error, -ENOTSUP);
  EXPECT_EQ(Unlink(stub, "inside").error, -ENOTSUP);
  EXPECT_EQ(Chmod(stub, S_IFDIR | 0700).error, -ENOTSUP);
  EXPECT_EQ(Setxattr(stub, "user.x", "v").error, -ENOTSUP);
  // No xattrs (the kernel asks for its ACLs in permission checks).
  EXPECT_EQ(Getxattr(stub, "system.posix_acl_access").error, -ENODATA);
  // Across it: EXDEV.
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f", d));
  EXPECT_EQ(Rename(d, "f", stub, "f").error, -EXDEV);
  EXPECT_EQ(Rename(stub, "inside", d, "g").error, -EXDEV);
  EXPECT_EQ(Rename(d, "mp", d, "mp2").error, -EXDEV);
  EXPECT_EQ(Link(f, stub, "f").error, -EXDEV);

  // Nothing reached either side of the boundary.
  EXPECT_EQ(::access(Path("d/mp/inside").c_str(), F_OK), 0);
  EXPECT_NE(::access(Path("d/mp/x").c_str(), F_OK), 0);
  EXPECT_NE(::access(Path("d/mp/f").c_str(), F_OK), 0);
  EXPECT_EQ(::access(Path("d/f").c_str(), F_OK), 0);
  EXPECT_NE(::access(Path("d/mp2").c_str(), F_OK), 0);
}

// Every refused operation on a stub, with its errno (review L2): removing
// it is EBUSY (as for a mount point), linking it EXDEV, and anything
// inside it ENOTSUP.
TEST_F(DirCacheFSTest, EveryOperationOnAStubIsRefused) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(Path("d/mp").c_str(), 0755), 0);
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  MountBelow("d/mp");
  auto [lookup, entry] = Lookup(d, "mp");
  ASSERT_EQ(lookup.error, 0);
  const InodeId stub = static_cast<InodeId>(entry.nodeid);
  auto name = [](std::string_view n) {
    std::string body(n);
    body.push_back('\0');
    return body;
  };
  EXPECT_EQ(Send(FUSE_RMDIR, static_cast<uint64_t>(d), name("mp")).error,
            -EBUSY);
  EXPECT_EQ(Unlink(d, "mp").error, -EBUSY);
  EXPECT_EQ(Link(stub, d, "mp2").error, -EXDEV);
  EXPECT_EQ(Send(FUSE_READLINK, static_cast<uint64_t>(stub), "").error,
            -ENOTSUP);
  struct fuse_mknod_in mknod = {};
  mknod.mode = S_IFIFO | 0644;
  std::string mknod_body;
  AppendBytes(mknod_body, mknod);
  mknod_body.append(name("fifo"));
  EXPECT_EQ(Send(FUSE_MKNOD, static_cast<uint64_t>(stub), mknod_body).error,
            -ENOTSUP);
  EXPECT_EQ(Send(FUSE_SYMLINK, static_cast<uint64_t>(stub),
                 name("link") + name("target"))
                .error,
            -ENOTSUP);
  EXPECT_EQ(Send(FUSE_RMDIR, static_cast<uint64_t>(stub), name("x")).error,
            -ENOTSUP);
  EXPECT_EQ(ErrnoOf(List(stub, false).status()), ENOTSUP);
  EXPECT_EQ(ErrnoOf(List(stub, true).status()), ENOTSUP);
  EXPECT_EQ(Fsyncdir(stub).error, -ENOTSUP);
  EXPECT_EQ(Send(FUSE_REMOVEXATTR, static_cast<uint64_t>(stub), name("user.x"))
                .error,
            -ENOTSUP);
  EXPECT_EQ(Ioctl(stub, FS_IOC_GETFLAGS, "", sizeof(int), FUSE_IOCTL_DIR)
                .error,
            -ENOTTY);
  EXPECT_EQ(Tmpfile(stub, O_RDWR).reply.error, -ENOTSUP);
  // Its reads are answered.
  EXPECT_EQ(Send(FUSE_STATFS, static_cast<uint64_t>(stub), "").error, 0);
  struct fuse_getxattr_in list = {};
  std::string list_body;
  AppendBytes(list_body, list);
  EXPECT_EQ(Send(FUSE_LISTXATTR, static_cast<uint64_t>(stub), list_body).error,
            0);
  EXPECT_EQ(::access(Path("d/mp").c_str(), F_OK), 0);
}

// A stub whose row is gone (its dentry relisted or recovered) is a stale
// nodeid: ESTALE, so the kernel's path walk retries with LOOKUP_REVAL and
// finds what the name is now, rather than ENOTSUP (review L3).
TEST_F(DirCacheFSTest, AGoneStubIsStale) {
  ASSERT_EQ(::mkdir(Path("mp").c_str(), 0755), 0);
  Start();
  MountBelow("mp");
  auto [lookup, entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(lookup.error, 0);
  const InodeId stub = static_cast<InodeId>(entry.nodeid);
  ASSERT_THAT(cache::MarkUnknown(ctx_, kRootInode,
                                 std::vector<std::string>{"mp"}),
              IsOk());
  EXPECT_EQ(Lookup(stub, "x").first.error, -ESTALE);
  EXPECT_EQ(Opendir(stub).error, -ESTALE);
  EXPECT_EQ(Mkdir(stub, "x").first.error, -ESTALE);
  EXPECT_EQ(Rename(kRootInode, "a", stub, "a").error, -ESTALE);
}

// The stub's nodeid is recorded with its dentry: a lookup resolved by a
// single probe (an unknown name in a complete listing) gets a stub, and a
// name that is no longer a boundary drops it (the old nodeid is stale).
TEST_F(DirCacheFSTest, BoundaryStubIsRecordedWithItsDentry) {
  ASSERT_EQ(::mkdir(Path("mp").c_str(), 0755), 0);
  WriteFile(Path("a"));
  Start();
  MountBelow("mp");
  ASSERT_THAT(Id("a"), IsOk());  // Populates the root.
  auto [first, first_entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(first.error, 0);
  ASSERT_GE(first_entry.nodeid, kFirstStubNodeid);

  // Forgotten and probed again: the same stub, refreshed in place.
  ASSERT_THAT(cache::MarkUnknown(ctx_, kRootInode,
                                 std::vector<std::string>{"mp"}),
              IsOk());
  ASSERT_EQ(Cached(kRootInode, "mp").first, LookupResult::kUnknown);
  auto [second, second_entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(second.error, 0);
  EXPECT_GE(second_entry.nodeid, kFirstStubNodeid);
  EXPECT_EQ(Cached(kRootInode, "mp").first, LookupResult::kRefused);

  // Once the name is no longer a boundary, the stub goes with the refusal.
  ASSERT_EQ(::umount2(Path("mp").c_str(), MNT_DETACH), 0);
  mounts_below_.clear();
  ASSERT_THAT(cache::MarkDirComplete(ctx_, kRootInode, false), IsOk());
  ASSERT_THAT(cache::ForgetNegativeDentries(ctx_, kRootInode), IsOk());
  auto [plain, plain_entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(plain.error, 0);
  EXPECT_LT(plain_entry.nodeid, kFirstStubNodeid);
  EXPECT_EQ(plain_entry.attr.ino, InoOf(Path("mp")));
  // The old stub's nodeid is stale now.
  EXPECT_EQ(Getattr(static_cast<InodeId>(second_entry.nodeid)).first.error,
            -ESTALE);
}

// Backing inode numbers at or above 2^63 are the stubs' (and, from Phase
// 14, nodeids are backing inode numbers): an object with one is refused,
// ENOTSUP, not served under a nodeid a stub may hold.
TEST_F(DirCacheFSTest, BackingInodeNumbersInTheStubRangeAreRefused) {
  ASSERT_EQ(::mkdir(Path("d").c_str(), 0755), 0);
  WriteFile(Path("d/big"));
  WriteFile(Path("d/small"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  FakeInodeNumbers()[InoOf(Path("d/big"))] = kFirstStubNodeid + 5;
  // Listing d probes every name, so the whole listing is refused (and
  // recorded as nothing: neither name is cached absent).
  EXPECT_EQ(Lookup(d, "big").first.error, -ENOTSUP);
  EXPECT_EQ(Lookup(d, "small").first.error, -ENOTSUP);
  EXPECT_EQ(ErrnoOf(List(d, false).status()), ENOTSUP);
  EXPECT_EQ(Cached(d, "big").first, LookupResult::kUnknown);
  EXPECT_EQ(Cached(d, "small").first, LookupResult::kUnknown);

  // A number below the range is served again.
  FakeInodeNumbers().clear();
  auto [lookup, entry] = Lookup(d, "big");
  ASSERT_EQ(lookup.error, 0);
  EXPECT_EQ(entry.attr.ino, InoOf(Path("d/big")));
}

}  // namespace
}  // namespace dcfs
