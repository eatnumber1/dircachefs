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
// binary and checks every trace against the models (formal/Trace.tla, and
// RevalTrace.tla and LifetimeTrace.tla for files and nodeids). The
// interleavings the hooks make are the reason to: they are the ones the
// model checks and today's single thread never produces.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include "dcfs/dir_cache_fs.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/time/simulated_clock.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_ops.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/mount_options.h"
#include "dcfs/sqlite.h"
#include "dcfs/protocol_events.h"
#include "dcfs/session_loop.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/dir_cache_fs_peer.h"
#include "dcfs/testonly/files.h"
#include "dcfs/testonly/cost_counter.h"
#include "dcfs/testonly/invariant_checker.h"
#include "dcfs/testonly/observers.h"
#include "dcfs/testonly/trace_recorder.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"
#include "gmock/gmock.h"
#include "gtest/gtest-spi.h"
#include "gtest/gtest.h"
#include "sqlite3.h"

namespace dcfs {
namespace {

// The errnos the next open_by_handle_at calls fail with, one per call, in
// order (0: that call is not failed).
std::deque<int> &OpenByHandleFailures() {
  static auto *errs = new std::deque<int>();
  return *errs;
}

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

// The errno the next name_to_handle_at fails with, once (0: none).
int &NameToHandleFailure() {
  static int err = 0;
  return err;
}

// While set, every fstatvfs reports the filesystem read-only (ST_RDONLY):
// a backing filesystem that went read-only by itself after an error.
bool &StatvfsReadOnly() {
  static bool read_only = false;
  return read_only;
}

// The hook the next statx runs (once).
std::function<void()> &StatxHook() {
  static auto *hook = new std::function<void()>();
  return *hook;
}

// A fake kernel passthrough (the harness's session has no FUSE device, so
// libfuse's fuse_passthrough_open fails with ENOTTY and every open is a
// fallback open): while `enabled`, an open is granted the next backing id and
// a close is recorded, failing with EBADF if `close_fails`.
struct FakePassthrough {
  bool enabled = false;
  bool close_fails = false;
  int next_id = 100;
  std::vector<int> closed;
};
FakePassthrough &Passthrough() {
  static auto *fake = new FakePassthrough();
  return *fake;
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

// The harness's cache database while a test runs (Start to TearDown), for
// NoTransactionAt.
::sqlite3 *&HarnessDb() {
  static ::sqlite3 *db = nullptr;
  return db;
}

// Step 26.2: the backstop under the invariant checks' hooks
// (ProtocolEvents::BackingCall). Every libc call through which backing.cc,
// file_handle.cc and device_id.cc reach the backing filesystem is wrapped
// below and calls this first: one made while a transaction or a statement
// cursor is open aborts, whether or not its call site called the hook, so
// a backing syscall added without a hook fails here. Not wrapped (SQLite
// makes them itself, inside its transactions): open, close, pread,
// pwrite, fsync, fdatasync, ftruncate, fstat, fcntl; nor the variadic
// ioctl and syscall (getdents64).
void NoTransactionAt(const char *call) {
  ::sqlite3 *db = HarnessDb();
  if (db == nullptr) return;
  if (sqlite3_get_autocommit(db) == 0) {
    LOG(FATAL) << "invariant violated: no-transaction-at-backing-call (the "
                  "harness's backstop): "
               << call << " while a transaction is open";
  }
  for (sqlite3_stmt *stmt = sqlite3_next_stmt(db, nullptr); stmt != nullptr;
       stmt = sqlite3_next_stmt(db, stmt)) {
    if (sqlite3_stmt_busy(stmt) != 0) {
      LOG(FATAL) << "invariant violated: no-transaction-at-backing-call (the "
                    "harness's backstop): "
                 << call << " while a statement is part way through its rows: "
                 << sqlite3_sql(stmt);
    }
  }
}

// Step 26.6: the fault sweep's state (FaultSitesTest). A backing call site
// is a hook's source location (ProtocolEvents::BackingCall) and one
// wrapped libc call after it, before the next hook: the j-th call of that
// name there (a probe's openat, then its reopen's). Recording, it lists the
// sites a workload reaches, over every time it reaches each hook (so the
// list does not depend on which name a listing probes first); injecting,
// it fails one site the first time it is reached.
struct FaultSweep {
  struct Site {
    std::string hook;
    std::string call;
    int j = 0;
  };
  bool recording = false;
  std::vector<Site> sites;
  std::vector<std::string> hooks;  // every hook location reached
  // Since the last hook: where it is, and how many calls of each name.
  std::string at;
  std::map<std::string, int> seen;
  // Injecting: fail `target` with `err`, once.
  std::optional<Site> target;
  int err = 0;
  std::string fired;  // The call that failed ("" until one has).
};

FaultSweep &Sweep() {
  static auto *sweep = new FaultSweep();
  return *sweep;
}

// The checker's observer of every backing call's hook.
void SweepAtHook(absl::SourceLocation where) {
  FaultSweep &f = Sweep();
  f.at = absl::StrCat(where.file_name(), ":", where.line());
  f.seen.clear();
  if (f.recording &&
      std::find(f.hooks.begin(), f.hooks.end(), f.at) == f.hooks.end()) {
    f.hooks.push_back(f.at);
  }
}

// How many wrapped libc calls (the backing filesystem's, see
// NoTransactionAt) were made: the slope tests' backing syscalls.
int64_t &WrappedCalls() {
  static int64_t calls = 0;
  return calls;
}

// In every wrapped libc call: the errno to fail it with, or 0.
int InjectedFault(const char *call) {
  ++WrappedCalls();
  FaultSweep &f = Sweep();
  if (f.at.empty()) return 0;  // Not after a hook (the test's own calls).
  const int j = f.seen[call]++;
  if (f.recording) {
    bool known = false;
    for (const FaultSweep::Site &s : f.sites) {
      known = known || (s.hook == f.at && s.call == call && s.j == j);
    }
    if (!known) f.sites.push_back({.hook = f.at, .call = call, .j = j});
  }
  if (!f.target.has_value() || !f.fired.empty()) return 0;
  if (f.target->hook != f.at || f.target->call != call || f.target->j != j) {
    return 0;
  }
  f.fired = call;
  return f.err;
}

}  // namespace
}  // namespace dcfs

// A wrap fails with the sweep's errno when it says so (step 26.6).
#define DCFS_INJECT(call, failed)                               \
  if (int injected = dcfs::InjectedFault(call); injected != 0) { \
    errno = injected;                                           \
    return failed;                                              \
  }

extern "C" {
int __real_fuse_passthrough_open(fuse_req_t req, int fd);
int __wrap_fuse_passthrough_open(fuse_req_t req, int fd) {
  dcfs::FakePassthrough &fake = dcfs::Passthrough();
  if (!fake.enabled) return __real_fuse_passthrough_open(req, fd);
  return fake.next_id++;
}
int __real_fuse_passthrough_close(fuse_req_t req, int backing_id);
int __wrap_fuse_passthrough_close(fuse_req_t req, int backing_id) {
  dcfs::FakePassthrough &fake = dcfs::Passthrough();
  if (!fake.enabled) return __real_fuse_passthrough_close(req, backing_id);
  fake.closed.push_back(backing_id);
  if (fake.close_fails) {
    errno = EBADF;
    return -1;
  }
  return 0;
}
int __real_open_by_handle_at(int mount_fd, struct file_handle *handle,
                             int flags);
int __wrap_open_by_handle_at(int mount_fd, struct file_handle *handle,
                             int flags) {
  dcfs::NoTransactionAt("open_by_handle_at");
  DCFS_INJECT("open_by_handle_at", -1)
  std::function<void()> hook = std::exchange(dcfs::OpenByHandleHook(), {});
  if (hook) hook();
  if (!dcfs::OpenByHandleFailures().empty()) {
    const int err = dcfs::OpenByHandleFailures().front();
    dcfs::OpenByHandleFailures().pop_front();
    if (err != 0) {
      errno = err;
      return -1;
    }
  }
  return __real_open_by_handle_at(mount_fd, handle, flags);
}
int __real_name_to_handle_at(int dirfd, const char *pathname,
                             struct file_handle *handle, int *mount_id,
                             int flags);
int __wrap_name_to_handle_at(int dirfd, const char *pathname,
                             struct file_handle *handle, int *mount_id,
                             int flags) {
  dcfs::NoTransactionAt("name_to_handle_at");
  DCFS_INJECT("name_to_handle_at", -1)
  std::function<void()> hook = std::exchange(dcfs::NameToHandleHook(), {});
  if (hook) hook();
  if (int err = std::exchange(dcfs::NameToHandleFailure(), 0); err != 0) {
    errno = err;
    return -1;
  }
  return __real_name_to_handle_at(dirfd, pathname, handle, mount_id, flags);
}
int __real_statx(int dirfd, const char *path, int flags, unsigned int mask,
                 struct statx *buf);
int __wrap_statx(int dirfd, const char *path, int flags, unsigned int mask,
                 struct statx *buf) {
  dcfs::NoTransactionAt("statx");
  if (std::function<void()> hook = std::exchange(dcfs::StatxHook(), {}); hook) {
    hook();
  }
  DCFS_INJECT("statx", -1)
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
  dcfs::NoTransactionAt("syncfs");
  DCFS_INJECT("syncfs", -1)
  std::function<void()> hook = std::exchange(dcfs::SyncfsHook(), {});
  if (hook) hook();
  return __real_syncfs(fd);
}

// The rest of the backstop's wraps (see dcfs::NoTransactionAt): each checks,
// then makes the real call.
#define DCFS_BACKSTOP(ret, name, params, args) \
  ret __real_##name params;                    \
  ret __wrap_##name params {                   \
    dcfs::NoTransactionAt(#name);              \
    DCFS_INJECT(#name, -1)                     \
    return __real_##name args;                 \
  }
DCFS_BACKSTOP(int, unlinkat, (int d, const char *p, int f), (d, p, f))
DCFS_BACKSTOP(int, renameat2,
              (int od, const char *op, int nd, const char *np, unsigned f),
              (od, op, nd, np, f))
DCFS_BACKSTOP(int, mkdirat, (int d, const char *p, mode_t m), (d, p, m))
DCFS_BACKSTOP(int, mknodat, (int d, const char *p, mode_t m, dev_t r),
              (d, p, m, r))
DCFS_BACKSTOP(int, symlinkat, (const char *t, int d, const char *p),
              (t, d, p))
DCFS_BACKSTOP(int, linkat,
              (int od, const char *op, int nd, const char *np, int f),
              (od, op, nd, np, f))
DCFS_BACKSTOP(int, fchownat, (int d, const char *p, uid_t u, gid_t g, int f),
              (d, p, u, g, f))
DCFS_BACKSTOP(int, fchmodat, (int d, const char *p, mode_t m, int f),
              (d, p, m, f))
DCFS_BACKSTOP(int, utimensat,
              (int d, const char *p, const struct timespec t[2], int f),
              (d, p, t, f))
DCFS_BACKSTOP(int, futimens, (int d, const struct timespec t[2]), (d, t))
DCFS_BACKSTOP(ssize_t, readlinkat, (int d, const char *p, char *b, size_t n),
              (d, p, b, n))
DCFS_BACKSTOP(ssize_t, fgetxattr, (int d, const char *n, void *v, size_t s),
              (d, n, v, s))
DCFS_BACKSTOP(ssize_t, flistxattr, (int d, char *l, size_t s), (d, l, s))
DCFS_BACKSTOP(int, fsetxattr,
              (int d, const char *n, const void *v, size_t s, int f),
              (d, n, v, s, f))
DCFS_BACKSTOP(int, fremovexattr, (int d, const char *n), (d, n))
DCFS_BACKSTOP(ssize_t, getxattr,
              (const char *p, const char *n, void *v, size_t s), (p, n, v, s))
DCFS_BACKSTOP(ssize_t, listxattr, (const char *p, char *l, size_t s),
              (p, l, s))
DCFS_BACKSTOP(int, setxattr,
              (const char *p, const char *n, const void *v, size_t s, int f),
              (p, n, v, s, f))
DCFS_BACKSTOP(int, removexattr, (const char *p, const char *n), (p, n))
DCFS_BACKSTOP(int, fstatat64,
              (int d, const char *p, struct stat64 *b, int f), (d, p, b, f))
DCFS_BACKSTOP(int, fstatfs64, (int d, struct statfs64 *b), (d, b))
int __real_fstatvfs64(int d, struct statvfs64 *b);
int __wrap_fstatvfs64(int d, struct statvfs64 *b) {
  dcfs::NoTransactionAt("fstatvfs64");
  DCFS_INJECT("fstatvfs64", -1)
  int ret = __real_fstatvfs64(d, b);
  if (ret == 0 && dcfs::StatvfsReadOnly()) b->f_flag |= ST_RDONLY;
  return ret;
}
DCFS_BACKSTOP(int, fallocate64, (int d, int m, off64_t o, off64_t l),
              (d, m, o, l))
DCFS_BACKSTOP(ssize_t, copy_file_range,
              (int i, loff_t *oi, int o, loff_t *oo, size_t l, unsigned f),
              (i, oi, o, oo, l, f))
#undef DCFS_BACKSTOP

// openat is variadic (the mode, with O_CREAT or O_TMPFILE).
int __real_openat64(int dirfd, const char *path, int flags, ...);
int __wrap_openat64(int dirfd, const char *path, int flags, ...) {
  dcfs::NoTransactionAt("openat");
  DCFS_INJECT("openat", -1)
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  return __real_openat64(dirfd, path, flags, mode);
}
}  // extern "C"

namespace dcfs {
namespace {


using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
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
  ASSERT_THAT(syscalls::openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC,
                               0644),
              IsOk())
      << path;  // the descriptor closes at once
}

// A write the kernel would make through a passthrough fd, which dcfs never
// sees.
std::string ReadWholeFile(const std::string &path) {
  absl::StatusOr<std::string> contents = testonly::ReadFileToString(path);
  EXPECT_THAT(contents, IsOk()) << path;
  return contents.value_or("");
}

void AppendToFile(const std::string &path, std::string_view data) {
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       syscalls::openat(AT_FDCWD, path, O_WRONLY | O_APPEND));
  EXPECT_THAT(syscalls::write(*fd, data.data(), data.size()),
              IsOkAndHolds(data.size()));
}

uint64_t InoOf(const std::string &path) {
  absl::StatusOr<struct stat> st =
      syscalls::fstatat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW);
  EXPECT_THAT(st, IsOk()) << path;
  return st.ok() ? st->st_ino : 0;
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
    ASSERT_OK_AND_ASSIGN(source_, syscalls::mkdtemp(templ));
  }

  // Starts the filesystem over the source tree built so far: a fresh
  // cache, a DirCacheFS, and a libfuse session that has seen FUSE_INIT.
  void Start() {
    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
    // Every request (and backing syscall) checks the invariants
    // (dcfs/testonly/invariant_checker.h); a violation aborts the test.
    checker_ = std::make_unique<testonly::InvariantChecker>();
    observers_ = std::make_unique<testonly::Observers>(
        std::vector<ProtocolEvents *>{checker_.get(), &counter_});
    Observe(ctx_, observers_.get());
    HarnessDb() = db_.Get();
    ASSERT_OK_AND_ASSIGN(
        FileDescriptor owned,
        syscalls::openat(AT_FDCWD, source_, O_RDONLY | O_DIRECTORY));
    const int source_fd = *owned;
    ASSERT_OK_AND_ASSIGN(RootIdentity root,
                         backing::ProbeRoot(ctx_, source_fd));
    ASSERT_THAT(Migrate(db_, root), IsOk());
    ASSERT_THAT(backing::InitRoot(ctx_, std::move(owned)), IsOk());

    fs_ = std::make_unique<DirCacheFS>(ctx_, options_);
    ops_ = MakeFuseOps();
    // As main.cc does: "-o" and the options joined.
    std::string arg0 = "dir_cache_fs_test";
    std::string dash_o = "-o";
    std::string opts = absl::StrJoin(options_.mount_options, ",");
    std::vector<char *> argv = {arg0.data()};
    if (!opts.empty()) {
      argv.push_back(dash_o.data());
      argv.push_back(opts.data());
    }
    struct fuse_args args =
        FUSE_ARGS_INIT(static_cast<int>(argv.size()), argv.data());
    se_ = fuse_session_new(&args, &ops_, sizeof(ops_), fs_.get());
    fuse_opt_free_args(&args);
    ASSERT_NE(se_, nullptr);
    struct fuse_custom_io io = {};
    // Requests come in through Send(); a read is SessionLoop draining
    // the device at a checkpoint, which gets what a test queued (Kernel).
    io.read = [](int, void *buf, size_t size, void *) -> ssize_t {
      return current_->ReadQueued(buf, size);
    };
    // The I/O callbacks get the session's userdata, which is the
    // DirCacheFS (fuse_ops.cc reaches it through fuse_req_userdata), so
    // they reach this test through `current_`.
    io.writev = [](int, struct iovec *iov, int count, void *) -> ssize_t {
      return current_->Capture(iov, count);
    };
    ASSERT_OK_AND_ASSIGN(FileDescriptor dummy,
                         syscalls::openat(AT_FDCWD, "/dev/null", O_RDWR));
    const int dummy_fd = std::move(dummy).Release();  // the session owns it
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
    // The trace ends here (TearDown's DESTROY is not the test's); the
    // checks go on through it.
    if (observers_ != nullptr && recorder_ != nullptr) {
      observers_->Remove(recorder_.get());
    }
    OpenByHandleHook() = {};
    OpenByHandleFailures().clear();
    NameToHandleHook() = {};
    SyncfsHook() = {};
    StatvfsReadOnly() = false;
    NameToHandleFailure() = 0;
    FakeInodeNumbers().clear();
    StatxFailure() = 0;
    StatxHook() = {};
    Passthrough() = FakePassthrough();
    for (const std::string &mount : mounts_below_) {
      syscalls::umount2(mount, MNT_DETACH).IgnoreError();
    }
    if (se_ != nullptr) fuse_session_destroy(se_);
    current_ = nullptr;
    fs_.reset();
    if (db_.Get() != nullptr) Observe(ctx_, &NoProtocolEvents());
    ctx_.events = &NoProtocolEvents();
    observers_.reset();
    checker_.reset();
    HarnessDb() = nullptr;
    if (!source_.empty()) testonly::RemoveAll(source_);
  }

  // The daemon starting again after its process died, as main.cc starts
  // it: a new Context over the same database, with no mount fd yet
  // (InitRoot registers the source's), through backing::Startup. The
  // test's own ctx_ (the dead process's memory) is left as it was; the
  // recorder, if one is on, records the new one too.
  absl::Status Restart(std::string_view boot_id) {
    MountFds mounts;
    Context ctx{db_, mounts, bitgen_};
    // The new process is checked too (step 26.2), and recorded.
    ctx.events = ctx_.events;
    ABSL_ASSIGN_OR_RETURN(FileDescriptor source,
                          syscalls::openat(AT_FDCWD, source_,
                                           O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    absl::Status started = backing::Startup(ctx, std::move(source), boot_id);
    // From here on ctx_ is the dead process's memory, which no longer
    // describes the database the new process recovered (its durable set,
    // its opens): it is not checked any more (the new process was, through
    // Startup). Its DESTROY at TearDown is the harness's cleanup, not a
    // daemon's. The recorder, if one is on, goes on recording it.
    if (observers_ != nullptr) {
      observers_->Remove(checker_.get());
      observers_->Remove(&counter_);
    }
    return started;
  }

  std::string Path(std::string_view rel) const {
    return absl::StrCat(source_, "/", rel);
  }

  // Records this test's protocol events from here on (see the top of this
  // file), as the trace "<suite>.<test>". Call it once the test's setup is
  // done: its writes to the cache are the state each directory's trace
  // begins in.
  //
  // With `identities_only`, only the nodeids' identity traces are written:
  // for a scenario of the identity model (formal/ident.tla) whose
  // invalidation of a row behind the other models' backs (a step neither
  // dcfs.tla nor lifetime.tla nor reval.tla has) would end theirs.
  void StartTrace(bool identities_only = false) {
    const ::testing::TestInfo *info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    std::fflush(stdout);
    recorder_ = std::make_unique<testonly::TraceRecorder>(
        STDOUT_FILENO,
        absl::StrCat(info->test_suite_name(), ".", info->name()),
        /*files=*/!identities_only, /*lifetimes=*/!identities_only,
        /*identities=*/true, /*directories=*/!identities_only);
    observers_->Add(recorder_.get());
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
    return SendOn(se_, opcode, nodeid, body);
  }
  // Send, to another session (NewSession's).
  Reply SendOn(struct fuse_session *se, uint32_t opcode, uint64_t nodeid,
               std::string_view body) {
    const uint64_t unique = next_unique_++;
    std::string buf;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + body.size());
    hdr.opcode = opcode;
    hdr.unique = unique;
    hdr.nodeid = nodeid;
    // Root, from this process: the pid whose /proc/<pid>/task/<pid>/status
    // FuseRequest::Caller reads the supplementary groups from. pid 0 (what
    // the kernel sends for a caller outside the daemon's pid namespace)
    // has none to read, and Caller logs a WARNING for it, rate limited to
    // one per 60 s for the whole process, so whichever test first
    // mutated after the window lapsed saw an unrelated WARNING (step 26.14d).
    hdr.pid = static_cast<uint32_t>(syscalls::getpid());
    AppendBytes(buf, hdr);
    buf.append(body);
    struct fuse_buf fbuf = {};
    fbuf.mem = buf.data();
    fbuf.size = buf.size();
    fuse_session_process_buf(se, &fbuf);
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
    absl::StatusOr<FileDescriptor> raw_fd =
        syscalls::openat(AT_FDCWD, Path(rel), O_RDONLY);
    if (!raw_fd.ok()) return 0;
    const int raw = **raw_fd;
    int flags = 0;
    uint64_t fh = 0;
    if (syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags).ok()) {
      const int immutable = flags | FS_IMMUTABLE_FL;
      if (syscalls::ioctl(raw, FS_IOC_SETFLAGS, &immutable).ok()) {
        OutOfBand(id);
        fh = Open(id, O_RDONLY).second;
        if (!syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags).ok()) fh = 0;
        OutOfBand(id);
      }
    }
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
  // A request body's name: its bytes and a NUL.
  static std::string NameBody(std::string_view name) {
    std::string body(name);
    body.push_back('\0');
    return body;
  }

  // A FUSE_MKNOD body for a fifo `name`.
  static std::string MknodBody(std::string_view name) {
    struct fuse_mknod_in in = {};
    in.mode = S_IFIFO | 0644;
    std::string body;
    AppendBytes(body, in);
    return body + NameBody(name);
  }

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

  // A FLUSH of the open `fh` of `id` (a close(2) of one of its descriptors).
  Reply Flush(InodeId id, uint64_t fh) {
    struct fuse_flush_in in = {};
    in.fh = fh;
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_FLUSH, static_cast<uint64_t>(id), body);
  }

  // A SETATTR of `id`'s access time to `atime` (as utimensat(2) sends it).
  Reply SetAtime(InodeId id, absl::Time atime) {
    struct fuse_setattr_in in = {};
    in.valid = FATTR_ATIME;
    const struct timespec ts = absl::ToTimespec(atime);
    in.atime = static_cast<uint64_t>(ts.tv_sec);
    in.atimensec = static_cast<uint32_t>(ts.tv_nsec);
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_SETATTR, static_cast<uint64_t>(id), body);
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

  // A LINK, and the entry it returned (zeroed on error).
  std::pair<Reply, struct fuse_entry_out> LinkEntry(InodeId id,
                                                    InodeId newparent,
                                                    std::string_view newname) {
    Reply reply = Link(id, newparent, newname);
    struct fuse_entry_out entry {};
    if (reply.error == 0 && reply.payload.size() >= sizeof(entry)) {
      std::memcpy(&entry, reply.payload.data(), sizeof(entry));
    }
    return {reply, entry};
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
    ASSERT_THAT(syscalls::mount("tmpfs", path, "tmpfs", 0, "mode=0751"),
                IsOk())
        << path;
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
    if (!result.ok()) return {LookupResult::Kind::kUnknown, 0};
    if (result->kind != LookupResult::Kind::kFound) return {result->kind, 0};
    absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, result->id);
    EXPECT_THAT(attr, IsOk());
    return {result->kind, attr.ok() ? attr->backing_ino : 0};
  }

  absl::StatusOr<InodeId> Id(std::string_view name, InodeId dir = kRootInode) {
    absl::StatusOr<LookupResult> result =
        backing::LookupOrPopulate(ctx_, dir, name);
    if (!result.ok()) return result.status();
    if (result->kind != LookupResult::Kind::kFound) {
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
  // The mount options are main.cc's for a plain command line, which
  // Start() gives libfuse too.
  DirCacheFS::Options options_{
      .sync_interval = absl::Hours(24),
      .max_held_fds = 64,
      .mount_options = BuildMountOptions(false, {}).value().options};
  std::unique_ptr<DirCacheFS> fs_;
  std::unique_ptr<testonly::TraceRecorder> recorder_;
  std::unique_ptr<testonly::InvariantChecker> checker_;
  // The cost counters (step 26.4b), and the one observer Context::events
  // is: the checker, the counter and, after StartTrace, the recorder.
  testonly::CostCounter counter_;
  std::unique_ptr<testonly::Observers> observers_;
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
  // Messages "the kernel" queued for the next read of the device
  // (QueueInterrupt), which only SessionLoop's draining does.
  std::deque<std::string> kernel_reads_;

  ssize_t ReadQueued(void *buf, size_t size) {
    if (kernel_reads_.empty()) {
      errno = EAGAIN;
      return -1;
    }
    std::string msg = std::move(kernel_reads_.front());
    kernel_reads_.pop_front();
    const size_t n = std::min(size, msg.size());
    std::memcpy(buf, msg.data(), n);
    return static_cast<ssize_t>(n);
  }

 protected:
  // The unique of the next request Send() sends.
  uint64_t NextUnique() const { return next_unique_; }
  size_t KernelReadsLeft() const { return kernel_reads_.size(); }
  // The reply to request `unique`, served by something other than Send()
  // (SessionLoop::Run); error -EIO if none came.
  Reply TakeReply(uint64_t unique) {
    auto it = replies_.find(unique);
    if (it == replies_.end()) return Reply{.error = -EIO};
    Reply reply = std::move(it->second);
    replies_.erase(it);
    return reply;
  }
  // Queues a request message for "the kernel" to hand out; its unique.
  uint64_t QueueRequest(uint32_t opcode, uint64_t nodeid,
                        std::string_view body) {
    std::string msg;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + body.size());
    hdr.opcode = opcode;
    hdr.unique = next_unique_++;
    hdr.nodeid = nodeid;
    AppendBytes(msg, hdr);
    msg.append(body);
    kernel_reads_.push_back(std::move(msg));
    return hdr.unique;
  }
  bool KernelReadsEmpty() const { return kernel_reads_.empty(); }
  // Queues the FUSE_INTERRUPT the kernel sends for request `unique` (its
  // caller got a signal).
  // A second session over `fs` (the same forged I/O as Start's), with
  // extra command-line arguments; null on failure.
  struct fuse_session *NewSession(DirCacheFS *fs,
                                  std::vector<std::string> extra) {
    std::vector<std::string> words = {"dir_cache_fs_test"};
    words.insert(words.end(), extra.begin(), extra.end());
    std::vector<char *> argv;
    for (std::string &w : words) argv.push_back(w.data());
    struct fuse_args args =
        FUSE_ARGS_INIT(static_cast<int>(argv.size()), argv.data());
    struct fuse_session *se = fuse_session_new(&args, &ops_, sizeof(ops_), fs);
    fuse_opt_free_args(&args);
    if (se == nullptr) return nullptr;
    struct fuse_custom_io io = {};
    io.read = [](int, void *buf, size_t size, void *) -> ssize_t {
      return current_->ReadQueued(buf, size);
    };
    io.writev = [](int, struct iovec *iov, int count, void *) -> ssize_t {
      return current_->Capture(iov, count);
    };
    absl::StatusOr<FileDescriptor> dummy =
        syscalls::openat(AT_FDCWD, "/dev/null", O_RDWR);
    if (!dummy.ok() ||
        fuse_session_custom_io(se, &io, sizeof(io),
                               std::move(*dummy).Release()) != 0) {
      fuse_session_destroy(se);
      return nullptr;
    }
    return se;
  }

  // Queues the FUSE_INIT the kernel sends when it mounts, offering `flags`.
  void QueueInit(uint32_t flags = FUSE_POSIX_ACL | FUSE_DONT_MASK) {
    struct fuse_init_in in = {};
    in.major = FUSE_KERNEL_VERSION;
    in.minor = FUSE_KERNEL_MINOR_VERSION;
    in.flags = flags;
    std::string msg;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + sizeof(in));
    hdr.opcode = FUSE_INIT;
    hdr.unique = next_unique_++;
    AppendBytes(msg, hdr);
    AppendBytes(msg, in);
    kernel_reads_.push_back(std::move(msg));
  }

  void QueueInterrupt(uint64_t unique) {
    struct fuse_interrupt_in in = {};
    in.unique = unique;
    std::string msg;
    struct fuse_in_header hdr = {};
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + sizeof(in));
    hdr.opcode = FUSE_INTERRUPT;
    hdr.unique = next_unique_++;
    AppendBytes(msg, hdr);
    AppendBytes(msg, in);
    kernel_reads_.push_back(std::move(msg));
  }
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
    WriteFile(Path("d/a"));
    if (second_link) {
      ASSERT_THAT(
          syscalls::linkat(AT_FDCWD, Path("d/a"), AT_FDCWD, Path("d/link_a"), 0),
          IsOk());
    }
    WriteFile(Path("d/c"));
    ino_a_ = InoOf(Path("d/a"));
    ino_c_ = InoOf(Path("d/c"));
    Start();
    ASSERT_OK_AND_ASSIGN(d_, Id("d"));
    ASSERT_THAT(Id("a", d_), IsOk());  // Populates d: rows for all.
    ASSERT_THAT(Id("c", d_), IsOk());
    ASSERT_THAT(cache::MarkDirComplete(ctx_, d_, false), IsOk());
    ASSERT_EQ(Cached(d_, "b").first, LookupResult::Kind::kUnknown);
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
    ASSERT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/a")).ok());
    ASSERT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/c")).ok());
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
  EXPECT_EQ(Cached(d_, "b"),
            std::make_pair(LookupResult::Kind::kFound, ino_c_));
  EXPECT_NE(Cached(d_, "a").first, LookupResult::Kind::kFound);
  EXPECT_NE(Cached(d_, "c").first, LookupResult::Kind::kFound);
  EXPECT_EQ(Cached(d_, "link_a"),
            std::make_pair(LookupResult::Kind::kFound, ino_a_));
}

// Without one the old file's row is gone; the old phase 3's LinkDentry
// failed and rolled back, leaving b unknown. Now the rename re-resolves
// and records what b really is.
TEST_F(RenameStaleSourceTest, OldObjectWithoutAnotherLinkIsNotLinked) {
  Build(/*second_link=*/false);
  RunRenames();
  EXPECT_EQ(Cached(d_, "b"),
            std::make_pair(LookupResult::Kind::kFound, ino_c_));
  EXPECT_NE(Cached(d_, "a").first, LookupResult::Kind::kFound);
  EXPECT_NE(Cached(d_, "c").first, LookupResult::Kind::kFound);
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
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("a")), IsOk());
  EXPECT_FALSE(syscalls::fstatat(AT_FDCWD, Path("b")).ok());
  other.End();
  EXPECT_EQ(Rename(kRootInode, "a", kRootInode, "b").error, 0);
  EXPECT_EQ(Cached(kRootInode, "b"),
            std::make_pair(LookupResult::Kind::kFound, InoOf(Path("b"))));
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
  ASSERT_OK_AND_ASSIGN(
      struct statx stale,
      syscalls::statx(AT_FDCWD, Path("f"), AT_SYMLINK_NOFOLLOW,
                      STATX_BASIC_STATS | STATX_BTIME));
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  WriteFile(Path("d/a"));
  ASSERT_THAT(
      syscalls::linkat(AT_FDCWD, Path("d/a"), AT_FDCWD, Path("d/link_a"), 0),
      IsOk());
  WriteFile(Path("d/b"));
  ASSERT_THAT(
      syscalls::linkat(AT_FDCWD, Path("d/b"), AT_FDCWD, Path("d/link_b"), 0),
      IsOk());
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
  ASSERT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/a")).ok());
  ASSERT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/b")).ok());
  ASSERT_EQ(InoOf(Path("d/link_a")), ino_x);
  ASSERT_EQ(InoOf(Path("d/link_b")), ino_y);

  EXPECT_EQ(Cached(d, "a").first, LookupResult::Kind::kNegative);
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
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("a")), IsOk());
  EXPECT_EQ(Cached(kRootInode, "a").first, LookupResult::Kind::kFound);
  other.End();
  EXPECT_EQ(Unlink(kRootInode, "a").error, 0);
  EXPECT_FALSE(syscalls::fstatat(AT_FDCWD, Path("a")).ok());
  EXPECT_EQ(Cached(kRootInode, "a").first, LookupResult::Kind::kNegative);
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("d/a")), IsOk());
}

// The same for an unlink.
TEST_F(DirCacheFSTest, UnlinkGivesUpWhileItsParentKeepsChanging) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("d/a")), IsOk());
}

// A readdir of an incomplete directory populates it three times, and a
// mkdir in it runs during each population (after its fill snapshot), so no
// listing can be recorded: EAGAIN.
TEST_F(DirCacheFSTest, ReaddirGivesUpWhileItsDirectoryKeepsChanging) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
  // Changes of d's own attributes (the model's attrchange, step 12.11).
  EXPECT_EQ(Chmod(d, 0750).error, 0);
  EXPECT_EQ(Setxattr(d, "user.k", "v").error, 0);
  EXPECT_THAT(List(d, true),
              IsOkAndHolds(UnorderedElementsAre(".", "..", "c", "e")));
  EXPECT_EQ(Fsyncdir(d).error, 0);
  EXPECT_THAT(Dirty(), Not(Contains(d)));
}

// A getattr and a readdirplus of directories whose attributes are unknown
// (left so by the setup): each refreshes them (its statx, then a fill).
TEST_F(DirCacheFSTest, UnknownAttributesAreRefreshed) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d1"), 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d2"), 0755), IsOk());
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
  EXPECT_EQ(Cached(kRootInode, "new").first, LookupResult::Kind::kFound);
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
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor raw_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
  const int raw = *raw_fd;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &immutable), IsOk());
  OutOfBand(f);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [ro, ro_fh] = Open(f, O_RDONLY);
  EXPECT_EQ(ro.error, 0);
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags), IsOk());
  OutOfBand(f);
  auto [rw, rw_fh] = Open(f, O_WRONLY);
  EXPECT_EQ(rw.error, 0);
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
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor raw_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
  const int raw = *raw_fd;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
  const int append_only = flags | FS_APPEND_FL;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &append_only), IsOk());
  OutOfBand(f);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  auto [app, app_fh] = Open(f, O_WRONLY | O_APPEND);
  EXPECT_EQ(app.error, 0);
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags), IsOk());
  OutOfBand(f);
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
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
            std::make_pair(LookupResult::Kind::kFound, attr.backing_ino));
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

// A create whose backing syscall succeeded but whose new row cannot be
// recorded (here a trigger; in the field a full or failing cache disk:
// enospc_cache_test) cannot name the object in its reply. It replies EEXIST,
// which is true now and makes the kernel drop the negative entry its
// LOOKUP before the create left (fuse_invalidate_entry, Linux 6.6): any
// other error left that entry answering ENOENT for a file that exists, for
// --entry_timeout_sec (step 11.4). The name stays unknown, and once the
// cache can record again a lookup finds the file.
TEST_F(DirCacheFSTest, CreateThatCannotBeRecordedRepliesEexist) {
  Start();
  // A negative entry (nodeid 0), which the kernel would cache.
  ASSERT_EQ(Lookup(kRootInode, "new").second.nodeid, 0u);
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_insert BEFORE INSERT ON inodes "
                       "BEGIN SELECT RAISE(ABORT, 'cache full'); END"),
              IsOk());
  EXPECT_EQ(Create(kRootInode, "new", O_RDWR).reply.error, -EEXIST);
  EXPECT_EQ(Mkdir(kRootInode, "newdir").first.error, -EEXIST);
  EXPECT_EQ(Send(FUSE_MKNOD, kRootInode, MknodBody("fifo")).error, -EEXIST);
  EXPECT_EQ(Send(FUSE_SYMLINK, kRootInode, NameBody("link") + NameBody("t"))
                .error,
            -EEXIST);
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_insert"), IsOk());
  for (const char *name : {"new", "newdir", "fifo", "link"}) {
    EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path(name), AT_SYMLINK_NOFOLLOW),
                IsOk())
        << name;
    EXPECT_THAT(cache::Lookup(ctx_, kRootInode, name),
                IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)))
        << name;
    EXPECT_NE(Lookup(kRootInode, name).second.nodeid, 0u) << name;
  }
}

// The trace of a create that cannot be recorded (its EEXIST after a
// syscall that succeeded, above) ends with a "failed" cut, a step the model
// does not have, not with a reply line T_Reply would reject (the model's
// create replies success there; step 12.7b). The recorder writes to a file
// of the test's own, not the harness's traces: the trace shards allow no
// cut.
TEST_F(DirCacheFSTest, CreateThatCannotBeRecordedEndsItsTraceFailed) {
  Start();
  const std::string path =
      absl::StrCat(std::getenv("TEST_TMPDIR"), "/create_not_recorded.trace");
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor fd,
      syscalls::openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0600));
  recorder_ = std::make_unique<testonly::TraceRecorder>(
      *fd, "selfcheck", /*files=*/false, /*lifetimes=*/false,
      /*identities=*/false, /*directories=*/true);
  observers_->Add(recorder_.get());
  recorder_->BeginAll(ctx_);
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_insert BEFORE INSERT ON inodes "
                       "BEGIN SELECT RAISE(ABORT, 'cache full'); END"),
              IsOk());
  EXPECT_EQ(Mkdir(kRootInode, "newdir").first.error, -EEXIST);
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_insert"), IsOk());
  observers_->Remove(recorder_.get());
  recorder_.reset();
  std::vector<std::string> root;
  for (std::string_view line :
       absl::StrSplit(ReadWholeFile(path), '\n', absl::SkipEmpty())) {
    if (absl::StartsWith(line, absl::StrCat("DCFS-TRACE selfcheck ",
                                            kRootInode, " "))) {
      root.emplace_back(line);
    }
  }
  ASSERT_FALSE(root.empty());
  EXPECT_THAT(root.back(),
              AllOf(HasSubstr("\"ev\":\"cut\""),
                    HasSubstr("failed: a create whose object could not be "
                              "recorded")));
  EXPECT_THAT(root, Not(Contains(HasSubstr("\"ev\":\"reply\""))));
  EXPECT_THAT(root, Not(Contains(HasSubstr("\"ev\":\"unexplained\""))));
}

// A create whose probe saw the object (its statx succeeded) but could not
// finish probing it (here the handle) cannot record it: the object exists,
// so EEXIST, which makes the kernel drop its negative entry.
TEST_F(DirCacheFSTest, CreateWhoseObjectWasSeenRepliesEexist) {
  Start();
  NameToHandleFailure() = EIO;
  EXPECT_EQ(Send(FUSE_MKNOD, kRootInode, MknodBody("fifo")).error, -EEXIST);
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("fifo"), AT_SYMLINK_NOFOLLOW),
              IsOk());
  EXPECT_NE(Lookup(kRootInode, "fifo").second.nodeid, 0u);
}

// Unlinks `path` when the request's phase-2 syscall returns: the created
// object is gone again before the probe looks at it.
class UnlinkAfterSyscall : public ProtocolEvents {
 public:
  explicit UnlinkAfterSyscall(std::string path) : path_(std::move(path)) {}
  void MutationSyscall(Context &, const absl::Status &status) override {
    if (status.ok()) {
      unlinked = syscalls::unlinkat(AT_FDCWD, path_, 0).ok();
    }
  }
  void NewChildProbed(Context &, events::Ino, std::string_view,
                      const events::Probe &probe) override {
    probes.push_back(probe.kind);
  }
  bool unlinked = false;
  std::vector<events::Probe::Kind> probes;

 private:
  std::string path_;
};

// A create whose probe finds the name gone (ENOENT before any statx saw the
// object) replies the probe's own error: EEXIST would not be true.
TEST_F(DirCacheFSTest, CreateWhoseObjectIsGoneRepliesEnoent) {
  Start();
  UnlinkAfterSyscall remover(Path("fifo"));
  observers_->Add(&remover);
  const int error = Send(FUSE_MKNOD, kRootInode, MknodBody("fifo")).error;
  observers_->Remove(&remover);
  ASSERT_TRUE(remover.unlinked);
  EXPECT_EQ(error, -ENOENT);
  EXPECT_THAT(remover.probes, ElementsAre(events::Probe::Kind::kAbsent))
      << "the model's CreateProbe found nothing";
}

// A writable create whose phase 1 for the writes (BeginWriting) cannot be
// recorded: the file exists and is recorded, so EEXIST.
TEST_F(DirCacheFSTest, CreateWhoseWritesCannotBeginRepliesEexist) {
  Start();
  // BeginWriting is the only writer of an unknown xattr row here.
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_unknown BEFORE INSERT ON "
                       "xattrs WHEN NEW.state = 'unknown' "
                       "BEGIN SELECT RAISE(ABORT, 'cache full'); END"),
              IsOk());
  EXPECT_EQ(Create(kRootInode, "w", O_RDWR).reply.error, -EEXIST);
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_unknown"), IsOk());
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("w")), IsOk());
  EXPECT_NE(Lookup(kRootInode, "w").second.nodeid, 0u);
}

// A create whose reply cannot refresh the new file's attributes (its last
// statx fails) is replied as done, from the row's last attributes
// (EntryAfterPhase2), not failed.
TEST_F(DirCacheFSTest, CreateWhoseRefreshFailsIsRepliedFromTheRow) {
  Start();
  int calls = 0;
  std::function<void()> count = [&] {
    ++calls;
    StatxHook() = count;
  };
  StatxHook() = count;
  ASSERT_EQ(Create(kRootInode, "w1", O_RDWR).reply.error, 0);
  StatxHook() = {};
  ASSERT_GT(calls, 0);
  int seen = 0;
  std::function<void()> fail_last = [&] {
    if (++seen == calls) {
      StatxFailure() = EIO;
    } else {
      StatxHook() = fail_last;
    }
  };
  StatxHook() = fail_last;
  EXPECT_EQ(Create(kRootInode, "w2", O_RDWR).reply.error, 0);
  EXPECT_EQ(seen, calls);
  StatxHook() = {};
}

// --- ESTALE from open_by_handle_at (step 11.3b) ------------------------------
//
// xfs and btrfs answer ESTALE also for an inode the device cannot read. dcfs
// asks the parent by name: the same inode still there (or a failed check)
// is EIO with the row kept and unknown; only a positive absence forgets it.

// A getattr of `id` whose cached attributes are unknown: the request that
// reopens it by handle (FreshAttr) and, failing, refreshes nothing.
#define EXPECT_KEPT_UNKNOWN(id, dir, name)                                  \
  do {                                                                      \
    absl::StatusOr<cache::CachedAttr> kept = cache::GetAttr(ctx_, id);      \
    EXPECT_THAT(kept, IsOkAndHolds(testing::Field(&cache::CachedAttr::valid, \
                                                  false)));                 \
    EXPECT_THAT(cache::Lookup(ctx_, dir, name),                             \
                IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));        \
  } while (false)

TEST_F(DirCacheFSTest, EstaleWithTheNameStillThereRepliesEio) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  OpenByHandleFailures() = {ESTALE};
  EXPECT_EQ(Getattr(f).first.error, -EIO);
  EXPECT_KEPT_UNKNOWN(f, kRootInode, "f");
  EXPECT_EQ(Getattr(f).first.error, 0);  // The device reads again.
}

TEST_F(DirCacheFSTest, EstaleWithTheNameCheckFailingRepliesEio) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  OpenByHandleFailures() = {ESTALE};
  NameToHandleFailure() = EIO;  // The name check's handle read.
  EXPECT_EQ(Getattr(f).first.error, -EIO);
  EXPECT_KEPT_UNKNOWN(f, kRootInode, "f");
}

TEST_F(DirCacheFSTest, EstaleWithTheNameGoneForgetsTheRow) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0), IsOk());
  OpenByHandleFailures() = {ESTALE};
  EXPECT_EQ(Chmod(f, 0600).error, -ESTALE);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DirCacheFSTest, EstaleWithTheNameReplacedForgetsTheRow) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(syscalls::renameat2(AT_FDCWD, Path("f"), AT_FDCWD, Path("g"), 0),
              IsOk());
  WriteFile(Path("f"));  // Another object under the name.
  OpenByHandleFailures() = {ESTALE};
  EXPECT_EQ(Chmod(f, 0600).error, -ESTALE);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              StatusIs(absl::StatusCode::kNotFound));
}

// The parent branches, with d/f: the child's handle fails, then the
// parent's (the check opens it by handle too).
class EstaleParentTest : public DirCacheFSTest {
 protected:
  void SetUpTree() {
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
    WriteFile(Path("d/f"));
    Start();
    ASSERT_OK_AND_ASSIGN(d_, Id("d"));
    ASSERT_OK_AND_ASSIGN(f_, Id("f", d_));
    ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f_), IsOk());
  }
  InodeId d_ = 0;
  InodeId f_ = 0;
};

// The parent's handle fails too and its own name is gone: the child is gone
// with it.
TEST_F(EstaleParentTest, ParentGoneForgetsTheChild) {
  SetUpTree();
  ASSERT_THAT(syscalls::renameat2(AT_FDCWD, Path("d"), AT_FDCWD, Path("e"), 0),
              IsOk());
  OpenByHandleFailures() = {ESTALE, ESTALE};
  EXPECT_EQ(Getattr(f_).first.error, -ESTALE);
  EXPECT_THAT(cache::GetAttr(ctx_, f_).status(),
              StatusIs(absl::StatusCode::kNotFound));
}

// The parent's handle fails but its name still holds it: the parent is
// unreadable, so the child cannot be checked: EIO, both kept.
TEST_F(EstaleParentTest, ParentStillThereRepliesEio) {
  SetUpTree();
  OpenByHandleFailures() = {ESTALE, ESTALE};
  EXPECT_EQ(Getattr(f_).first.error, -EIO);
  EXPECT_KEPT_UNKNOWN(f_, d_, "f");
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "d"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
}

// The parent's open fails with EIO: the child cannot be checked: EIO.
TEST_F(EstaleParentTest, ParentUnreadableRepliesEio) {
  SetUpTree();
  OpenByHandleFailures() = {ESTALE, EIO};
  EXPECT_EQ(Getattr(f_).first.error, -EIO);
  EXPECT_KEPT_UNKNOWN(f_, d_, "f");
}

// An object whose only name a failed mutation left unknown (an unknown
// dentry does not say which object it named): the unknown names are asked,
// and the one that still names it keeps it: EIO, not ESTALE.
TEST_F(DirCacheFSTest, EstaleWithTheNameUnknownRepliesEio) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  ASSERT_THAT(db_.Exec("UPDATE dentries SET state = 'unknown', inode = NULL "
                       "WHERE name = CAST('f' AS BLOB)"),
              IsOk());
  OpenByHandleFailures() = {ESTALE};
  EXPECT_EQ(Getattr(f).first.error, -EIO);
  EXPECT_THAT(cache::GetAttr(ctx_, f),
              IsOkAndHolds(testing::Field(&cache::CachedAttr::valid, false)));
}

// No name at all: the ESTALE is believed, as before.
TEST_F(DirCacheFSTest, EstaleWithNoNameForgetsTheRow) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM dentries WHERE name = CAST('f' AS BLOB)"),
              IsOk());
  OpenByHandleFailures() = {ESTALE};
  EXPECT_EQ(Getattr(f).first.error, -ESTALE);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              StatusIs(absl::StatusCode::kNotFound));
}

// More unknown names than are asked, none of them checked to be another
// object's: not proof of absence: EIO.
TEST_F(DirCacheFSTest, EstaleWithTooManyUnknownNamesRepliesEio) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM dentries WHERE name = CAST('f' AS BLOB)"),
              IsOk());
  for (int i = 0; i < 17; ++i) {
    ASSERT_THAT(db_.Exec(absl::StrCat(
                    "INSERT INTO dentries (parent, name, state) VALUES (1, "
                    "CAST('u", i, "' AS BLOB), 'unknown')")),
                IsOk());
  }
  OpenByHandleFailures() = {ESTALE};
  EXPECT_EQ(Getattr(f).first.error, -EIO);
  EXPECT_THAT(cache::GetAttr(ctx_, f), IsOk());
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
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mp"), 0755), IsOk());
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
  EXPECT_EQ(testonly::FileSize(Path("dst")).value_or(-1), 10);
  EXPECT_THAT(Dirty(), Contains(dst));  // The copy's phase 1.
  EXPECT_EQ(Release(dst, out_fh).error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, dst));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 10);

  // A failure the backing filesystem reports is replied, and leaves the
  // attributes right: here a destination whose shared backing fd is
  // read-only (the file was immutable when it was opened).
  int flags = 0;
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor raw_fd,
      syscalls::openat(AT_FDCWD, Path("dst"), O_RDONLY));
  const int raw = *raw_fd;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &immutable), IsOk());
  auto [ro, ro_fh] = Open(dst, O_RDONLY);
  ASSERT_EQ(ro.error, 0);
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags), IsOk());
  EXPECT_EQ(CopyFileRange(src, in_fh, dst, ro_fh, 100), -EBADF);
  EXPECT_EQ(testonly::FileSize(Path("dst")).value_or(-1), 10);
  EXPECT_EQ(Release(dst, ro_fh).error, 0);
  EXPECT_EQ(Release(src, in_fh).error, 0);
}

TEST_F(DirCacheFSTest, IoctlForwardsItsAllowlist) {
  WriteFile(Path("f"));
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  int flags = 0;
  {
    ASSERT_OK_AND_ASSIGN(
        FileDescriptor raw_fd,
        syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
    const int raw = *raw_fd;
    ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
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
  ASSERT_OK_AND_ASSIGN(struct stat st,
                       syscalls::fstatat(AT_FDCWD, Path("f")));
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

// --- access times (step 23.8) -----------------------------------------------
//
// Reads go through passthrough: dcfs never sees them, and the backing
// filesystem stamps the access time itself. While dcfs holds a backing
// descriptor for a file (its passthrough opens), the attributes it serves
// come from a statx of that descriptor: at FLUSH, at RELEASE, and for an
// attribute request while it is held. Nothing is predicted. The harness has
// no kernel passthrough: a test reads the backing file through a descriptor
// of its own, which moves the same inode's access time as a passthrough read
// would.

// Sets path's atime and mtime (seconds before now).
void SetTimes(const std::string &path, int64_t atime_ago, int64_t mtime_ago) {
  ASSERT_OK_AND_ASSIGN(struct timespec now,
                       syscalls::clock_gettime(CLOCK_REALTIME));
  const struct timespec times[2] = {
      {.tv_sec = now.tv_sec - atime_ago, .tv_nsec = 0},
      {.tv_sec = now.tv_sec - mtime_ago, .tv_nsec = 0},
  };
  ASSERT_THAT(syscalls::utimensat(AT_FDCWD, path, times, 0), IsOk()) << path;
}

// The access time of `path` on the backing filesystem (a symlink's own).
absl::Time BackingAtime(const std::string &path) {
  absl::StatusOr<struct statx> stx =
      syscalls::statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_ATIME);
  EXPECT_THAT(stx, IsOk()) << path;
  if (!stx.ok()) return absl::InfinitePast();
  return absl::FromUnixSeconds(stx->stx_atime.tv_sec) +
         absl::Nanoseconds(stx->stx_atime.tv_nsec);
}

// Reads a byte of `path` through a descriptor of its own opened with
// `flags`: what a passthrough read does to the backing inode.
void ReadBacking(const std::string &path, int flags = O_RDONLY) {
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       syscalls::openat(AT_FDCWD, path, flags));
  char c = 0;
  ASSERT_THAT(syscalls::read(*fd, &c, 1), IsOkAndHolds(1)) << path;
}

class AtimeTest : public DirCacheFSTest {
 protected:
  // A file "f" with contents, its atime two days old and its mtime three:
  // any read moves the atime under relatime.
  void MakeOldFile() {
    WriteFile(Path("f"));
    AppendToFile(Path("f"), "contents");
    SetTimes(Path("f"), 2 * 86400, 3 * 86400);
  }

  // The access time a GETATTR of `id` replies.
  absl::Time ServedAtime(InodeId id) {
    auto [reply, attr] = Getattr(id);
    EXPECT_EQ(reply.error, 0);
    return absl::FromUnixSeconds(static_cast<int64_t>(attr.atime)) +
           absl::Nanoseconds(attr.atimensec);
  }

  // The access time in `id`'s row, which must be current.
  absl::Time CachedAtime(InodeId id) {
    absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
    EXPECT_THAT(attr, IsOk());
    if (!attr.ok()) return absl::InfinitePast();
    EXPECT_TRUE(attr->valid);
    return absl::TimeFromTimespec(attr->st.st_atim);
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
    ASSERT_THAT(
        syscalls::mount(nullptr, tmpdir, nullptr, MS_REMOUNT | flag, nullptr),
        IsOk());
    remounted_ = true;
  }

  void TearDown() override {
    if (remounted_) {
      syscalls::mount(nullptr, std::getenv("TEST_TMPDIR"), nullptr,
                      MS_REMOUNT | MS_RELATIME, nullptr)
          .IgnoreError();
    }
    DirCacheFSTest::TearDown();
  }

  bool remounted_ = false;
};

// An open that reads nothing (a shell's `: <file`, lsattr's private open)
// leaves the access time where the backing filesystem has it.
TEST_F(AtimeTest, OpenThatReadsNothingLeavesTheAtime) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const absl::Time before = BackingAtime(Path("f"));
  OpenAndRelease(f, O_RDONLY);
  EXPECT_EQ(BackingAtime(Path("f")), before);
  EXPECT_EQ(ServedAtime(f), before);
}

// After a read, a stat shows exactly what the backing filesystem stamped,
// recorded at the release from the held descriptor, and the row is dirty:
// the backing filesystem writes the atime back lazily, and only the next
// sync point's syncfs makes it durable.
TEST_F(AtimeTest, ReadThenStatServesTheBackingsAtimeExactly) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const absl::Time before = BackingAtime(Path("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"));
  const absl::Time read = BackingAtime(Path("f"));
  ASSERT_GT(read, before);
  ASSERT_EQ(Release(f, fh).error, 0);
  EXPECT_EQ(CachedAtime(f), read);
  EXPECT_EQ(ServedAtime(f), read);
  EXPECT_THAT(Dirty(), Contains(f));
}

// A GETATTR while the file is open is answered from the held descriptor:
// it shows a read made since the open.
TEST_F(AtimeTest, GetattrWhileHeldServesTheRead) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const absl::Time before = BackingAtime(Path("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  EXPECT_EQ(ServedAtime(f), before);
  ReadBacking(Path("f"));
  EXPECT_EQ(ServedAtime(f), BackingAtime(Path("f")));
  EXPECT_THAT(Dirty(), Contains(f));
  EXPECT_EQ(Release(f, fh).error, 0);
}

// A file's own noatime flag (chattr +A): its reads leave the atime, and so
// does dcfs.
TEST_F(AtimeTest, NoatimeFlagIsRespected) {
  MakeOldFile();
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                         syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
    int flags = 0;
    ASSERT_THAT(syscalls::ioctl(*fd, FS_IOC_GETFLAGS, &flags), IsOk());
    flags |= FS_NOATIME_FL;
    ASSERT_THAT(syscalls::ioctl(*fd, FS_IOC_SETFLAGS, &flags), IsOk());
  }
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const absl::Time before = BackingAtime(Path("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"));
  ASSERT_EQ(Release(f, fh).error, 0);
  EXPECT_EQ(BackingAtime(Path("f")), before);
  EXPECT_EQ(ServedAtime(f), before);
}

// O_NOATIME (the kernel passes the open's flags to the backing file a
// passthrough read goes through): the atime stays, and dcfs serves it.
TEST_F(AtimeTest, ONoatimeOpenServesTheUnchangedAtime) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const absl::Time before = BackingAtime(Path("f"));
  auto [open, fh] = Open(f, O_RDONLY | O_NOATIME);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"), O_RDONLY | O_NOATIME);
  ASSERT_EQ(Release(f, fh).error, 0);
  EXPECT_EQ(BackingAtime(Path("f")), before);
  EXPECT_EQ(ServedAtime(f), before);
}

// The costs: one statx of the held descriptor per FLUSH and per RELEASE, one
// per attribute request while held (and no transaction when nothing
// changed), and nothing new for an attribute request served from the cache.
TEST_F(AtimeTest, OneStatxPerFlushReleaseAndHeldGetattr) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Getattr(f).first.error, 0);
  counter_.Reset();
  ASSERT_EQ(Getattr(f).first.error, 0);
  EXPECT_EQ(counter_.counts().backing_calls, 1) << "a GETATTR while held";
  EXPECT_EQ(counter_.counts().transactions, 0) << "nothing changed";
  counter_.Reset();
  ASSERT_EQ(Flush(f, fh).error, 0);
  EXPECT_EQ(counter_.counts().backing_calls, 1) << "a FLUSH";
  counter_.Reset();
  ASSERT_EQ(Release(f, fh).error, 0);
  EXPECT_EQ(counter_.counts().backing_calls, 1) << "a RELEASE";
  counter_.Reset();
  ASSERT_EQ(Getattr(f).first.error, 0);
  EXPECT_EQ(counter_.counts().backing_calls, 0) << "a GETATTR from the cache";
  EXPECT_EQ(counter_.counts().transactions, 0) << "a GETATTR from the cache";
  // A written file, closed: dcfs keeps an O_PATH descriptor of it until its
  // last FORGET, but no read can move its access time: from the cache too.
  Created w = Create(kRootInode, "w", O_WRONLY);
  ASSERT_EQ(w.reply.error, 0);
  EXPECT_EQ(Release(w.id, w.fh).error, 0);
  ASSERT_EQ(Getattr(w.id).first.error, 0);
  counter_.Reset();
  EXPECT_EQ(Getattr(w.id).first.error, 0);
  EXPECT_EQ(counter_.counts().backing_calls, 0)
      << "a GETATTR of a written, closed file";
}

// Any record of a held file's attributes may capture an access time the
// backing filesystem has not written back yet, so it marks the row dirty,
// whichever path records it (here a refresh by handle, as a mutation's
// phase 3 or a population's probe records it).
TEST_F(AtimeTest, AnyFillOfAHeldFileMarksItDirty) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  // Dirty since the cold open; taken out here (no request comes before the
  // record below puts it back), to see the record's own mark.
  ASSERT_THAT(Dirty(), Contains(f));
  ASSERT_THAT(db_.Exec(absl::StrCat("DELETE FROM dirty WHERE inode = ", f)),
              IsOk());
  ReadBacking(Path("f"));
  ASSERT_THAT(backing::RefreshAttrs(ctx_, f), IsOk());
  EXPECT_EQ(CachedAtime(f), BackingAtime(Path("f")));
  EXPECT_THAT(Dirty(), Contains(f));
  EXPECT_EQ(Release(f, fh).error, 0);
}

// A power loss after a read and its release: the cache recorded the new
// access time, the backing filesystem lost its lazy write-back of it (put
// back here by hand, as the disk would have it). The recorded row is dirty,
// so the restart forgets it and serves the backing filesystem's value.
TEST_F(AtimeTest, PowerLossAfterAReadServesTheBackingsAtime) {
  MakeOldFile();
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const absl::Time before = BackingAtime(Path("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"));
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_GT(CachedAtime(f), before);
  const struct timespec times[2] = {absl::ToTimespec(before),
                                    {.tv_sec = 0, .tv_nsec = UTIME_OMIT}};
  ASSERT_THAT(syscalls::utimensat(AT_FDCWD, Path("f"), times, 0), IsOk());

  ASSERT_THAT(Restart("boot"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid)
      << "recovered: to be read again from the backing filesystem, which has "
      << before << "; the row says " << absl::TimeFromTimespec(attr.st.st_atim);
}

// A held fill that cannot record: its statx fails (the FLUSH still
// succeeds), or a mutation of the file ran while it read (a chmod run
// inside the statx, as under coroutines). Either leaves the attributes
// unknown, the row dirty, and the next attribute reply re-reads them.
TEST_F(AtimeTest, AHeldFillThatCannotRecordLeavesTheAttributesUnknown) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  StatxFailure() = EIO;
  EXPECT_EQ(Flush(f, fh).error, 0) << "a close does not fail over it";
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr failed, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(failed.valid) << "after a failed statx";
  EXPECT_THAT(Dirty(), Contains(f));
  EXPECT_EQ(ServedAtime(f), BackingAtime(Path("f")));

  // The chmod's own refresh records its attributes; a read after it moves
  // the access time again, so the held fill reads something else and has
  // to decide (with nothing moved since, it would skip the write).
  StatxHook() = [&] {
    EXPECT_EQ(Chmod(f, 0600).error, 0);
    ReadBacking(Path("f"));
  };
  EXPECT_EQ(Getattr(f).first.error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr raced, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(raced.valid) << "after a chmod during the statx";
  EXPECT_THAT(Dirty(), Contains(f));
  auto [reply, attr] = Getattr(f);
  EXPECT_EQ(reply.error, 0);
  EXPECT_EQ(attr.mode & 07777, 0600u) << "the next reply re-reads";
  EXPECT_EQ(Release(f, fh).error, 0);
}

// A held fill whose record fails (its transaction rolls back: here the
// dirty insert is refused) leaves the attributes unknown, not the row
// valid with the access time from before the read.
TEST_F(AtimeTest, AHeldFillWhoseRecordFailsLeavesTheAttributesUnknown) {
  MakeOldFile();
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"));
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER refuse_dirty BEFORE INSERT ON "
                       "dirty BEGIN SELECT RAISE(FAIL, 'refused'); END"),
              IsOk());
  EXPECT_EQ(Flush(f, fh).error, 0) << "a close does not fail over it";
  ASSERT_THAT(db_.Exec("DROP TRIGGER refuse_dirty"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid) << "after a held fill that could not record";
  EXPECT_EQ(Release(f, fh).error, 0);
}

// After a crash, an atime-only row (a file only read) makes its attributes
// unknown and nothing else: its names, xattrs and the listing it is in
// stay, and the start does not probe it.
TEST_F(AtimeTest, RecoveryOfAnAtimeOnlyRowForgetsOnlyItsAttributes) {
  MakeOldFile();
  const uint8_t v[] = {'v'};
  ASSERT_THAT(syscalls::setxattr(Path("f"), "user.k", v, 0), IsOk());
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_EQ(Getxattr(f, "user.k").error, 0);
  OpenAndRelease(f, O_RDONLY);
  ASSERT_THAT(Dirty(), ElementsAre(f));
  ASSERT_THAT(Restart("boot"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid);
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "f"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)))
      << "its name";
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true))
      << "the listing it is in";
  EXPECT_THAT(cache::GetXattr(ctx_, f, "user.k"),
              IsOkAndHolds(::testing::Optional(::testing::Eq("v"))))
      << "its xattrs";
}

// On a noatime mount a listing stamps nothing.
TEST_F(AtimeTest, ReaddirOnANoatimeMountStampsNothing) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  RemountSource(MS_NOATIME);
  Start();
  ASSERT_EQ(ctx_.atime, AtimePolicy::kNever);
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_THAT(List(d, false), IsOk());
  const absl::Time listed = CachedAtime(d);
  ASSERT_THAT(List(d, false), IsOk());
  EXPECT_EQ(CachedAtime(d), listed);
}

// Today's numbers (steps per entry, and per listing), measured with
// RecordProperty below; lowering them after a reduction is welcome.
// (In tenths: steps * 10 <= per_entry * n + fixed.)
constexpr int64_t kReaddirStepsPerEntry = 13;      // measured 1.12 to 1.28
constexpr int64_t kReaddirplusStepsPerEntry = 13;  // measured 1.22 to 1.31
constexpr int64_t kReaddirStepsFixed = 100;

// Logging is initialized for the whole run, before any test (initializing
// per test would change what later tests print).
const bool kLogInitialized = (absl::InitializeLog(), true);

// --- readdir work counting (step N4) ----------------------------------------
//
// A listing of N cached entries costs SQLite steps linear in N: counting
// them replaces the wall-clock budget that failed under load. The bounds
// are today's numbers (steps <= a * N + b); a listing that re-checks
// completeness or re-reads attributes per entry, or one that rescans the
// directory from the start on every reply, exceeds them (and the 1000-entry
// case exceeds ten times the 100-entry one).

class ReaddirWorkTest : public DirCacheFSTest {
 protected:
  // The whole listing of `dir` through replies of `reply_size` bytes (each
  // resuming at the last entry's offset), as the kernel reads it; returns
  // the names without "." and "..".
  std::vector<std::string> ListAll(InodeId dir, bool plus,
                                   uint32_t reply_size = 4096) {
    std::vector<std::string> names;
    uint64_t off = 0;
    while (true) {
      struct fuse_read_in in = {};
      in.offset = off;
      in.size = reply_size;
      std::string body;
      AppendBytes(body, in);
      Reply reply = Send(plus ? FUSE_READDIRPLUS : FUSE_READDIR,
                         static_cast<uint64_t>(dir), body);
      EXPECT_EQ(reply.error, 0);
      if (reply.error != 0 || reply.payload.empty()) break;
      const std::string &p = reply.payload;
      size_t pos = 0;
      while (pos < p.size()) {
        const size_t at =
            plus ? pos + offsetof(struct fuse_direntplus, dirent) : pos;
        struct fuse_dirent d {};
        std::memcpy(&d, p.data() + at, FUSE_NAME_OFFSET);
        std::string name = p.substr(at + FUSE_NAME_OFFSET, d.namelen);
        if (name != "." && name != "..") names.push_back(std::move(name));
        off = d.off;
        pos += plus ? FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET_DIRENTPLUS + d.namelen)
                    : FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + d.namelen);
      }
    }
    return names;
  }

  // Steps of one warm full listing of a directory of `n` entries.
  void WarmListingSteps(int n, bool plus, int64_t *steps) {
    EXPECT_THAT(syscalls::mkdirat(AT_FDCWD, Path("many"), 0755), IsOk());
    for (int i = 0; i < n; ++i) {
      WriteFile(Path(absl::StrFormat("many/file-%05d", i)));
    }
    Start();
    ASSERT_OK_AND_ASSIGN(InodeId many, Id("many"));
    // Cold: the first listing populates the cache.
    EXPECT_EQ(ListAll(many, plus).size(), static_cast<size_t>(n));
    counter_.Reset();
    EXPECT_EQ(ListAll(many, plus).size(), static_cast<size_t>(n));
    *steps = counter_.counts().steps;
    std::cerr << "READDIR-STEPS " << (plus ? "plus" : "plain") << " n=" << n
              << " steps=" << *steps << "\n";
  }
};

TEST_F(ReaddirWorkTest, WarmReaddirStepsAreLinearInTheEntries) {
  int64_t steps = 0;
  WarmListingSteps(100, /*plus=*/false, &steps);
  RecordProperty("steps_100", steps);
  EXPECT_LE(steps * 10, kReaddirStepsPerEntry * 100 + kReaddirStepsFixed);
}

TEST_F(ReaddirWorkTest, WarmReaddirplusStepsAreLinearInTheEntries) {
  int64_t steps = 0;
  WarmListingSteps(100, /*plus=*/true, &steps);
  RecordProperty("steps_100", steps);
  EXPECT_LE(steps * 10, kReaddirplusStepsPerEntry * 100 + kReaddirStepsFixed);
}

TEST_F(ReaddirWorkTest, ThousandEntriesAreLinearToo) {
  int64_t steps = 0;
  WarmListingSteps(1000, /*plus=*/false, &steps);
  RecordProperty("steps_1000", steps);
  EXPECT_LE(steps * 10, kReaddirStepsPerEntry * 1000 + kReaddirStepsFixed);
}

TEST_F(ReaddirWorkTest, ThousandEntriesPlusAreLinearToo) {
  int64_t steps = 0;
  WarmListingSteps(1000, /*plus=*/true, &steps);
  RecordProperty("steps_1000", steps);
  EXPECT_LE(steps * 10, kReaddirplusStepsPerEntry * 1000 + kReaddirStepsFixed);
}

// --- the injected clock (step 26.10) ----------------------------------------
//
// DirCacheFS reads the time only through Context::clock: the periodic sync
// point and relatime at read-open. A SimulatedClock makes both deterministic.

class ClockTest : public DirCacheFSTest {
 protected:
  ClockTest() {
    ctx_.clock = &clock_;
    options_.sync_interval = absl::Seconds(5);
  }

  // An empty request, at which the periodic sync point is considered.
  void Tick() {
    struct fuse_getattr_in in = {};
    std::string body;
    AppendBytes(body, in);
    EXPECT_EQ(Send(FUSE_GETATTR, static_cast<uint64_t>(kRootInode), body).error,
              0);
  }

  // The access time in `id`'s row, current or not (a recovered row keeps
  // its last one as a hint).
  absl::Time RowAtime(InodeId id) {
    absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
    EXPECT_THAT(attr, IsOk());
    if (!attr.ok()) return absl::InfinitePast();
    return absl::TimeFromTimespec(attr->st.st_atim);
  }

  absl::SimulatedClock clock_{absl::FromUnixSeconds(1'800'000'000)};
};

// The periodic sync point runs once the interval has elapsed on the
// injected clock, not before, and exactly once; a descriptor held for a
// written file survives it (the FORGET after it still re-reads through the
// held descriptor).
TEST_F(ClockTest, PeriodicSyncPointFollowsTheInjectedClock) {
  WriteFile(Path("f"));
  Start();
  int syncs = 0;
  SyncfsHook() = [&] { ++syncs; };
  // A LOOKUP, not Id(): the FORGET below must forget a lookup the kernel
  // was handed (the invariant checks' lookup-count, step 26.2).
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);  // written, held
  ASSERT_THAT(Dirty(), Contains(f));
  ASSERT_EQ(syncs, 0);

  clock_.AdvanceTime(absl::Seconds(4));
  Tick();
  EXPECT_EQ(syncs, 0) << "before the interval has elapsed";

  clock_.AdvanceTime(absl::Seconds(2));
  Tick();
  EXPECT_EQ(syncs, 1) << "once, after it";
  Tick();
  EXPECT_EQ(syncs, 1) << "not again until another interval has elapsed";

  // The held descriptor is unchanged across the sync point.
  AppendToFile(Path("f"), "after");
  Forget(f, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 5);
}

// Collects every log line, VLOG lines included, with its severity while it
// lives (the lines a daemon would write to stderr at --stderrthreshold=0
// --v=N).
class AllLogCapture : public absl::LogSink {
 public:
  AllLogCapture() { absl::AddLogSink(this); }
  ~AllLogCapture() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    lines.emplace_back(entry.log_severity(),
                       std::string(entry.text_message()));
  }
  // How many lines at `min` or above contain `text` (all if empty).
  int Count(absl::LogSeverity min, std::string_view text = "") const {
    return static_cast<int>(std::count_if(
        lines.begin(), lines.end(), [&](const auto &line) {
          return line.first >= min && absl::StrContains(line.second, text);
        }));
  }
  std::string Dump() const {
    std::string out;
    for (const auto &line : lines) {
      absl::StrAppend(&out, absl::LogSeverityName(line.first), " ",
                      line.second, "\n");
    }
    return out;
  }
  std::vector<std::pair<absl::LogSeverity, std::string>> lines;
};

// Sets the global verbosity (--v) for the test's scope.
class ScopedVerbosity {
 public:
  explicit ScopedVerbosity(int level)
      : saved_(absl::SetGlobalVLogLevel(level)) {}
  ~ScopedVerbosity() { absl::SetGlobalVLogLevel(saved_); }

 private:
  int saved_;
};

// A writable open refused for the backing file's flag says which flag, in
// the failure the request's log shows at -v=2 (the errno alone is replied).
TEST_F(DirCacheFSTest, RefusedWritableOpenNamesTheBackingFlag) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  int flags = 0;
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor raw_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
  const int raw = *raw_fd;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
  ScopedVerbosity v(2);
  for (const auto &[flag, said] :
       {std::pair{FS_IMMUTABLE_FL, "immutable"},
        std::pair{FS_APPEND_FL, "append-only"}}) {
    const int changed = flags | flag;
    ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &changed), IsOk());
    OutOfBand(f);
    AllLogCapture capture;
    EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM) << said;
    EXPECT_GE(capture.Count(absl::LogSeverity::kInfo,
                            absl::StrCat("the backing file is ", said)),
              1)
        << said << "\n" << capture.Dump();
    ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags), IsOk());
    OutOfBand(f);
  }
  EXPECT_EQ(Release(f, fh).error, 0);
}

// docs/design.md "Logging": each sync point is one INFO line with the rows
// it cleared and its duration; none when nothing was dirty.
TEST_F(ClockTest, ASyncPointIsLoggedAtInfoWithItsRows) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);  // written: dirty
  ASSERT_THAT(Dirty(), Contains(f));
  AllLogCapture capture;
  Tick();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "sync point"), 0)
      << "before the interval has elapsed\n" << capture.Dump();
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  ASSERT_EQ(capture.Count(absl::LogSeverity::kInfo, "sync point"), 1)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "sync point: cleared "
                                                     "1 of 1 dirty rows in "),
            1)
      << capture.Dump();
  Tick();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "sync point"), 1)
      << "nothing dirty, no sync point\n" << capture.Dump();
}

// An fsync's sync point is not announced at INFO (one per call), only at
// --v=1.
TEST_F(ClockTest, AnFsyncdirSyncPointIsLoggedOnlyAtVerbosityOne) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);  // written: dirty
  {
    AllLogCapture capture;
    ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "sync point"), 0)
        << capture.Dump();
  }
  auto [open2, fh2] = Open(f, O_RDWR);
  ASSERT_EQ(open2.error, 0);
  ASSERT_EQ(Release(f, fh2).error, 0);
  ScopedVerbosity v(1);
  AllLogCapture capture;
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "sync point: cleared"), 1)
      << capture.Dump();
}

// The first request to reach the backing after more than 60 seconds says so
// at INFO; a shorter pause does not, and
// neither does a request answered from the cache.
TEST_F(ClockTest, TheFirstBackingAccessAfterAnIdlePeriodIsLoggedAtInfo) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  AllLogCapture capture;
  auto reach_backing = [&] {
    ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
    ASSERT_EQ(Getattr(f).first.error, 0);
  };
  reach_backing();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "idle"), 0)
      << "the first access of the run\n" << capture.Dump();
  clock_.AdvanceTime(absl::Seconds(30));
  reach_backing();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "idle"), 0)
      << "a short pause\n" << capture.Dump();
  clock_.AdvanceTime(absl::Seconds(90));
  EXPECT_EQ(Getattr(f).first.error, 0);  // from the cache: attributes valid
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "idle"), 0)
      << "a request that did not reach the backing\n" << capture.Dump();
  reach_backing();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                          "first backing access after 1m30s idle"),
            1)
      << capture.Dump();
}

// --v=1: one line per request that reached the backing filesystem; --v=2:
// every request with its reply; nothing of either by default.
TEST_F(DirCacheFSTest, VerboseLevelsShowRequestsAndTheirReplies) {
  WriteFile(Path("f"));
  Start();
  {
    AllLogCapture capture;
    auto [lookup, entry] = Lookup(kRootInode, "f");
    ASSERT_EQ(lookup.error, 0);
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "Lookup(ino="), 0)
        << "--v=0\n" << capture.Dump();
  }
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  {
    ScopedVerbosity v(1);
    AllLogCapture capture;
    ASSERT_EQ(Getattr(f).first.error, 0);
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                            absl::StrCat("Getattr(ino=", f,
                                         ") reached the backing: ")),
              1)
        << "--v=1\n" << capture.Dump();
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, ") -> "), 0)
        << "--v=1 shows no replies\n" << capture.Dump();
    ASSERT_EQ(Getattr(f).first.error, 0);  // cached now
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "reached the backing"),
              1)
        << "a cached request is not shown at --v=1\n" << capture.Dump();
  }
  {
    ScopedVerbosity v(2);
    AllLogCapture capture;
    ASSERT_EQ(Getattr(f).first.error, 0);
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                            absl::StrCat("Getattr(ino=", f, ") -> OK")),
              1)
        << "--v=2\n" << capture.Dump();
    EXPECT_EQ(Getattr(9999).first.error, -ESTALE);
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "Getattr(ino=9999) -> "),
              1)
        << capture.Dump();
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                            "Getattr(ino=9999) -> OK"),
              0)
        << "a failed reply shows its status\n" << capture.Dump();
  }
}

// docs/style.md 1.7: an error dcfs produced is logged once, at ERROR, by
// the handler that replies, and not again by the function below it. Here a
// mkdir whose backing change succeeds but whose record fails (the old code
// logged "created ... could not record it" at WARNING and the handler
// logged the same status at ERROR). Since step 11.4 it replies EEXIST
// (CreatedButNotCompleted), an errno dcfs chose.
TEST_F(DirCacheFSTest, AnErrorDcfsProducedIsLoggedOnceAtError) {
  Start();
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_new_inodes BEFORE INSERT ON "
                       "inodes BEGIN SELECT RAISE(ABORT, 'no inserts'); END"),
              IsOk());
  AllLogCapture capture;
  auto [reply, id] = Mkdir(kRootInode, "d");
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_new_inodes"), IsOk());
  EXPECT_EQ(reply.error, -EEXIST);
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning), 1) << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kError,
                          "Could not complete a create that reached the "
                          "backing filesystem"),
            1)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kError, "for d in directory 1"),
            1)
      << capture.Dump();
}

// An errno the backing filesystem answered with is the answer, not a
// failure of dcfs: no ERROR line. (A mkdir of an existing name fails in
// mkdirat with EEXIST, which CreateChild returns as a Status.)
TEST_F(DirCacheFSTest, AnErrnoFromTheBackingIsNotLoggedAtError) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("e"), 0755), IsOk());
  Start();
  AllLogCapture capture;
  EXPECT_EQ(Mkdir(kRootInode, "e").first.error, -EEXIST);
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning), 0) << capture.Dump();
}

// The create happened but recording it failed with an errno of the backing
// filesystem: still dcfs's failure to record a backing change, so logged
// once at ERROR, not dropped as a forwarded errno.
TEST_F(DirCacheFSTest, ABackingErrnoWhileRecordingACreateIsLoggedAtError) {
  Start();
  // The first statx of the request is RecordNewChild's probe of the new
  // directory.
  StatxFailure() = EIO;
  AllLogCapture capture;
  auto [reply, id] = Mkdir(kRootInode, "d");
  EXPECT_NE(reply.error, 0) << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning), 1) << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kError,
                          "while probing the new child"),
            1)
      << capture.Dump();
}

// A DcfsErrnoToStatus status, through the whole request: one ERROR line, at
// the handler (RefuseReservedIno no longer logs itself).
TEST_F(DirCacheFSTest, ADcfsErrnoStatusIsLoggedOnceAtErrorByTheHandler) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(struct statx stx,
                       syscalls::statx(AT_FDCWD, Path("f"), 0, STATX_INO));
  FakeInodeNumbers()[stx.stx_ino] = uint64_t{1} << 63;
  AllLogCapture capture;
  EXPECT_EQ(Lookup(kRootInode, "f").first.error, -ENOTSUP) << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning), 1) << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kError, "2^63"), 1)
      << capture.Dump();
}

// A failed record of the access time leaves a stale atime present: ERROR.
// (Step 23.8: the record is the held fill at FLUSH; when it cannot record,
// and cannot mark the attributes unknown either.)
TEST_F(DirCacheFSTest, AFailedAtimeRecordIsLoggedAtError) {
  WriteFile(Path("f"));
  AppendToFile(Path("f"), "contents");
  SetTimes(Path("f"), 2 * 86400, 3 * 86400);
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"));
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_updates BEFORE UPDATE ON "
                       "inodes BEGIN SELECT RAISE(ABORT, 'no updates'); END"),
              IsOk());
  AllLogCapture capture;
  EXPECT_EQ(Flush(f, fh).error, 0);
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_updates"), IsOk());
  EXPECT_EQ(capture.Count(absl::LogSeverity::kError,
                          "Flush: could not record the attributes"),
            1)
      << capture.Dump();
  EXPECT_EQ(Release(f, fh).error, 0);
}

// --v=3: the SQL statements, and one line per step with its outcome.
TEST_F(DirCacheFSTest, VerbosityThreeShowsSqlAndSteps) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  {
    AllLogCapture capture;
    ASSERT_EQ(Getattr(f).first.error, 0);
    EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo, "sqlite3_step"), 0)
        << "--v=0\n" << capture.Dump();
  }
  ScopedVerbosity v(3);
  AllLogCapture capture;
  ASSERT_EQ(Getattr(f).first.error, 0);
  EXPECT_GE(capture.Count(absl::LogSeverity::kInfo, "sqlite3_step: SELECT"), 1)
      << capture.Dump();
  EXPECT_GE(capture.Count(absl::LogSeverity::kInfo, "sqlite3_step: -> "), 1)
      << capture.Dump();
}

// The missing-capability branch of Init: a kernel that does not offer
// passthrough (the harness's INIT does not) is a WARNING, once.
TEST_F(DirCacheFSTest, NoPassthroughIsAWarningAtInit) {
  AllLogCapture capture;
  Start();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning,
                          "FUSE_CAP_PASSTHROUGH NOT granted"),
            1)
      << capture.Dump();
}

// Records the lifetime steps with their argument.
class LifetimeSteps : public ProtocolEvents {
 public:
  void LifetimeChanged(Context &, events::Ino, events::LifetimeStep step,
                       uint64_t arg, events::LifetimeFn) override {
    steps.emplace_back(step, arg);
  }
  std::vector<std::pair<events::LifetimeStep, uint64_t>> steps;
};

// Recovery says what it probed: the INFO summary counts the rows probed, the
// ones gone and the ones that could not be probed.
TEST_F(DirCacheFSTest, RecoveryLogsItsProbeSummary) {
  WriteFile(Path("f"));
  WriteFile(Path("kept"));
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId kept, Id("kept"));
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0), IsOk());
  const InodeId dirty[] = {f, kept};
  ASSERT_THAT(cache::MarkDirty(ctx_, dirty), IsOk());
  AllLogCapture capture;
  ASSERT_THAT(Restart("boot"), IsOk());
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                          "recovery: probed 2 recovered rows, 1 gone, 0 "
                          "could not be probed"),
            1)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning, "forgot 1 rows"), 1)
      << capture.Dump();
}

// A recovered row whose object is gone for good (its handle opens ENOENT) is
// forgotten by the probe and counted as gone, with the lifetime model's
// probe (formal/lifetime.tla's ProbeRow) told so.
TEST_F(DirCacheFSTest, RecoveryCountsARowWhoseObjectIsGone) {
  WriteFile(Path("f"));
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  const InodeId dirty[] = {f};
  ASSERT_THAT(cache::MarkDirty(ctx_, dirty), IsOk());
  LifetimeSteps steps;
  observers_->Add(&steps);
  OpenByHandleFailures() = {ENOENT};
  AllLogCapture capture;
  ASSERT_THAT(Restart("boot"), IsOk());
  observers_->Remove(&steps);
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                          "recovery: probed 1 recovered rows, 1 gone, 0 "
                          "could not be probed"),
            1)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning, "forgot 1 rows"), 1)
      << capture.Dump();
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(steps.steps,
              Contains(::testing::Pair(events::LifetimeStep::kProbed, 1u)))
      << "the probe reports the row as gone";
}

// A row that cannot be probed (here its deletion fails) is an ERROR of its
// own and counts in the summary.
TEST_F(DirCacheFSTest, RecoveryCountsTheRowsItCouldNotProbe) {
  WriteFile(Path("f"));
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0), IsOk());
  const InodeId dirty[] = {f};
  ASSERT_THAT(cache::MarkDirty(ctx_, dirty), IsOk());
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_delete BEFORE DELETE ON inodes "
                       "BEGIN SELECT RAISE(ABORT, 'no deletes'); END"),
              IsOk());
  AllLogCapture capture;
  ASSERT_THAT(Restart("boot"), IsOk());
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_delete"), IsOk());
  EXPECT_EQ(capture.Count(absl::LogSeverity::kError,
                          "could not probe recovered inode"),
            1)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kInfo,
                          "recovery: probed 1 recovered rows, 0 gone, 1 could "
                          "not be probed"),
            1)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning, "forgot"), 0)
      << "no row was forgotten\n" << capture.Dump();
  EXPECT_THAT(cache::GetAttr(ctx_, f), IsOk());  // Left for next time.
  ASSERT_THAT(cache::DeleteInode(ctx_, f), IsOk());
}

// A row invalidated while a mutation's phase 3 refreshes it (NotFound: it is
// gone, nothing to record) is not "a backing change that could not be
// recorded".
TEST_F(DirCacheFSTest, ARowGoneDuringPhase3IsNotLoggedAsAFailure) {
  WriteFile(Path("a"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "a");
  ASSERT_EQ(lookup.error, 0);
  const InodeId a = static_cast<InodeId>(entry.nodeid);
  // The first statx after the rename is the root's refresh; the row of the
  // renamed file goes then, before its own refresh.
  StatxHook() = [&] { EXPECT_THAT(cache::DeleteInode(ctx_, a), IsOk()); };
  AllLogCapture capture;
  EXPECT_EQ(Rename(kRootInode, "a", kRootInode, "b").error, 0)
      << capture.Dump();
  EXPECT_EQ(capture.Count(absl::LogSeverity::kWarning), 0) << capture.Dump();
}

// Step 23.8: a listing stamps the access time the backing mount's rule
// gives (relatime: if the cached one is not after the modification or
// change time, or is a day old) in the cache, from the injected clock, and
// never on the backing filesystem, whose listing was dcfs's own population.
// A later refresh from the backing filesystem keeps the later stamp, an
// explicit SETATTR replaces it, and it survives a restart.
TEST_F(ClockTest, ReaddirStampsTheAtimeInTheCacheOnly) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  WriteFile(Path("d/f"));
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> names, List(d, false));
  ASSERT_THAT(names, Contains("f"));
  const absl::Time backing = BackingAtime(Path("d"));
  // The simulated clock is months after the directory's times.
  const absl::Time first = clock_.TimeNow();
  EXPECT_EQ(RowAtime(d), first);
  // Within the day: unchanged, by either kind of listing.
  clock_.AdvanceTime(absl::Hours(2));
  ASSERT_THAT(List(d, true), IsOk());
  EXPECT_EQ(RowAtime(d), first);
  // A day later: the new time.
  clock_.AdvanceTime(absl::Hours(23));
  ASSERT_THAT(List(d, false), IsOk());
  const absl::Time second = clock_.TimeNow();
  EXPECT_EQ(RowAtime(d), second);
  EXPECT_EQ(BackingAtime(Path("d")), backing) << "written to the backing";

  // A create in d: phase 3 re-reads d's attributes from the backing
  // filesystem, whose atime is older; the stamp stays.
  Created g = Create(d, "g", O_WRONLY);
  ASSERT_EQ(g.reply.error, 0);
  ASSERT_EQ(Release(g.id, g.fh).error, 0);
  EXPECT_EQ(RowAtime(d), second);

  // An explicit access time replaces it, older or not.
  const absl::Time set = absl::FromUnixSeconds(1'000'000'000);
  ASSERT_EQ(SetAtime(d, set).error, 0);
  EXPECT_EQ(BackingAtime(Path("d")), set);
  EXPECT_EQ(RowAtime(d), set);
  // Then a listing stamps again (not after mtime).
  ASSERT_THAT(List(d, false), IsOk());
  EXPECT_EQ(RowAtime(d), second);

  // The next process finds the stamp in the row.
  ASSERT_THAT(Restart("boot"), IsOk());
  EXPECT_EQ(RowAtime(d), second);
}

// Step 23.8: so does a READLINK of a symlink, whose target the cache serves.
TEST_F(ClockTest, ReadlinkStampsTheAtimeInTheCacheOnly) {
  ASSERT_THAT(syscalls::symlinkat("target", AT_FDCWD, Path("s")), IsOk());
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId s, Id("s"));
  ASSERT_EQ(Send(FUSE_READLINK, static_cast<uint64_t>(s), "").error, 0);
  const absl::Time backing = BackingAtime(Path("s"));
  clock_.AdvanceTime(absl::Hours(48));
  ASSERT_EQ(Send(FUSE_READLINK, static_cast<uint64_t>(s), "").error, 0);
  EXPECT_EQ(RowAtime(s), clock_.TimeNow());
  EXPECT_EQ(BackingAtime(Path("s")), backing) << "written to the backing";
}

// Step 23.8: the atime-only dirty rows (a cold read-only open's, a held
// fill's) do not drive the periodic sync point (its syncfs would force the
// backing filesystem's lazy atime write-back, which lazytime defers by a
// day); one that runs for a mutation clears them, but keeps the row of a
// file still held, until its release records the truth.
TEST_F(ClockTest, AtimeOnlyDirtyRowsDoNotDriveSyncPoints) {
  WriteFile(Path("f"));
  AppendToFile(Path("f"), "contents");
  WriteFile(Path("g"));
  WriteFile(Path("h"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId g, Id("g"));
  ASSERT_OK_AND_ASSIGN(InodeId h, Id("h"));
  // A first sync point settles Context::dirty.any (conservatively true
  // until one has run).
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  ASSERT_THAT(Dirty(), ::testing::IsEmpty());
  int syncs = 0;
  SyncfsHook() = [&] { ++syncs; };  // (Once.)
  // A cold read-only open marks the row dirty, atime only.
  {
    auto [open, fh] = Open(f, O_RDONLY);
    ASSERT_EQ(open.error, 0);
    EXPECT_THAT(Dirty(), Contains(f));
    ASSERT_EQ(Release(f, fh).error, 0);
  }
  EXPECT_THAT(Dirty(), Contains(f));
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  EXPECT_EQ(syncs, 0) << "a sync point for atime-only rows";
  EXPECT_THAT(Dirty(), Contains(f));

  // h held across a sync point that a mutation (g's chmod) drives.
  auto [open, fh] = Open(h, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Chmod(g, 0600).error, 0);
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  EXPECT_EQ(syncs, 1);
  EXPECT_THAT(Dirty(), Not(Contains(f))) << "released: cleared";
  EXPECT_THAT(Dirty(), Not(Contains(g)));
  EXPECT_THAT(Dirty(), Contains(h)) << "held: kept";
  EXPECT_EQ(Release(h, fh).error, 0);
  EXPECT_THAT(Dirty(), Contains(h)) << "recorded at the release";
}

// Step 23.8 (review): a mutation of a file held open read-only leaves a
// row the sync point must keep (the file is open), but its syncfs covered
// the mutation: the row stays as atime-only, and drives no further sync
// point.
TEST_F(ClockTest, AMutationOfAnOpenFileDrivesOneSyncPoint) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  ASSERT_THAT(Dirty(), ::testing::IsEmpty());
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  EXPECT_EQ(Chmod(f, 0600).error, 0);
  int syncs = 0;
  SyncfsHook() = [&] { ++syncs; };
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  EXPECT_EQ(syncs, 1) << "the chmod's";
  SyncfsHook() = [&] { ++syncs; };
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  EXPECT_EQ(syncs, 1) << "none for the row kept only because f is open";
  EXPECT_THAT(Dirty(), Contains(f));
  EXPECT_EQ(Release(f, fh).error, 0);
  SyncfsHook() = {};
}

// Step 23.8 (review): atime-only rows do drive a sync point once they are
// older than the kernel's dirtytime expiry (by then it writes the access
// times back anyway).
TEST_F(ClockTest, AtimeOnlyRowsDriveASyncPointAfterTheDirtytimeExpiry) {
  ctx_.dirty.atime_expiry = absl::Seconds(30);
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  ASSERT_THAT(Dirty(), ::testing::IsEmpty());
  {
    auto [open, fh] = Open(f, O_RDONLY);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(f, fh).error, 0);
  }
  ASSERT_THAT(Dirty(), Contains(f));
  int syncs = 0;
  SyncfsHook() = [&] { ++syncs; };
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  EXPECT_EQ(syncs, 0) << "within the expiry";
  clock_.AdvanceTime(absl::Seconds(30));
  Tick();
  EXPECT_EQ(syncs, 1) << "past it";
  EXPECT_THAT(Dirty(), ::testing::IsEmpty());
  SyncfsHook() = {};
}

// Step 23.8 (review): a cold open during a sync point's syncfs adds a row
// that moves no fill guard; ClearDirty's one-statement fast path must not
// delete it (Context::dirty.inserts).
TEST_F(ClockTest, ASyncPointKeepsARowAddedDuringItsSyncfs) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId g, Id("g"));
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  ASSERT_THAT(Dirty(), ::testing::IsEmpty());
  EXPECT_EQ(Chmod(g, 0600).error, 0);  // What drives the sync point.
  uint64_t fh = 0;
  SyncfsHook() = [&] { fh = Open(f, O_RDONLY).second; };
  clock_.AdvanceTime(absl::Seconds(6));
  Tick();
  ASSERT_NE(fh, 0u) << "the open during the syncfs";
  EXPECT_THAT(Dirty(), Contains(f));
  EXPECT_EQ(Release(f, fh).error, 0);
}

// Step 23.8 (review): an explicit access time that the backing filesystem
// refuses (an immutable directory: EPERM) leaves the cached stamp.
TEST_F(ClockTest, AFailedAtimeSetKeepsTheStamp) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_THAT(List(d, false), IsOk());
  const absl::Time stamp = RowAtime(d);
  ASSERT_EQ(stamp, clock_.TimeNow());
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       syscalls::openat(AT_FDCWD, Path("d"), O_RDONLY));
  int flags = 0;
  ASSERT_THAT(syscalls::ioctl(*fd, FS_IOC_GETFLAGS, &flags), IsOk());
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_THAT(syscalls::ioctl(*fd, FS_IOC_SETFLAGS, &immutable), IsOk());
  EXPECT_EQ(SetAtime(d, absl::FromUnixSeconds(1'000'000'000)).error, -EPERM);
  EXPECT_EQ(RowAtime(d), stamp);
  ASSERT_THAT(syscalls::ioctl(*fd, FS_IOC_SETFLAGS, &flags), IsOk());
}

// Step 23.8: a crash while a file is open and has been read, before dcfs
// looked at it again: the cold open's dirty row makes the restart forget the
// access time it had before the read.
TEST_F(DirCacheFSTest, CrashWhileAFileIsReadForgetsItsAtime) {
  WriteFile(Path("f"));
  AppendToFile(Path("f"), "contents");
  const struct timespec old[2] = {{.tv_sec = 1'000'000'000, .tv_nsec = 0},
                                  {.tv_sec = 999'000'000, .tv_nsec = 0}};
  ASSERT_THAT(syscalls::utimensat(AT_FDCWD, Path("f"), old, 0), IsOk());
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ReadBacking(Path("f"));
  const absl::Time read = BackingAtime(Path("f"));
  ASSERT_GT(read, absl::FromUnixSeconds(1'000'000'000));
  // The daemon dies with the file open.
  ASSERT_THAT(Restart("boot"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid)
      << "recovered: to be read again from the backing filesystem, which has "
      << read << "; the row says " << absl::TimeFromTimespec(attr.st.st_atim);
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
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor held_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_PATH));
  const int held = *held_fd;
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);

  Reply get = Ioctl(f, FS_IOC_GETFLAGS, "", sizeof(int));
  EXPECT_EQ(get.error, 0);
  auto [in, in_fh] = Open(src, O_RDONLY);
  auto [out, out_fh] = Open(f, O_RDWR);
  ASSERT_EQ(in.error, 0);
  ASSERT_EQ(out.error, 0);
  EXPECT_EQ(CopyFileRange(src, in_fh, f, out_fh, 100), 3);
  ASSERT_OK_AND_ASSIGN(struct stat st, syscalls::fstat(held));
  EXPECT_EQ(st.st_size, 3);
  // Set the flags it has (ext4 refuses to clear its extents flag).
  const std::string set = get.payload.substr(sizeof(struct fuse_ioctl_out));
  ASSERT_EQ(set.size(), sizeof(int));
  EXPECT_EQ(Ioctl(f, FS_IOC_SETFLAGS, set, 0).error, 0);
  EXPECT_EQ(Release(f, out_fh).error, 0);
  EXPECT_EQ(Release(src, in_fh).error, 0);
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
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor raw_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
  const int raw = *raw_fd;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &immutable), IsOk());
  auto [ro, ro_fh] = Open(f, O_RDONLY);  // Shared fd: read-only.
  ASSERT_EQ(ro.error, 0);
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags), IsOk());
  OutOfBand(f);
  auto [rw, rw_fh] = Open(f, O_WRONLY);
  ASSERT_EQ(rw.error, 0);

  struct fuse_fallocate_in falloc = {};
  falloc.fh = rw_fh;
  falloc.length = 4096;
  std::string body;
  AppendBytes(body, falloc);
  EXPECT_EQ(Send(FUSE_FALLOCATE, static_cast<uint64_t>(f), body).error, 0);
  EXPECT_EQ(testonly::FileSize(Path("f")).value_or(-1), 4096);
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
  absl::StatusOr<std::vector<std::string>> fds =
      testonly::ListDirectory("/proc/self/fd");
  EXPECT_THAT(fds, IsOk());
  // The listing's own descriptor is among them, as in the iterator's.
  return fds.ok() ? static_cast<int>(fds->size()) : 0;
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
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0),
              IsOk());  // Behind dcfs's back.
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
  ASSERT_OK_AND_ASSIGN(struct rlimit saved, syscalls::getrlimit(RLIMIT_NOFILE));
  int lowest_free = -1;
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor probe, syscalls::dup(0));
    lowest_free = *probe;  // closed again at the end of the block
  }
  struct rlimit tight = saved;
  tight.rlim_cur = static_cast<rlim_t>(lowest_free);
  ASSERT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, tight), IsOk());
  Reply release = Release(f, fh);  // Cannot hold a descriptor now.
  ASSERT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, saved), IsOk());
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

  ASSERT_OK_AND_ASSIGN(struct rlimit saved, syscalls::getrlimit(RLIMIT_NOFILE));
  int lowest_free = -1;
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor probe, syscalls::dup(0));
    lowest_free = *probe;  // closed again at the end of the block
  }
  struct rlimit tight = saved;
  tight.rlim_cur = static_cast<rlim_t>(lowest_free + kCap + kReaders + 2);
  ASSERT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, tight), IsOk());
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
  ASSERT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, saved), IsOk());

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
  auto write_file = [&](InodeId id) {
    auto [open, fh] = Open(id, O_RDWR);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(id, fh).error, 0);
  };
  const int base = OpenFdCount();
  write_file(ids["f"]);
  EXPECT_EQ(OpenFdCount(), base + 1);
  Forget(ids["f"], 1);
  EXPECT_EQ(OpenFdCount(), base) << "after the last FORGET";

  write_file(ids["g"]);
  ASSERT_EQ(Unlink(kRootInode, "g").error, 0);
  EXPECT_EQ(OpenFdCount(), base + 1) << "the removed record's alone";
  Forget(ids["g"], 1);
  EXPECT_EQ(OpenFdCount(), base) << "after the removed file's last FORGET";

  write_file(ids["h"]);
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
  observers_->Add(&count);
  BatchForget({{ids[0], 1}, {ids[1], 1}, {ids[2], 1}});
  EXPECT_EQ(count.begun, 1);
  EXPECT_EQ(count.synced_begun, 1);
  EXPECT_EQ(count.ids_named, 3);
  // DESTROY likewise, for the rest.
  EXPECT_EQ(Send(FUSE_DESTROY, 0, "").error, 0);
  EXPECT_EQ(count.begun, 2);
  EXPECT_EQ(count.ids_named, 6);
  observers_->Remove(&count);
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
  ASSERT_OK_AND_ASSIGN(struct rlimit saved, syscalls::getrlimit(RLIMIT_NOFILE));
  int lowest_free = -1;
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor probe, syscalls::dup(0));
    lowest_free = *probe;  // closed again at the end of the block
  }
  struct rlimit tight = saved;
  tight.rlim_cur = static_cast<rlim_t>(lowest_free);
  ASSERT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, tight), IsOk());
  Reply release = Release(f, again_fh);  // No new descriptor possible.
  ASSERT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, saved), IsOk());
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
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor held_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_PATH));
  const int held = *held_fd;
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
  ASSERT_OK_AND_ASSIGN(struct stat st, syscalls::fstat(held));
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
  // A fallocate has no row to begin a mutation on: it goes through the open's
  // descriptor, and the object grows.
  struct fuse_fallocate_in falloc = {};
  falloc.fh = fh;
  falloc.length = 4096;
  std::string falloc_body;
  AppendBytes(falloc_body, falloc);
  EXPECT_EQ(Send(FUSE_FALLOCATE, static_cast<uint64_t>(f), falloc_body).error,
            0);
  ASSERT_OK_AND_ASSIGN(struct stat grown, syscalls::fstat(held));
  EXPECT_EQ(grown.st_size, 4096);
  EXPECT_EQ(Release(f, fh).error, 0);
  EXPECT_FALSE(fs_->HasOpenFiles(f));
  // Still answered after the release, until the kernel forgets it.
  EXPECT_EQ(Getattr(f).first.error, 0);
  Forget(f, 1);
  EXPECT_EQ(Getattr(f).first.error, -ESTALE);
  EXPECT_EQ(Chmod(f, S_IFREG | 0644).error, -ESTALE);
}

TEST_F(DirCacheFSTest, RemovedDirectoryCanBeChanged) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "d");
  ASSERT_EQ(lookup.error, 0);
  const InodeId d = static_cast<InodeId>(entry.nodeid);
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor held_fd,
      syscalls::openat(AT_FDCWD, Path("d"), O_RDONLY | O_DIRECTORY));
  const int held = *held_fd;
  std::string body = "d";
  body.push_back('\0');
  ASSERT_EQ(Send(FUSE_RMDIR, kRootInode, body).error, 0);

  EXPECT_EQ(Chmod(d, S_IFDIR | 0700).error, 0);
  EXPECT_EQ(Setxattr(d, "user.k", "v").error, 0);
  ASSERT_OK_AND_ASSIGN(struct stat st, syscalls::fstat(held));
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
}

// Step 23.9: LINK of a removed object answers what the backing filesystem
// answers for the same object: a link of the descriptor dcfs holds
// (linkat AT_EMPTY_PATH). An unlinked file: ENOENT (vfs_link refuses an
// inode with no link that O_TMPFILE did not make linkable); a removed
// directory: EPERM (no directory is hard-linked). Each is compared with the
// same operation on the backing filesystem, through a descriptor the test
// holds on the same object.
TEST_F(DirCacheFSTest, LinkOfARemovedObjectAnswersAsTheBacking) {
  WriteFile(Path("f"));
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  Start();
  auto [lookup_f, entry_f] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup_f.error, 0);
  auto [lookup_d, entry_d] = Lookup(kRootInode, "d");
  ASSERT_EQ(lookup_d.error, 0);
  ASSERT_OK_AND_ASSIGN(FileDescriptor f_fd,
                       syscalls::openat(AT_FDCWD, Path("f"), O_PATH));
  ASSERT_OK_AND_ASSIGN(FileDescriptor d_fd,
                       syscalls::openat(AT_FDCWD, Path("d"), O_PATH));
  ASSERT_OK_AND_ASSIGN(FileDescriptor root_fd,
                       syscalls::openat(AT_FDCWD, Path(""), O_RDONLY));
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);
  std::string rmdir_body = "d";
  rmdir_body.push_back('\0');
  ASSERT_EQ(Send(FUSE_RMDIR, kRootInode, rmdir_body).error, 0);

  const int f_backing = ErrnoOf(
      syscalls::linkat(*f_fd, "", *root_fd, "f-back", AT_EMPTY_PATH));
  EXPECT_EQ(f_backing, ENOENT) << "the backing filesystem's answer";
  EXPECT_EQ(Link(static_cast<InodeId>(entry_f.nodeid), kRootInode, "f-link")
                .error,
            -f_backing)
      << "an unlinked file";
  const int d_backing = ErrnoOf(
      syscalls::linkat(*d_fd, "", *root_fd, "d-back", AT_EMPTY_PATH));
  EXPECT_EQ(d_backing, EPERM) << "the backing filesystem's answer";
  EXPECT_EQ(Link(static_cast<InodeId>(entry_d.nodeid), kRootInode, "d-link")
                .error,
            -d_backing)
      << "a removed directory";
  // Neither name appeared, and the cache says so after the failed links.
  EXPECT_EQ(ErrnoOf(syscalls::fstatat(AT_FDCWD, Path("f-link"),
                                     AT_SYMLINK_NOFOLLOW)
                        .status()),
            ENOENT);
  EXPECT_EQ(Lookup(kRootInode, "f-link").second.nodeid, 0u)
      << "a negative entry (nodeid 0)";
  EXPECT_EQ(Lookup(kRootInode, "d-link").second.nodeid, 0u)
      << "a negative entry (nodeid 0)";
}

// Step 23.9 (review): the one removed object Linux links back: an
// O_TMPFILE file (not O_EXCL) that was closed without a name while the
// kernel still holds it (an O_PATH descriptor through /proc/self/fd). Its
// last release retired its row into a removed record; the LINK succeeds on
// the backing filesystem as it does there, and is recorded as a create is:
// the record's nodeid comes back with its row, the name links to it, and
// the row is dirty (a power loss that loses the link must not leave it
// valid).
TEST_F(DirCacheFSTest, LinkOfAClosedTmpfileGivesItsNodeidAName) {
  Start();
  // The backing filesystem's own answer, for the same sequence.
  ASSERT_OK_AND_ASSIGN(FileDescriptor ref,
                       syscalls::openat(AT_FDCWD, Path(""),
                                        O_TMPFILE | O_RDWR, 0644));
  ASSERT_OK_AND_ASSIGN(FileDescriptor ref_path,
                       syscalls::openat(AT_FDCWD,
                                        absl::StrCat("/proc/self/fd/", *ref),
                                        O_PATH));
  ref = FileDescriptor();
  EXPECT_THAT(syscalls::linkat(*ref_path, "", AT_FDCWD, Path("ref"),
                               AT_EMPTY_PATH),
              IsOk())
      << "the backing filesystem links it";

  Created tmp = Tmpfile(kRootInode, O_RDWR);
  ASSERT_EQ(tmp.reply.error, 0);
  EXPECT_EQ(Release(tmp.id, tmp.fh).error, 0);  // Closed, no name: retired.
  EXPECT_THAT(cache::GetAttr(ctx_, tmp.id).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  auto [reply, entry] = LinkEntry(tmp.id, kRootInode, "t");
  ASSERT_EQ(reply.error, 0) << "as the backing filesystem";
  EXPECT_EQ(entry.nodeid, static_cast<uint64_t>(tmp.id)) << "its own nodeid";
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, tmp.id));
  EXPECT_EQ(attr.backing_ino, InoOf(Path("t")));
  EXPECT_EQ(Cached(kRootInode, "t"),
            std::make_pair(LookupResult::Kind::kFound, attr.backing_ino));
  EXPECT_THAT(Dirty(), Contains(tmp.id)) << "a created row is dirty";
}

// --- Boundary stubs (step 23.5) -------------------------------------------
//
// A mount point or subvolume boundary below the source is served as a stub
// directory: listed, looked up as a directory with a nodeid at or above
// 2^63 (as its inode number too), and anything inside it ENOTSUP; renaming
// it, or a rename or link into it, EXDEV.

constexpr uint64_t kFirstStubNodeid = uint64_t{1} << 63;

TEST_F(DirCacheFSTest, BoundaryIsAStubDirectory) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d/mp"), 0755), IsOk());
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
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("d/mp/inside")), IsOk());
  EXPECT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/mp/x")).ok());
  EXPECT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/mp/f")).ok());
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("d/f")), IsOk());
  EXPECT_FALSE(syscalls::fstatat(AT_FDCWD, Path("d/mp2")).ok());
}

// Every refused operation on a stub, with its errno (review L2): removing
// it is EBUSY (as for a mount point), linking it EXDEV, and anything
// inside it ENOTSUP.
TEST_F(DirCacheFSTest, EveryOperationOnAStubIsRefused) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d/mp"), 0755), IsOk());
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
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("d/mp")), IsOk());
}

// A stub whose row is gone (its dentry relisted or recovered) is a stale
// nodeid: ESTALE, so the kernel's path walk retries with LOOKUP_REVAL and
// finds what the name is now, rather than ENOTSUP (review L3).
TEST_F(DirCacheFSTest, AGoneStubIsStale) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mp"), 0755), IsOk());
  Start();
  MountBelow("mp");
  auto [lookup, entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(lookup.error, 0);
  const InodeId stub = static_cast<InodeId>(entry.nodeid);
  // Relisted as something else (here: gone).
  ASSERT_THAT(cache::SetNegative(ctx_, kRootInode, "mp"), IsOk());
  EXPECT_EQ(Lookup(stub, "x").first.error, -ESTALE);
  EXPECT_EQ(Opendir(stub).error, -ESTALE);
  EXPECT_EQ(Mkdir(stub, "x").first.error, -ESTALE);
  EXPECT_EQ(Rename(kRootInode, "a", stub, "a").error, -ESTALE);
}

// The stub's nodeid is recorded with its dentry: a lookup resolved by a
// single probe (an unknown name in a complete listing) gets a stub, and a
// name that is no longer a boundary drops it (the old nodeid is stale).
TEST_F(DirCacheFSTest, BoundaryStubIsRecordedWithItsDentry) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mp"), 0755), IsOk());
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
  ASSERT_EQ(Cached(kRootInode, "mp").first, LookupResult::Kind::kUnknown);
  auto [second, second_entry] = Lookup(kRootInode, "mp");
  ASSERT_EQ(second.error, 0);
  EXPECT_EQ(second_entry.nodeid, first_entry.nodeid);
  EXPECT_EQ(second_entry.generation, first_entry.generation);
  EXPECT_EQ(Cached(kRootInode, "mp").first, LookupResult::Kind::kRefused);

  // Once the name is no longer a boundary, the stub goes with the refusal.
  ASSERT_THAT(syscalls::umount2(Path("mp"), MNT_DETACH), IsOk());
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

// An out-of-band change in a directory holding two boundaries forgets
// their refusals (ForgetNegativeDentries, before its relisting): each
// stub's nodeid still answers for its own boundary meanwhile, and the
// names refused again, in the other order, keep their nodeids and
// generations; no nodeid passes to the other boundary
// (formal/findings/lifetime_stub_nodeid_reused, fixed).
TEST_F(DirCacheFSTest, ForgottenStubsKeepTheirNodeids) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mp1"), 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("mp2"), 0755), IsOk());
  Start();
  MountBelow("mp1");
  MountBelow("mp2");
  ASSERT_THAT(syscalls::fchmodat(AT_FDCWD, Path("mp1"), 0700, 0), IsOk());
  ASSERT_THAT(syscalls::fchmodat(AT_FDCWD, Path("mp2"), 0755, 0), IsOk());
  auto [l1, e1] = Lookup(kRootInode, "mp1");
  auto [l2, e2] = Lookup(kRootInode, "mp2");
  ASSERT_EQ(l1.error, 0);
  ASSERT_EQ(l2.error, 0);
  ASSERT_NE(e1.nodeid, e2.nodeid);
  const InodeId stub1 = static_cast<InodeId>(e1.nodeid);
  const InodeId stub2 = static_cast<InodeId>(e2.nodeid);

  ASSERT_THAT(cache::ForgetNegativeDentries(ctx_, kRootInode), IsOk());
  auto [g1, a1] = Getattr(stub1);
  ASSERT_EQ(g1.error, 0);
  EXPECT_EQ(a1.mode & 07777, 0700u);

  auto [m2, f2] = Lookup(kRootInode, "mp2");
  auto [m1, f1] = Lookup(kRootInode, "mp1");
  ASSERT_EQ(m2.error, 0);
  ASSERT_EQ(m1.error, 0);
  EXPECT_EQ(f1.nodeid, e1.nodeid);
  EXPECT_EQ(f1.generation, e1.generation);
  EXPECT_EQ(f2.nodeid, e2.nodeid);
  EXPECT_EQ(f2.generation, e2.generation);
  auto [h1, b1] = Getattr(stub1);
  auto [h2, b2] = Getattr(stub2);
  ASSERT_EQ(h1.error, 0);
  ASSERT_EQ(h2.error, 0);
  EXPECT_EQ(b1.mode & 07777, 0700u);
  EXPECT_EQ(b2.mode & 07777, 0755u);
}

// Backing inode numbers at or above 2^63 are the stubs' (and, from Phase
// 14, nodeids are backing inode numbers): an object with one is refused,
// ENOTSUP, not served under a nodeid a stub may hold.
TEST_F(DirCacheFSTest, BackingInodeNumbersInTheStubRangeAreRefused) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  WriteFile(Path("d/big"));
  WriteFile(Path("d/small"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  FakeInodeNumbers()[InoOf(Path("d/big"))] = kFirstStubNodeid + 5;
  // Listing d probes every name, so the whole listing is refused (and
  // recorded as nothing: neither name is cached absent).
  AllLogCapture capture;
  EXPECT_EQ(Lookup(d, "big").first.error, -ENOTSUP);
  EXPECT_GE(capture.Count(
                absl::LogSeverity::kError,
                absl::StrCat("Backing inode number ", kFirstStubNodeid + 5,
                             " of ")),
            1)
      << "the ERROR names the number after its label\n" << capture.Dump();
  EXPECT_EQ(Lookup(d, "small").first.error, -ENOTSUP);
  EXPECT_EQ(ErrnoOf(List(d, false).status()), ENOTSUP);
  EXPECT_EQ(Cached(d, "big").first, LookupResult::Kind::kUnknown);
  EXPECT_EQ(Cached(d, "small").first, LookupResult::Kind::kUnknown);

  // A number below the range is served again.
  FakeInodeNumbers().clear();
  auto [lookup, entry] = Lookup(d, "big");
  ASSERT_EQ(lookup.error, 0);
  EXPECT_EQ(entry.attr.ino, InoOf(Path("d/big")));
}

// --- Nodeids' lifetimes (formal/lifetime.tla) ------------------------------
//
// Forged FORGETs with any nlookup, batches, removals and a crash: what dcfs
// keeps for a nodeid (its row, a removed record, the written_ entry and its
// held descriptor) must go exactly when the model says. Each records its
// trace, which //dcfs:dir_cache_fs_trace_test validates against
// formal/LifetimeTrace.tla (formal/trace_tests/life_*.log keep some of them
// for the known-bug variants).

// A FORGET of part of a written file's lookups keeps its held descriptor:
// a store after it is seen at the last FORGET.
TEST_F(DirCacheFSTest, NonFinalForgetKeepsTheHeldDescriptor) {
  WriteFile(Path("f"));
  Start();
  StartTrace();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);  // A sync point: f is clean.
  Forget(f, 1);
  EXPECT_THAT(Dirty(), Not(Contains(f)));  // Not reconciled yet.
  AppendToFile(Path("f"), "stored");       // The mapping's stores.
  Forget(f, 1);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(attr.valid);
  EXPECT_EQ(attr.st.st_size, 6);
  EXPECT_THAT(Dirty(), Contains(f));
}

// Identity (formal/ident.tla): a reopen by handle that reaches another
// object than the row's answers ESTALE and forgets the row (OpenNode's
// VerifyBackingIdentity). Here the row's recorded generation is made not
// the object's: what an inode number recycled behind dcfs's back looks
// like where the handle carries no generation (on ext4 the handle itself
// would be refused: OutOfBandReplacementGetsEstaleFromItsHandle). An NFS
// client's handle for the nodeid (its LOOKUP(nodeid, ".")) then gets
// ESTALE too, and the name gets a new nodeid and generation. Its identity
// trace is formal/trace_tests/ident_mismatch.log.
TEST_F(DirCacheFSTest, IdentityCheckRefusesAnotherObjectBehindTheHandle) {
  WriteFile(Path("f"));
  Start();
  StartTrace(/*identities_only=*/true);
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  // An NFS client's reconnection, answered from the row.
  auto [dot, dot_entry] = Lookup(f, ".");
  ASSERT_EQ(dot.error, 0);
  EXPECT_EQ(dot_entry.generation, entry.generation);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  ASSERT_NE(attr.backing_gen, 0u) << "ext4 reports generations";
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * doctor,
      db_.Prepared("UPDATE inodes SET backing_gen = ? WHERE id = ?"));
  ASSERT_THAT(doctor->BindAll(attr.backing_gen + 1, f), IsOk());
  ASSERT_THAT(doctor->ExecuteOnce(), IsOk());

  EXPECT_EQ(Open(f, O_RDONLY).first.error, -ESTALE);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(Lookup(f, ".").first.error, -ESTALE);
  auto [again, again_entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(again.error, 0);
  EXPECT_NE(again_entry.nodeid, entry.nodeid);
  EXPECT_NE(again_entry.generation, entry.generation);
  Forget(f, 2);
  Forget(static_cast<InodeId>(again_entry.nodeid), 1);
}

// ... and a file replaced behind dcfs's back (unlinked and created again,
// which on ext4 usually recycles its inode number): its handle is stale
// (open_by_handle_at: ESTALE, the handle's generation or a freed inode),
// the row goes, and the nodeid the kernel holds answers ESTALE from then
// on, never the new file.
TEST_F(DirCacheFSTest, OutOfBandReplacementGetsEstaleFromItsHandle) {
  WriteFile(Path("f"));
  Start();
  StartTrace(/*identities_only=*/true);
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0), IsOk());
  WriteFile(Path("f"));

  EXPECT_EQ(Open(f, O_RDONLY).first.error, -ESTALE);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(Getattr(f).first.error, -ESTALE);
  EXPECT_EQ(Lookup(f, ".").first.error, -ESTALE);
  Forget(f, 1);
}

// An unlinked file the kernel holds two lookups of is answered from its
// removed record until both are forgotten, one at a time.
TEST_F(DirCacheFSTest, RemovedFileIsServedUntilItsLastForget) {
  WriteFile(Path("f"));
  Start();
  StartTrace();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(Getattr(f).second.nlink, 0u);
  Forget(f, 1);
  EXPECT_EQ(Getattr(f).first.error, 0);
  Forget(f, 1);
  EXPECT_EQ(Getattr(f).first.error, -ESTALE);
}

// A FORGET_MULTI takes each entry's nlookup off its nodeid's count: both
// are forgotten, and the removed one's record goes.
TEST_F(DirCacheFSTest, ForgetMultiTakesOffEachEntrysCount) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  StartTrace();
  InodeId f = 0, g = 0;
  for (int i = 0; i < 2; ++i) {
    auto [lf, ef] = Lookup(kRootInode, "f");
    auto [lg, eg] = Lookup(kRootInode, "g");
    ASSERT_EQ(lf.error, 0);
    ASSERT_EQ(lg.error, 0);
    f = static_cast<InodeId>(ef.nodeid);
    g = static_cast<InodeId>(eg.nodeid);
  }
  ASSERT_EQ(Unlink(kRootInode, "g").error, 0);
  EXPECT_EQ(Getattr(g).first.error, 0);
  BatchForget({{f, 2}, {g, 2}});
  EXPECT_EQ(Getattr(g).first.error, -ESTALE);
  // f's row stays (a named object's row is a cache record), and the next
  // lookups count from 0 again, by its name or as ".", which an NFS
  // handle's reconnection sends.
  EXPECT_THAT(cache::GetAttr(ctx_, f), IsOk());
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  ASSERT_EQ(Lookup(f, ".").first.error, 0);
  Forget(f, 2);
}

// A rename over an open file keeps its row until its last release (with
// nlink 0 in it), which retires it into a removed record while the kernel
// holds the nodeid.
TEST_F(DirCacheFSTest, RenameOverAnOpenFileRetiresItAtTheLastRelease) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  StartTrace();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Rename(kRootInode, "g", kRootInode, "f").error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid);
  EXPECT_EQ(attr.st.st_nlink, 0u);
  ASSERT_EQ(Release(f, fh).error, 0);
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(Getattr(f).first.error, 0);
  Forget(f, 1);
  EXPECT_EQ(Getattr(f).first.error, -ESTALE);
}

// DESTROY lets go of every nodeid: a written file's held descriptor and
// written_ entry with them (its reconciliation is
// DestroyReconcilesWrittenFiles's). With no writable open left the
// shutdown is clean, and the next start sweeps nothing.
TEST_F(DirCacheFSTest, DestroyLetsGoOfEveryNodeid) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  // A running daemon's flag (StartRun clears it): the trace begins in a
  // run, which FinishRun ends.
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  StartTrace();
  auto [lf, ef] = Lookup(kRootInode, "f");
  ASSERT_EQ(lf.error, 0);
  const InodeId f = static_cast<InodeId>(ef.nodeid);
  auto [open, fh] = Open(f, O_WRONLY);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_EQ(Lookup(kRootInode, "g").first.error, 0);
  ASSERT_EQ(Unlink(kRootInode, "g").error, 0);
  EXPECT_EQ(Send(FUSE_DESTROY, 0, "").error, 0);
  EXPECT_THAT(cache::GetAttr(ctx_, f), IsOk());
  ASSERT_THAT(backing::FinishRun(ctx_), IsOk());
  ASSERT_THAT(GetCleanShutdown(db_), IsOkAndHolds(true));
  ASSERT_THAT(backing::StartRun(ctx_, "boot"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, f), IsOk());
}

// The number of rows in `inodes`.
absl::StatusOr<int64_t> InodeRows(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                        db.Prepared("SELECT COUNT(*) FROM inodes"));
  ABSL_ASSIGN_OR_RETURN(bool row, stmt->Step());
  if (!row) return absl::InternalError("no row");
  const int64_t n = stmt->Column<int64_t>(0);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return n;
}

// A crash between an unlink's backing syscall and its phase 3 (here: phase
// 1 and the unlink made by hand), then the start that follows a crash, in
// main.cc's order (Restart): the removed file's row, its nlink column still
// 1, is probed by handle (it was in the dirty set) and goes, as phase 3
// would have deleted it; a dirty directory removed the same way goes too,
// and a dirty file that still exists stays
// (formal/known_bugs/lifetime_crash_before_settle). Recorded from the
// crash on: the directory's trace ends ("gone") at the probe, and the kept
// file's nodeid trace has its probe.
TEST_F(DirCacheFSTest, CrashBetweenUnlinkAndPhase3LeavesNoRow) {
  WriteFile(Path("f"));
  WriteFile(Path("kept"));
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId kept, Id("kept"));
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  ASSERT_OK_AND_ASSIGN(const int64_t before, InodeRows(db_));
  for (auto [name, id] : {std::pair<std::string, InodeId>{"f", f},
                          std::pair<std::string, InodeId>{"d", d}}) {
    ASSERT_OK_AND_ASSIGN(
        cache::Mutation phase1,
        cache::BeginRemove(ctx_, kRootInode, name, id, cache::BeginFill(ctx_)));
    phase1.End();  // In memory only: the database keeps phase 1 alone.
  }
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0), IsOk());
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("d"), AT_REMOVEDIR), IsOk());
  const InodeId touched[] = {kept};
  ASSERT_THAT(cache::MarkDirty(ctx_, touched), IsOk());
  StartTrace();
  ASSERT_EQ(Lookup(kRootInode, "kept").first.error, 0);

  ASSERT_THAT(Restart("boot"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::GetAttr(ctx_, d).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::GetAttr(ctx_, kept), IsOk());
  EXPECT_THAT(InodeRows(db_), IsOkAndHolds(before - 2));
}

// A crash during recovery (steps 12.6, 12.6b): recovery may stop anywhere
// and run again, and what it recovered stays dirty until a sync point. The
// start that follows a crash between an unlink's syscall and its phase 3
// probes the unlinked file's row away; the recovered rows stay in the dirty
// set (the crashed run's backing changes may not be durable yet: only a
// sync point's syncfs makes them so), and the cache answers from the
// backing filesystem meanwhile. Built with DCFS_CRASH_DURING_RECOVERY
// (//dcfs:dir_cache_fs_crash_during_recovery_test, whose traces trace
// validation checks; trace_tests/recover_crash.log keeps the root's), the
// first start dies in RecoverDirty and the second's probe of the file's row
// fails: the row stays dirty, unprobed, and the third start probes it.
TEST_F(DirCacheFSTest, CrashDuringRecoveryRecoversAgain) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(true));
  {
    ASSERT_OK_AND_ASSIGN(
        cache::Mutation phase1,
        cache::BeginRemove(ctx_, kRootInode, "f", f, cache::BeginFill(ctx_)));
    phase1.End();  // In memory only: the database keeps phase 1 alone.
  }
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, Path("f"), 0), IsOk());
  StartTrace();

#ifdef DCFS_CRASH_DURING_RECOVERY
  // The daemon dies in RecoverDirty: nothing of it was recorded.
  ASSERT_FALSE(Restart("boot").ok());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(Contains(kRootInode)));
  // The next start's probe of the file's row fails: the row stays, its
  // attributes unknown, and dirty, for the next start.
  ASSERT_THAT(Restart("boot"), IsOk());
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  EXPECT_FALSE(attr.valid);
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(Contains(f)));
#endif
  ASSERT_THAT(Restart("boot"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(cache::IsDirComplete(ctx_, kRootInode), IsOkAndHolds(false));
  EXPECT_THAT(cache::Lookup(ctx_, kRootInode, "g"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kUnknown)));
  // Still dirty until a sync point: the unlink may not be durable yet.
  EXPECT_THAT(cache::ListDirty(ctx_),
              IsOkAndHolds(::testing::IsSupersetOf({kRootInode, f})));
  // Meanwhile the cache answers from the backing filesystem. (Through
  // ctx_, given the memory the last start's process had: Restart's Context
  // is gone, and ctx_'s is the crashed process's.)
  ctx_.dirty.durable.clear();
  ctx_.dirty.any = true;
  EXPECT_THAT(backing::LookupOrPopulate(ctx_, kRootInode, "f"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kNegative)));
  EXPECT_THAT(backing::LookupOrPopulate(ctx_, kRootInode, "g"),
              IsOkAndHolds(IsLookup(LookupResult::Kind::kFound)));
  EXPECT_THAT(cache::ListDirty(ctx_),
              IsOkAndHolds(::testing::IsSupersetOf({kRootInode, f})));
  // The first sync point (syncfs, then ClearDirty) takes them out.
  ASSERT_THAT(backing::SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(::testing::IsEmpty()));
}

// DESTROY while an unlinked file is still open for reading (SIGTERM, a
// lazy unmount): its row, kept by phase 3 with nlink 0 until the last
// release that never comes, goes at the next start although that start is
// clean (no writable open kept anything dirty)
// (formal/findings/lifetime_destroy_with_open_files, fixed).
TEST_F(DirCacheFSTest, CleanStartSweepsTheRowOfAFileOpenAtDestroy) {
  WriteFile(Path("f"));
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
  ASSERT_EQ(attr.st.st_nlink, 0u);
  EXPECT_EQ(Send(FUSE_DESTROY, 0, "").error, 0);
  ASSERT_THAT(backing::FinishRun(ctx_), IsOk());
  ASSERT_THAT(GetCleanShutdown(db_), IsOkAndHolds(true));
  ASSERT_THAT(backing::StartRun(ctx_, "boot"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, f).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

// A backing filesystem that went read-only during the run (by itself, after
// an error) answers syncfs with success without making anything durable: a
// sync point must keep the dirty set, so the run does not end clean (step
// 11.5, fault_shutdown_test's "ro"). Once it is writable again (in the
// field: unmounted, checked and mounted again) a sync point clears it.
TEST_F(DirCacheFSTest, SyncPointKeepsTheDirtySetIfTheBackingWentReadOnly) {
  Start();
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());  // A running daemon.
  ASSERT_EQ(Mkdir(kRootInode, "new").first.error, 0);
  ASSERT_THAT(Dirty(), Contains(kRootInode));
  StatvfsReadOnly() = true;
  absl::Status synced = backing::SyncBacking(ctx_);
  EXPECT_THAT(GetErrnoFromStatus(synced), IsOkAndHolds(EROFS));
  EXPECT_THAT(synced.message(), HasSubstr("read-only"));
  EXPECT_THAT(Dirty(), Contains(kRootInode));
  EXPECT_THAT(backing::FinishRun(ctx_), Not(IsOk()));
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(false));
  StatvfsReadOnly() = false;
  EXPECT_THAT(backing::SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
}

// A source read-only at the start but remounted read-write during the run
// can lose changes again: the exemption ends at the first sync point that
// finds it writable, and a later one that finds it read-only fails.
TEST_F(DirCacheFSTest, ReadOnlyAtStartExemptionEndsOnceWritable) {
  Start();
  ctx_.source_read_only_at_start = true;
  EXPECT_THAT(backing::SyncBacking(ctx_), IsOk());  // Found writable.
  ASSERT_EQ(Mkdir(kRootInode, "new").first.error, 0);
  StatvfsReadOnly() = true;
  EXPECT_THAT(GetErrnoFromStatus(backing::SyncBacking(ctx_)),
              IsOkAndHolds(EROFS));
  EXPECT_THAT(Dirty(), Contains(kRootInode));
}

// A source read-only from the start never had a change to lose: its sync
// points clear the dirty set (of failed mutations) as usual.
TEST_F(DirCacheFSTest, SyncPointOfASourceReadOnlyFromTheStartClears) {
  Start();
  ASSERT_EQ(Mkdir(kRootInode, "new").first.error, 0);
  ctx_.source_read_only_at_start = true;
  StatvfsReadOnly() = true;
  EXPECT_THAT(backing::SyncBacking(ctx_), IsOk());
  EXPECT_THAT(cache::ListDirty(ctx_), IsOkAndHolds(testing::IsEmpty()));
}

// An O_TMPFILE file left open by a crash: the next start (unclean) sweeps
// its row.
TEST_F(DirCacheFSTest, TmpfileRowGoesAtTheStartAfterACrash) {
  Start();
  // A running daemon's flag (StartRun clears it): the trace begins in a
  // run, and the crash is the daemon's dying with the tmpfile open.
  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  StartTrace();
  Created tmp = Tmpfile(kRootInode, O_RDWR);
  ASSERT_EQ(tmp.reply.error, 0);
  ASSERT_THAT(backing::StartRun(ctx_, "boot"), IsOk());
  EXPECT_THAT(cache::GetAttr(ctx_, tmp.id).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

// --- Runtime invariant checks (step 26.2) ----------------------------------
//
// Each test breaks one invariant on purpose and expects the checker the
// fixture installs (dcfs/testonly/invariant_checker.h) to abort, naming the
// invariant and the request. What breaks it is the test's own doing (a
// transaction it opens, a row it rewrites, DirCacheFS bookkeeping it
// changes through the testonly peer), inside the death test's child, so
// that the fixture's own checks at DESTROY still pass: no production code
// is faulted.

using DirCacheFSDeathTest = DirCacheFSTest;
using testonly::DirCacheFSPeer;

// A regex for "in request <op> nodeid <id>" in a violation's message.
std::string InRequest(std::string_view op, InodeId id) {
  return absl::StrCat("in request ", op, " nodeid ", id, "[,)]");
}

TEST_F(DirCacheFSDeathTest, TransactionOpenAtABackingSyscall) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  // Its GETATTR must reach the backing file (open_by_handle_at).
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec("BEGIN"), IsOk());
        Getattr(f);
      },
      "invariant violated: no-transaction-at-backing-call: a transaction is "
      "open.*open_by_handle_at.*" + InRequest("GETATTR", f));
}

TEST_F(DirCacheFSDeathTest, StatementMidStepAtABackingSyscall) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_THAT(cache::MarkAttrsUnknown(ctx_, f), IsOk());
  EXPECT_DEATH(
      {
        // A read cursor part way through its rows holds a read transaction.
        absl::StatusOr<sqlite3::Statement> cursor =
            sqlite3::Statement::Prepare(db_, "SELECT id FROM inodes");
        ASSERT_THAT(cursor, IsOk());
        ASSERT_THAT(cursor->Step(), IsOkAndHolds(true));
        Getattr(f);
      },
      "invariant violated: no-transaction-at-backing-call: a statement is "
      "part way through its rows: SELECT id FROM inodes.*" +
          InRequest("GETATTR", f));
}

TEST_F(DirCacheFSDeathTest, TransactionOpenAtARequestEnd) {
  Start();
  // The root's attributes are cached: its GETATTR makes no backing syscall.
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec("BEGIN"), IsOk());
        Getattr(kRootInode);
      },
      "invariant violated: no-transaction-at-request-end: a transaction is "
      "open.*" + InRequest("GETATTR", kRootInode));
}

// The dirty-set bug phase 1's fast path could hide: an inode taken for
// durably dirty that has no dirty row, so that a phase 1 skips its insert.
TEST_F(DirCacheFSDeathTest, MutationInFlightWithoutADirtyRow) {
  Start();
  ASSERT_THAT(Dirty(), Not(Contains(kRootInode)));
  EXPECT_DEATH(
      {
        ctx_.dirty.durable.insert(kRootInode);
        Mkdir(kRootInode, "d");
      },
      "invariant violated: dirty-set: inode 1 has a mutation in "
      "flight but no dirty row.*" +
          InRequest("MKDIR", kRootInode));
}

TEST_F(DirCacheFSDeathTest, DurablyDirtyWithoutADirtyRow) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_THAT(Dirty(), Not(Contains(f)));
  EXPECT_DEATH(
      {
        ctx_.dirty.durable.insert(f);
        Getattr(f);
      },
      absl::StrCat("invariant violated: dirty-set: inode ", f,
                   " is in Context::dirty.durable but has no dirty "
                   "row.*",
                   InRequest("GETATTR", f)));
}

TEST_F(DirCacheFSDeathTest, DirtySetSaidEmptyButIsNot) {
  Start();
  ASSERT_EQ(Mkdir(kRootInode, "d").first.error, 0);
  ASSERT_THAT(Dirty(), Contains(kRootInode));
  EXPECT_DEATH(
      {
        ctx_.dirty.any = false;
        Getattr(kRootInode);
      },
      "invariant violated: dirty-set: Context::dirty.any is false "
      "but the dirty table has rows.*" +
          InRequest("GETATTR", kRootInode));
}

// Step 23.8: the atime-only half of dirty-set, and open-file.
TEST_F(DirCacheFSDeathTest, DirtyAtimeSaidEmptyButIsNot) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);  // A cold open: an atime-only row.
  ASSERT_EQ(open.error, 0);
  ASSERT_THAT(Dirty(), Contains(f));
  EXPECT_DEATH(
      {
        ctx_.dirty.atime = false;
        Getattr(kRootInode);
      },
      "invariant violated: dirty-set: Context::dirty.atime is false "
      "but the dirty table has atime-only rows.*" +
          InRequest("GETATTR", kRootInode));
}

TEST_F(DirCacheFSDeathTest, OpenFileWithoutADirtyRow) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  EXPECT_DEATH(
      {
        ASSERT_THAT(
            db_.Exec(absl::StrCat("DELETE FROM dirty WHERE inode = ", f)),
            IsOk());
        Getattr(f);
      },
      absl::StrCat("invariant violated: open-file: inode ", f,
                   " has an open backing file but no dirty row.*") +
          InRequest("GETATTR", f));
}

TEST_F(DirCacheFSDeathTest, AttributesCurrentWithNoLinks) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(
                        "UPDATE inodes SET nlink = 0 WHERE id = ", f)),
                    IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: tri-state: inode ", f,
                   ": attributes recorded as current with nlink 0"));
}

TEST_F(DirCacheFSDeathTest, RefusedDentryWithoutAStub) {
  WriteFile(Path("f"));
  Start();
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  EXPECT_DEATH(
      {
        ASSERT_THAT(
            db_.Exec("UPDATE dentries SET state = 'refused', inode = NULL "
                     "WHERE parent = 1 AND name = CAST('f' AS BLOB)"),
            IsOk());
        Getattr(kRootInode);
      },
      "invariant violated: tri-state: dentry \"f\" of inode 1 is "
      "refused but has no stub");
}

TEST_F(DirCacheFSDeathTest, NonRootGenerationZero) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(
                        "UPDATE inodes SET fuse_gen = 0 WHERE id = ", f)),
                    IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: identity: inode ", f,
                   " has FUSE generation 0"));
}

TEST_F(DirCacheFSDeathTest, AttributesCurrentWhileOpenForWriting) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(
                        "UPDATE inodes SET attrs_valid = 1 WHERE id = ", f)),
                    IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " is open for writing but its attributes are "
                   "recorded as current"));
}

TEST_F(DirCacheFSDeathTest, OpenForWritingWithoutADirtyRow) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        ASSERT_THAT(
            db_.Exec(absl::StrCat("DELETE FROM dirty WHERE inode = ", f)),
            IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " is open for writing but has no dirty row"));
}

TEST_F(DirCacheFSDeathTest, OpenForWritingButNotInWritten) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableWritten(*fs_).erase(f);
        Getattr(f);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " is open for writing but not in written_.*",
                   InRequest("GETATTR", f)));
}

TEST_F(DirCacheFSDeathTest, ForgetOfMoreLookupsThanCounted) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(Forget(f, 2),
               absl::StrCat("invariant violated: lookup-count: FORGET of 2 "
                            "lookups of nodeid ", f, ", but 1 counted.*",
                            InRequest("FORGET", f)));
}

// The same nodeid twice in one BATCH_FORGET counts as one FORGET of both.
TEST_F(DirCacheFSDeathTest, BatchForgetOfMoreLookupsThanCounted) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(BatchForget({{f, 1}, {f, 1}}),
               absl::StrCat("invariant violated: lookup-count: FORGET of 2 "
                            "lookups of nodeid ", f, ", but 1 counted.*",
                            InRequest("BATCH_FORGET", f)));
}

TEST_F(DirCacheFSDeathTest, ZeroLookupCountKept) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableLookups(*fs_)[f] = 0;
        Getattr(f);
      },
      absl::StrCat("invariant violated: lookup-count: nodeid ", f,
                   " has a lookup count of 0"));
}

// A held descriptor dropped without telling held_fds_.
TEST_F(DirCacheFSDeathTest, HeldDescriptorDroppedBehindTheCount) {
  Start();
  Created f = Create(kRootInode, "f", O_RDWR);
  ASSERT_EQ(f.reply.error, 0);
  ASSERT_EQ(Release(f.id, f.fh).error, 0);
  ASSERT_TRUE(DirCacheFSPeer::Written(*fs_).at(f.id).has_value());
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableWritten(*fs_)[f.id].reset();
        Getattr(kRootInode);
      },
      "invariant violated: held-fds: held_fds_ is 1 but 0 entries "
      "of written_ hold a descriptor");
}

TEST_F(DirCacheFSDeathTest, RemovedRecordWithoutALookup) {
  Start();
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr root,
                       cache::GetAttr(ctx_, kRootInode));
  constexpr InodeId kGone = 12345;
  EXPECT_DEATH(
      {
        absl::StatusOr<FileDescriptor> fd =
            syscalls::openat(AT_FDCWD, source_, O_PATH);
        ASSERT_THAT(fd, IsOk());
        DirCacheFSPeer::AddRemoved(*fs_, kGone, root, *std::move(fd));
        Getattr(kRootInode);
      },
      "invariant violated: removed-record: nodeid 12345 has a "
      "removed record but the kernel holds no lookup of it");
}

// The cost counters (step 26.4b): FUSE requests by opcode, SQLite steps and
// transactions (durable ones apart), backing calls; the invariant
// checker's own statements are not the daemon's cost.
TEST_F(DirCacheFSTest, CostCounterCountsRequestsStepsAndTransactions) {
  WriteFile(Path("f"));
  Start();
  counter_.Reset();
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  ASSERT_EQ(Mkdir(kRootInode, "d").first.error, 0);
  const testonly::CostCounter::Counts counts = counter_.counts();
  EXPECT_EQ(counts.requests.at("LOOKUP"), 2);
  EXPECT_EQ(counts.requests.at("MKDIR"), 1);
  EXPECT_GT(counts.steps, 0);
  EXPECT_GT(counts.transactions, 0);
  // The mkdir's phase 1 is durable: the root was not yet durably dirty.
  EXPECT_GE(counts.durable_transactions, 1);
  EXPECT_GT(counts.backing_calls, 0);
  // The checker's statements are left out.
  ASSERT_THAT(checker_->CheckAll(ctx_, fs_.get()), IsOk());
  EXPECT_EQ(counter_.counts().steps, counts.steps);
}

// What step 26.6 calls after an injected fault: the checks as a status.
TEST_F(DirCacheFSTest, InvariantChecksReportAsAStatus) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_THAT(checker_->CheckAll(ctx_, fs_.get()), IsOk());
  EXPECT_THAT(checker_->CheckBackingCall(ctx_), IsOk());
  DirCacheFSPeer::MutableLookups(*fs_)[f] = 0;
  absl::Status all = checker_->CheckAll(ctx_, fs_.get());
  EXPECT_EQ(all.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(all.message(), ::testing::StartsWith("lookup-count: "));
  DirCacheFSPeer::MutableLookups(*fs_)[f] = 1;
  EXPECT_THAT(checker_->CheckAll(ctx_, fs_.get()), IsOk());
}

// A sync point's one-statement clear, DELETE FROM dirty with no WHERE
// (SQLite's truncate optimisation, which its update hook does not see),
// dropping the row of an inode open for writing that the request does not
// name (a passthrough-written file: no WRITE reaches dcfs).
TEST_F(DirCacheFSDeathTest, DirtyRowOfAnOpenFileTruncatedAway) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  ASSERT_THAT(Dirty(), Contains(f));
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec("DELETE FROM dirty"), IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " is open for writing but has no dirty row.*",
                   InRequest("GETATTR", kRootInode)));
}

// The same for a durably dirty inode the request does not name.
TEST_F(DirCacheFSDeathTest, DurableDirtyRowTruncatedAway) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Mkdir(kRootInode, "d").first.error, 0);
  ASSERT_TRUE(ctx_.dirty.durable.contains(kRootInode));
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec("DELETE FROM dirty"), IsOk());
        Getattr(f);
      },
      "invariant violated: dirty-set: inode 1 is in Context::dirty.durable "
      "but has no dirty row.*" +
          InRequest("GETATTR", f));
}

constexpr std::string_view kInsertStub =
    "INSERT INTO stubs (id, parent, name, fuse_gen, mode, nlink, uid, gid, "
    "rdev, size, blocks, blksize, atime_s, atime_ns, mtime_s, mtime_ns, "
    "ctime_s, ctime_ns, btime_s, btime_ns) VALUES (-5, 1, CAST('";
constexpr std::string_view kInsertStubEnd =
    "' AS BLOB), 7, 16877, 2, 0, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 0, 0, 0, 0)";

TEST_F(DirCacheFSDeathTest, StubWithoutARefusedDentry) {
  Start();
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(kInsertStub, "x", kInsertStubEnd)),
                    IsOk());
        Getattr(kRootInode);
      },
      "invariant violated: tri-state: stub [0-9]+ of dentry \"x\" of inode 1 "
      "whose dentry is missing");
}

TEST_F(DirCacheFSDeathTest, DentryWithAStubButNotRefused) {
  WriteFile(Path("f"));
  Start();
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(kInsertStub, "f", kInsertStubEnd)),
                    IsOk());
        ASSERT_THAT(db_.Exec("UPDATE dentries SET inode = inode "
                             "WHERE parent = 1 AND name = CAST('f' AS BLOB)"),
                    IsOk());
        Getattr(kRootInode);
      },
      "invariant violated: tri-state: dentry \"f\" of inode 1 is present but "
      "has a stub");
}

TEST_F(DirCacheFSDeathTest, AttributesCurrentWithAColumnNull) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(
                        "UPDATE inodes SET mode = NULL WHERE id = ", f)),
                    IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: tri-state: inode ", f,
                   ": attributes recorded as current with a column NULL"));
}

TEST_F(DirCacheFSDeathTest, GenerationAbove32Bits) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(
      {
        ASSERT_THAT(
            db_.Exec(absl::StrCat(
                "UPDATE inodes SET fuse_gen = 4294967296 WHERE id = ", f)),
            IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: identity: inode ", f,
                   " has FUSE generation 4294967296"));
}

TEST_F(DirCacheFSDeathTest, HeldDescriptorsAboveTheCap) {
  Start();
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableHeldFds(*fs_) = 65;
        Getattr(kRootInode);
      },
      "invariant violated: held-fds: held_fds_ is 65, above max_held_fds_ "
      "64");
}

TEST_F(DirCacheFSDeathTest, SharedFileWithNoRefs) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableRefs(*fs_, f) = 0;
        Getattr(f);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   "'s shared backing file has refs 0 and writable_refs 1"));
}

TEST_F(DirCacheFSDeathTest, SharedFileWithMoreWritableRefsThanRefs) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableWritableRefs(*fs_, f) = 2;
        Getattr(f);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   "'s shared backing file has refs 1 and writable_refs 2"));
}

TEST_F(DirCacheFSDeathTest, WritableSharedFileNotOpenForWriting) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        DirCacheFSPeer::MutableOpenForWrite(*fs_).erase(f);
        Getattr(f);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " has writable opens but is not open for writing"));
}

TEST_F(DirCacheFSDeathTest, OpenForWriteSetNotDirCacheFSs) {
  Start();
  absl::flat_hash_set<int64_t> other;
  EXPECT_DEATH(
      {
        ctx_.open_for_write = &other;
        Getattr(kRootInode);
      },
      "invariant violated: writable-open: Context::open_for_write is not "
      "DirCacheFS's set");
}

TEST_F(DirCacheFSDeathTest, RemovedRecordBesideAWrittenEntry) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr row, cache::GetAttr(ctx_, f));
  EXPECT_DEATH(
      {
        absl::StatusOr<FileDescriptor> fd =
            syscalls::openat(AT_FDCWD, Path("f"), O_PATH);
        ASSERT_THAT(fd, IsOk());
        DirCacheFSPeer::AddRemoved(*fs_, f, row, *std::move(fd));
        DirCacheFSPeer::MutableWritten(*fs_)[f];
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: removed-record: nodeid ", f,
                   " has both a removed record and a written_ entry"));
}

// Startup's full check (after its probe of the recovered rows) finds what
// no request changed, and names itself.
TEST_F(DirCacheFSDeathTest, FullCheckAtStartup) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  EXPECT_DEATH(
      {
        ASSERT_THAT(db_.Exec(absl::StrCat(
                        "UPDATE inodes SET nlink = 0 WHERE id = ", f)),
                    IsOk());
        ASSERT_THAT(Restart("boot"), IsOk());
      },
      absl::StrCat("invariant violated: tri-state: inode ", f,
                   ": attributes recorded as current with nlink 0 \\(in "
                   "Startup\\)"));
}

// Schema v5: a stub outlives its refusal while the name is unknown (a
// mutation's phase 1 forgot it, so that refusing it again keeps its
// nodeid), not once the name is recorded present or absent.
TEST_F(DirCacheFSTest, StubOfAnUnknownDentryIsLegal) {
  WriteFile(Path("f"));
  Start();
  ASSERT_EQ(Lookup(kRootInode, "f").first.error, 0);
  ASSERT_THAT(db_.Exec(absl::StrCat(kInsertStub, "u", kInsertStubEnd)),
              IsOk());
  ASSERT_THAT(db_.Exec("INSERT INTO dentries (parent, name, state) "
                       "VALUES (1, CAST('u' AS BLOB), 'unknown')"),
              IsOk());
  EXPECT_THAT(checker_->CheckChanged(ctx_, fs_.get(), {}), IsOk());
  EXPECT_THAT(checker_->CheckAll(ctx_, fs_.get()), IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM stubs WHERE id = -5"), IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM dentries WHERE name = CAST('u' AS BLOB)"),
              IsOk());
}

// DESTROY's full check names itself, and its exemption (written_, which
// Destroy empties) does not cover a file still open for writing without
// its dirty row.
TEST_F(DirCacheFSDeathTest, FullCheckAtDestroy) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  EXPECT_DEATH(
      {
        ASSERT_THAT(
            db_.Exec(absl::StrCat("DELETE FROM dirty WHERE inode = ", f)),
            IsOk());
        fuse_session_destroy(se_);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " is open for writing but has no dirty row \\(in "
                   "DESTROY\\)"));
}

// The exemption itself: DESTROY with a file still open for writing (a lazy
// unmount) passes, although Destroy emptied written_.
TEST_F(DirCacheFSTest, DestroyWithAFileStillOpenForWriting) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  fuse_session_destroy(se_);  // Would abort on a violation.
  se_ = nullptr;
  EXPECT_TRUE(DirCacheFSPeer::Written(*fs_).empty());
}

// An open older than the run (the harness standing for a crash: StartRun
// under a live DirCacheFS) is left out, but only until it is released: a
// writable open of this run is checked again.
TEST_F(DirCacheFSDeathTest, OpenOlderThanTheRunIsLeftOutUntilReleased) {
  WriteFile(Path("f"));
  Start();
  auto [lookup, entry] = Lookup(kRootInode, "f");
  ASSERT_EQ(lookup.error, 0);
  const InodeId f = static_cast<InodeId>(entry.nodeid);
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_OK_AND_ASSIGN(std::vector<InodeId> recovered,
                       backing::StartRun(ctx_, "boot"));
  // Recovered; and taken out of the dirty set, as the first sync point
  // after the start would (the old open is not one of this run's).
  ASSERT_THAT(recovered, Contains(f));
  ASSERT_THAT(db_.Exec(absl::StrCat("DELETE FROM dirty WHERE inode = ", f)),
              IsOk());
  ASSERT_THAT(Dirty(), Not(Contains(f)));
  EXPECT_EQ(Getattr(kRootInode).first.error, 0);  // Left out: no abort.
  ASSERT_EQ(Release(f, fh).error, 0);
  ASSERT_EQ(Open(f, O_RDWR).first.error, 0);
  ASSERT_THAT(Dirty(), Contains(f));
  EXPECT_DEATH(
      {
        ASSERT_THAT(
            db_.Exec(absl::StrCat("DELETE FROM dirty WHERE inode = ", f)),
            IsOk());
        Getattr(kRootInode);
      },
      absl::StrCat("invariant violated: writable-open: inode ", f,
                   " is open for writing but has no dirty row"));
}

// Above kRecountLimit written_ entries a request's check no longer recounts
// held_fds_ (only the full check does); at or below it, it does.
TEST_F(DirCacheFSTest, InvariantChecksRecountOnlyBelowTheLimit) {
  Start();
  auto &written = DirCacheFSPeer::MutableWritten(*fs_);
  for (InodeId id = 1'000'000;
       written.size() <= testonly::InvariantChecker::kRecountLimit; ++id) {
    written[id];  // An entry holding no descriptor.
  }
  ++DirCacheFSPeer::MutableHeldFds(*fs_);  // Says one does.
  EXPECT_THAT(checker_->CheckChanged(ctx_, fs_.get(), {}), IsOk());
  absl::Status all = checker_->CheckAll(ctx_, fs_.get());
  EXPECT_THAT(all.message(), ::testing::StartsWith("held-fds: "));
  written.clear();
  absl::Status changed = checker_->CheckChanged(ctx_, fs_.get(), {});
  EXPECT_THAT(changed.message(), ::testing::StartsWith("held-fds: "));
  --DirCacheFSPeer::MutableHeldFds(*fs_);
  EXPECT_THAT(checker_->CheckChanged(ctx_, fs_.get(), {}), IsOk());
}

// --- 8.2: survivors of the first mutation run (tools/mutation) -----------

// A tmpfile whose create fails after its row was recorded takes the row
// back (ForgetRemoved). Here the row is already gone by then (invalidated
// meanwhile): "already gone is fine", the create's own error is replied and
// the undo reports nothing.
TEST_F(DirCacheFSTest, TmpfileUndoToleratesARowThatIsAlreadyGone) {
  Start();
  // As TmpfileUndoForgetsItsRow: the statx after RecordTmpfile's probe is the
  // reply's attribute refresh, which fails; just before it the row goes.
  NameToHandleHook() = [&] {
    StatxHook() = [&] {
      ASSERT_THAT(db_.Exec("DELETE FROM inodes WHERE id <> 1"), IsOk());
      StatxFailure() = EIO;
    };
  };
  WarningCapture capture;
  Created tmp = Tmpfile(kRootInode, O_RDWR);
  EXPECT_EQ(tmp.reply.error, -EIO);
  // (The create's own error is logged too, at ERROR, when replied.)
  EXPECT_THAT(capture.lines, Not(Contains(HasSubstr("Tmpfile undo"))))
      << absl::StrJoin(capture.lines, "\n");
}

// The other side: a row that cannot be deleted (here by a trigger) is
// reported at WARNING, once, and the RELEASE still replies 0 (the kernel
// ignores its errors); the row stays for the next start's sweep.
TEST_F(DirCacheFSTest, ReleaseOfAnUnlinkedFileWhoseRowCannotBeDeletedWarns) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Unlink(kRootInode, "f").error, 0);
  ASSERT_THAT(db_.Exec("CREATE TEMP TRIGGER no_delete BEFORE DELETE ON inodes "
                       "BEGIN SELECT RAISE(ABORT, 'no deletes'); END"),
              IsOk());

  WarningCapture capture;
  EXPECT_EQ(Release(f, fh).error, 0);
  ASSERT_THAT(db_.Exec("DROP TRIGGER no_delete"), IsOk());
  EXPECT_THAT(capture.lines,
              ElementsAre(HasSubstr("Release: could not delete the row of "
                                    "unlinked inode")));
  EXPECT_THAT(cache::GetAttr(ctx_, f), IsOk());  // Left for next time.
  // Leave the cache as the checker expects it.
  ASSERT_THAT(cache::DeleteInode(ctx_, f), IsOk());
}

// A copy_file_range's mutation ends after its backing copy and before its
// phase-3 refreshes, which run as ordinary fills (a fill is refused while a
// mutation of the inode is in flight): at the refresh's first backing call
// nothing is in flight.
TEST_F(DirCacheFSTest, CopyFileRangeEndsItsMutationBeforeItsRefreshes) {
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

  // Phase 1 and the backing copy make no statx: the first is the refresh.
  std::optional<size_t> in_flight;
  StatxHook() = [&] { in_flight = ctx_.fills.inflight.size(); };
  EXPECT_EQ(CopyFileRange(src, in_fh, dst, out_fh, 100), 10);
  ASSERT_TRUE(in_flight.has_value()) << "the hook did not run";
  EXPECT_EQ(*in_flight, 0u) << "the copy's mutation was still in flight when "
                               "its phase-3 refresh began";
  EXPECT_EQ(Release(dst, out_fh).error, 0);
  EXPECT_EQ(Release(src, in_fh).error, 0);
}

// A removexattr's mutation ends after its backing call and before its
// phase-3 refreshes (see CopyFileRangeEndsItsMutationBeforeItsRefreshes).
TEST_F(DirCacheFSTest, RemovexattrEndsItsMutationBeforeItsRefreshes) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_EQ(Setxattr(f, "user.k", "v").error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);

  std::string name = "user.k";
  name.push_back('\0');
  // The backing removexattr reopens the inode (a statx or more, inside the
  // mutation); the last statx is the refresh's.
  std::vector<size_t> in_flight;
  std::function<void()> observe = [&] {
    in_flight.push_back(ctx_.fills.inflight.size());
    StatxHook() = observe;
  };
  StatxHook() = observe;
  EXPECT_EQ(Send(FUSE_REMOVEXATTR, static_cast<uint64_t>(f), name).error, 0);
  StatxHook() = {};
  ASSERT_FALSE(in_flight.empty()) << "the hook did not run";
  EXPECT_EQ(in_flight.back(), 0u) << "the removexattr's mutation was still in "
                                     "flight when its phase-3 refresh began";
}

// Trace validation of a copy_file_range: the files' trace (formal/reval.tla)
// has its open, write and releases. (A missing End of the copy's mutation is
// not in any trace: neither model has an event for the end of an attribute
// change, so no fault build can make validation reject it; the unit test
// above is what catches it.)
TEST_F(DirCacheFSTest, TraceScenarioCopyFileRange) {
  WriteFile(Path("src"));
  AppendToFile(Path("src"), "0123456789");
  WriteFile(Path("dst"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId src, Id("src"));
  ASSERT_OK_AND_ASSIGN(InodeId dst, Id("dst"));

  StartTrace();  // and the files' (formal/reval.tla)
  auto [in, in_fh] = Open(src, O_RDONLY);
  auto [out, out_fh] = Open(dst, O_WRONLY);
  ASSERT_EQ(in.error, 0);
  ASSERT_EQ(out.error, 0);
  EXPECT_EQ(CopyFileRange(src, in_fh, dst, out_fh, 100), 10);
  EXPECT_EQ(Release(dst, out_fh).error, 0);
  EXPECT_EQ(Release(src, in_fh).error, 0);
}

// A fallocate's mutation, like a copy_file_range's, ends after its backing
// call and before its phase-3 refreshes (see
// CopyFileRangeEndsItsMutationBeforeItsRefreshes).
TEST_F(DirCacheFSTest, FallocateEndsItsMutationBeforeItsRefreshes) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_WRONLY);
  ASSERT_EQ(open.error, 0);
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);

  struct fuse_fallocate_in falloc = {};
  falloc.fh = fh;
  falloc.length = 4096;
  std::string body;
  AppendBytes(body, falloc);
  // Phase 1 and the backing fallocate make no statx: the first is the refresh.
  std::optional<size_t> in_flight;
  StatxHook() = [&] { in_flight = ctx_.fills.inflight.size(); };
  EXPECT_EQ(Send(FUSE_FALLOCATE, static_cast<uint64_t>(f), body).error, 0);
  ASSERT_TRUE(in_flight.has_value()) << "the hook did not run";
  EXPECT_EQ(*in_flight, 0u) << "the fallocate's mutation was still in flight "
                               "when its phase-3 refresh began";
  EXPECT_EQ(Release(f, fh).error, 0);
}

// A RELEASE says whether the open it ends could write (the lifetime model's
// argument of kReleased): 1 for a writable open, 0 for a read-only one, both
// when other opens remain and when it is the file's last.
TEST_F(DirCacheFSTest, ReleaseReportsWhetherTheOpenCouldWrite) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [ro, ro_fh] = Open(f, O_RDONLY);
  auto [rw, rw_fh] = Open(f, O_RDWR);
  auto [last, last_fh] = Open(f, O_RDONLY);
  ASSERT_EQ(ro.error, 0);
  ASSERT_EQ(rw.error, 0);
  ASSERT_EQ(last.error, 0);

  LifetimeSteps steps;
  ctx_.events = &steps;
  EXPECT_EQ(Release(f, ro_fh).error, 0);    // others remain
  EXPECT_EQ(Release(f, rw_fh).error, 0);    // others remain
  EXPECT_EQ(Release(f, last_fh).error, 0);  // the last, read-only
  auto [again, again_fh] = Open(f, O_RDWR);
  ASSERT_EQ(again.error, 0);
  EXPECT_EQ(Release(f, again_fh).error, 0);  // the last, writable
  ctx_.events = &NoProtocolEvents();
  using events::LifetimeStep;
  std::vector<uint64_t> released;
  for (const auto &[step, arg] : steps.steps) {
    if (step == LifetimeStep::kReleased) released.push_back(arg);
  }
  EXPECT_THAT(released, ElementsAre(0u, 1u, 0u, 1u));
}

// --- 8.2c: survivors of the second mutation sweep ------------------------

// A setattr's mutation, like a copy_file_range's, ends after its backing
// syscalls and before its phase-3 refresh. Every statx of the request records
// what is in flight: the refresh, the request's last, finds nothing.
TEST_F(DirCacheFSTest, SetattrEndsItsMutationBeforeItsRefresh) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  std::vector<size_t> in_flight;
  std::function<void()> observe = [&] {
    in_flight.push_back(ctx_.fills.inflight.size());
    StatxHook() = observe;
  };
  StatxHook() = observe;
  EXPECT_EQ(Chmod(f, S_IFREG | 0600).error, 0);
  ASSERT_FALSE(in_flight.empty()) << "the hook did not run";
  EXPECT_EQ(in_flight.back(), 0u) << "the setattr's mutation was still in "
                                     "flight when its phase-3 refresh began";
}

// The reconciliation at a written file's last FORGET reports nothing when it
// goes well, and nothing when the row is already gone (invalidated meanwhile).
TEST_F(DirCacheFSTest, ForgetReconciliationsAreQuiet) {
  WriteFile(Path("f"));
  WriteFile(Path("g"));
  Start();
  InodeId ids[2];
  for (int i = 0; i < 2; ++i) {
    auto [lookup, entry] = Lookup(kRootInode, i == 0 ? "f" : "g");
    ASSERT_EQ(lookup.error, 0);
    ids[i] = static_cast<InodeId>(entry.nodeid);
    auto [open, fh] = Open(ids[i], O_RDWR);
    ASSERT_EQ(open.error, 0);
    ASSERT_EQ(Release(ids[i], fh).error, 0);
  }
  ASSERT_EQ(Fsyncdir(kRootInode).error, 0);

  // f's attributes changed behind the mapping: reconciled and recorded.
  AppendToFile(Path("f"), "stored");
  {
    WarningCapture capture;
    Forget(ids[0], 1);
    EXPECT_THAT(capture.lines, testing::IsEmpty())
        << absl::StrJoin(capture.lines, "\n");
  }
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, ids[0]));
  EXPECT_EQ(attr.st.st_size, 6);

  // g's row went: "gone already", not worth a warning.
  ASSERT_THAT(cache::DeleteInode(ctx_, ids[1]), IsOk());
  {
    WarningCapture capture;
    Forget(ids[1], 1);
    EXPECT_THAT(capture.lines, testing::IsEmpty())
        << absl::StrJoin(capture.lines, "\n");
  }
}

// The open's reply carries the passthrough backing id the kernel granted
// (libfuse's fuse_reply_open), none when it refused; a second open of the
// file shares it and the last release closes it, once.
TEST_F(DirCacheFSTest, OpenRepliesWithTheGrantedBackingIdAndReleaseClosesIt) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto backing_id_of = [](const Reply &reply) {
    struct fuse_open_out out {};
    EXPECT_GE(reply.payload.size(), sizeof(out));
    std::memcpy(&out, reply.payload.data(), sizeof(out));
    return out.backing_id;
  };

  // Refused (the harness's own libfuse call fails): the fallback, no id.
  auto [refused, refused_fh] = Open(f, O_RDONLY);
  ASSERT_EQ(refused.error, 0);
  EXPECT_EQ(backing_id_of(refused), 0);
  ASSERT_EQ(Release(f, refused_fh).error, 0);

  Passthrough().enabled = true;
  auto [first, first_fh] = Open(f, O_RDONLY);
  auto [second, second_fh] = Open(f, O_RDWR);
  ASSERT_EQ(first.error, 0);
  ASSERT_EQ(second.error, 0);
  const int id = backing_id_of(first);
  EXPECT_GT(id, 0);
  EXPECT_EQ(backing_id_of(second), id);  // The shared one.
  ASSERT_EQ(Release(f, first_fh).error, 0);
  EXPECT_THAT(Passthrough().closed, testing::IsEmpty());
  ASSERT_EQ(Release(f, second_fh).error, 0);
  EXPECT_THAT(Passthrough().closed, ElementsAre(id));
}

// A passthrough id that cannot be closed at the last release is reported;
// one that closes is not.
TEST_F(DirCacheFSTest, ReleaseReportsAFailedPassthroughClose) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  Passthrough().enabled = true;
  for (bool fails : {false, true}) {
    Passthrough().close_fails = fails;
    auto [open, fh] = Open(f, O_RDONLY);
    ASSERT_EQ(open.error, 0);
    WarningCapture capture;
    EXPECT_EQ(Release(f, fh).error, 0);
    if (fails) {
      EXPECT_THAT(capture.lines, ElementsAre(HasSubstr("Release: closing "
                                                       "passthrough backing id")));
    } else {
      EXPECT_THAT(capture.lines, testing::IsEmpty())
          << absl::StrJoin(capture.lines, "\n");
    }
  }
}

// An open that fails after the passthrough id was granted gives it back (a
// cold open: nothing else uses it) and keeps it when the failing open is
// one of several (a shared one: an earlier open still depends on it, and on
// the BackingFile).
TEST_F(DirCacheFSTest, FailedOpensGiveBackThePassthroughIdOnlyWhenTheyOwnIt) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  Passthrough().enabled = true;
  int flags = 0;
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor raw_fd,
      syscalls::openat(AT_FDCWD, Path("f"), O_RDONLY));
  const int raw = *raw_fd;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_GETFLAGS, &flags), IsOk());
  const int immutable = flags | FS_IMMUTABLE_FL;
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &immutable), IsOk());
  OutOfBand(f);

  // Cold: the shared fd falls back to read-only, the writable check fails
  // (EPERM), and the id granted for it is closed again.
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  EXPECT_THAT(Passthrough().closed, ElementsAre(100));
  EXPECT_FALSE(fs_->HasOpenFiles(f));

  // Shared: a read-only open is outstanding; a failing writable one leaves
  // its id and its BackingFile alone.
  auto [ro, ro_fh] = Open(f, O_RDONLY);
  ASSERT_EQ(ro.error, 0);
  EXPECT_EQ(Open(f, O_WRONLY).first.error, -EPERM);
  EXPECT_THAT(Passthrough().closed, ElementsAre(100));  // Still just the first.
  EXPECT_TRUE(fs_->HasOpenFiles(f));
  EXPECT_EQ(Release(f, ro_fh).error, 0);
  EXPECT_THAT(Passthrough().closed, ElementsAre(100, 101));
  ASSERT_THAT(syscalls::ioctl(raw, FS_IOC_SETFLAGS, &flags), IsOk());
  OutOfBand(f);
}

// A writable open whose phase 1 fails gives its passthrough id back too.
TEST_F(DirCacheFSTest, WritableOpenWhosePhase1FailsClosesItsPassthroughId) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  Passthrough().enabled = true;
  OpenByHandleHook() = [&] {
    ASSERT_THAT(db_.Exec("PRAGMA query_only = 1"), IsOk());
  };
  auto [open, fh] = Open(f, O_RDWR);
  ASSERT_THAT(db_.Exec("PRAGMA query_only = 0"), IsOk());
  EXPECT_NE(open.error, 0);
  EXPECT_THAT(Passthrough().closed, ElementsAre(100));
  EXPECT_FALSE(fs_->HasOpenFiles(f));
}

// A setxattr the backing filesystem accepts is recorded in the cache as
// stored, right away, with the attributes refreshed for its ctime: no
// backing read is needed to answer for it.
TEST_F(DirCacheFSTest, SetxattrRecordsWhatTheBackingFilesystemStored) {
  WriteFile(Path("f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr before, cache::GetAttr(ctx_, f));
  ASSERT_TRUE(before.valid);
  ASSERT_EQ(Setxattr(f, "user.k", "v").error, 0);
  EXPECT_THAT(cache::GetXattr(ctx_, f, "user.k"),
              IsOkAndHolds(testing::Optional(std::string("v"))));
  ASSERT_OK_AND_ASSIGN(cache::CachedAttr after, cache::GetAttr(ctx_, f));
  EXPECT_TRUE(after.valid);
  EXPECT_THAT(Dirty(), Contains(f));  // Until a sync point covers it.
}

// The backstop under the hooks: a backing syscall made where no hook was
// called (here backing::StatFd, a descriptor-only helper whose callers
// call the hook) while a transaction is open still aborts, at the wrapped
// libc call (see __wrap_statx at the top of this file).
TEST_F(DirCacheFSDeathTest, UnhookedBackingSyscallInATransaction) {
  Start();
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       syscalls::openat(AT_FDCWD, source_, O_PATH));
  EXPECT_DEATH(
      {
        ASSERT_THAT(ctx_.db.Transaction([&]() -> absl::Status {
          return backing::StatFd(*fd).status();
        }),
                    IsOk());
      },
      "invariant violated: no-transaction-at-backing-call \\(the harness's "
      "backstop\\): statx while a transaction is open");
}

// --- Cancellation (Phase 22; docs/design.md, "Cancellation") -------------
//
// An interrupted request stops at a checkpoint (dcfs/checkpoint.h) just
// before a backing syscall and replies EINTR, leaving the cache as the
// tri-state rule wants: nothing recorded it did not finish, a mutation's
// phase-1 records unknown and dirty, every guard released, no transaction
// open (formal/dcfs.tla's Interrupt, GuardsBalanced).

// The harness's interruption source: the request being served is
// interrupted from the `at`-th checkpoint on (0: never), counting from
// when the test set it.
class FakeInterrupts final : public Interrupts {
 public:
  bool Interrupted() override {
    ++checkpoints;
    return at != 0 && checkpoints >= at;
  }
  int at = 0;
  int checkpoints = 0;
};

// Counts the probes (name_to_handle_at) from now on.
std::shared_ptr<int> CountProbes() {
  auto count = std::make_shared<int>(0);
  auto arm = std::make_shared<std::function<void()>>();
  *arm = [count, arm] {
    NameToHandleHook() = [count, arm] {
      ++*count;
      (*arm)();
    };
  };
  (*arm)();
  return count;
}

// Interrupted on arrival (its first checkpoint): a lookup that needs a
// population replies EINTR without touching the backing filesystem, and
// the next one, not interrupted, is served.
TEST_F(DirCacheFSTest, InterruptedLookupRepliesEintrWithoutTheBacking) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  WriteFile(Path("d/f"));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  StartTrace();
  interrupts.at = 1;
  std::shared_ptr<int> probes = CountProbes();
  int opens = 0;
  OpenByHandleHook() = [&] { ++opens; };
  EXPECT_EQ(Lookup(d, "f").first.error, -EINTR);
  EXPECT_EQ(*probes, 0);
  EXPECT_EQ(opens, 0);
  EXPECT_THAT(cache::ChildrenComplete(ctx_, d), IsOkAndHolds(false));
  EXPECT_FALSE(ctx_.db.InTransaction());
  interrupts.at = 0;
  EXPECT_EQ(Lookup(d, "f").first.error, 0);
  ctx_.interrupts = &NoInterrupts();
}

// A population interrupted between its probe batches stops: nothing of it
// is recorded (the directory stays incomplete), no guard or transaction is
// left, and the next listing populates it whole.
TEST_F(DirCacheFSTest, InterruptedPopulationRecordsNothing) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  constexpr int kNames = 200;
  for (int i = 0; i < kNames; ++i) WriteFile(Path(absl::StrCat("d/f", i)));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  // (Not traced: the trace spec's initial states range over every subset
  // of the names, 2^200 here; InterruptedPopulationIsTraced is.)
  // Its first checkpoints pass: probes run, then the interrupt is seen.
  interrupts.at = 3;
  std::shared_ptr<int> probes = CountProbes();
  EXPECT_EQ(ErrnoOf(List(d, false).status()), EINTR);
  EXPECT_GT(*probes, 0);
  EXPECT_LT(*probes, kNames);
  EXPECT_THAT(cache::ChildrenComplete(ctx_, d), IsOkAndHolds(false));
  EXPECT_EQ(Cached(d, "f0").first, LookupResult::Kind::kUnknown);
  EXPECT_TRUE(ctx_.fills.inflight.empty());
  EXPECT_FALSE(ctx_.db.InTransaction());
  interrupts.at = 0;
  absl::StatusOr<std::vector<std::string>> names = List(d, false);
  ASSERT_THAT(names, IsOk());
  EXPECT_EQ(names->size(), static_cast<size_t>(kNames + 2));
  EXPECT_THAT(cache::ChildrenComplete(ctx_, d), IsOkAndHolds(true));
  ctx_.interrupts = &NoInterrupts();
}

// The same at the population's first probe batch, traced (three names:
// the trace's "interrupt" line goes where the population started).
TEST_F(DirCacheFSTest, InterruptedPopulationIsTraced) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  for (const char *name : {"a", "b", "c"}) {
    WriteFile(Path(absl::StrCat("d/", name)));
  }
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  StartTrace();
  interrupts.at = 1;
  EXPECT_EQ(ErrnoOf(List(d, false).status()), EINTR);
  EXPECT_THAT(cache::ChildrenComplete(ctx_, d), IsOkAndHolds(false));
  interrupts.at = 0;
  EXPECT_THAT(List(d, false), IsOkAndHolds(::testing::SizeIs(5)));
  ctx_.interrupts = &NoInterrupts();
}

// A mutation interrupted before its backing syscall replies EINTR: the
// backing filesystem is untouched, the names phase 1 made unknown stay
// unknown and dirty, and the mutation's guard is released.
TEST_F(DirCacheFSTest, MutationInterruptedBeforeItsSyscallLeavesItUnknown) {
  WriteFile(Path("f"));
  Start();
  ASSERT_THAT(List(kRootInode, false), IsOk());  // f cached, no lookup.
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  StartTrace();
  interrupts.at = 1;
  EXPECT_EQ(Unlink(kRootInode, "f").error, -EINTR);
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("f")), IsOk());
  EXPECT_EQ(Cached(kRootInode, "f").first, LookupResult::Kind::kUnknown);
  EXPECT_THAT(Dirty(), Contains(kRootInode));
  EXPECT_TRUE(ctx_.fills.inflight.empty());
  interrupts.checkpoints = 0;
  EXPECT_EQ(Mkdir(kRootInode, "d").first.error, -EINTR);
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("d")).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(Cached(kRootInode, "d").first, LookupResult::Kind::kUnknown);
  EXPECT_TRUE(ctx_.fills.inflight.empty());
  EXPECT_FALSE(ctx_.db.InTransaction());
  interrupts.at = 0;
  EXPECT_EQ(Unlink(kRootInode, "f").error, 0);
  ctx_.interrupts = &NoInterrupts();
}

// Interrupted once its syscall has run (here: during its phase 3), a
// mutation completes and replies success: the change exists.
TEST_F(DirCacheFSTest, MutationInterruptedAfterItsSyscallCompletes) {
  WriteFile(Path("f"));
  Start();
  ASSERT_THAT(List(kRootInode, false), IsOk());  // f cached, no lookup.
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  StartTrace();
  // The first open by handle comes after the checkpoint: the backing
  // unlink's or phase 3's.
  OpenByHandleHook() = [&] { interrupts.at = 1; };
  EXPECT_EQ(Unlink(kRootInode, "f").error, 0);
  EXPECT_NE(interrupts.at, 0) << "the hook did not run";
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("f")).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(Cached(kRootInode, "f").first, LookupResult::Kind::kNegative);
  ctx_.interrupts = &NoInterrupts();
}

// End to end: the kernel's FUSE_INTERRUPT for the request being served,
// read from the device by SessionLoop at a checkpoint (as the daemon's
// loop does), stops a population.
TEST_F(DirCacheFSTest, ForgedFuseInterruptStopsAPopulation) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  for (int i = 0; i < 200; ++i) WriteFile(Path(absl::StrCat("d/f", i)));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  SessionLoop loop(se_);
  ctx_.interrupts = &loop;
  // (Not traced, as InterruptedPopulationRecordsNothing.)
  // The readdir about to be sent is next_unique_; its caller is
  // interrupted during the first probe.
  const uint64_t readdir = NextUnique();
  NameToHandleHook() = [&] { QueueInterrupt(readdir); };
  EXPECT_EQ(ErrnoOf(List(d, false).status()), EINTR);
  EXPECT_THAT(cache::ChildrenComplete(ctx_, d), IsOkAndHolds(false));
  EXPECT_TRUE(KernelReadsEmpty());
  // Not interrupted: served.
  EXPECT_THAT(List(d, false), IsOk());
  ctx_.interrupts = &NoInterrupts();
}

// A refused FUSE_INIT ends the loop with -EPROTO, as libfuse's own loop
// does (its private se->error): here the session asks for max_read=4096
// and DirCacheFS does not ("init() and fuse_session_new() requested
// different maximum read size").
TEST_F(DirCacheFSTest, RefusedInitEndsTheLoopWithEproto) {
  Start();
  // Its own Context: a DirCacheFS points its Context at its own state.
  MountFds mounts;
  Context ctx{db_, mounts, bitgen_};
  DirCacheFS other(ctx, options_);
  struct fuse_session *se = NewSession(&other, {"-o", "max_read=4096"});
  ASSERT_NE(se, nullptr);
  QueueInit();
  {
    SessionLoop loop(se);
    EXPECT_EQ(loop.Run(), -EPROTO);
  }
  fuse_session_destroy(se);
}

// default_permissions is required (README, "Flags"): a DirCacheFS whose
// mount options lack it refuses the mount, naming the option, and the loop
// ends with -EPROTO as for any refused INIT.
TEST_F(DirCacheFSTest, MountWithoutDefaultPermissionsIsRefused) {
  Start();
  MountFds mounts;
  Context ctx{db_, mounts, bitgen_};
  DirCacheFS::Options options = options_;
  options.mount_options = {"allow_other", "suid"};
  DirCacheFS other(ctx, options);
  struct fuse_session *se = NewSession(&other, {});
  ASSERT_NE(se, nullptr);
  WarningCapture capture;
  QueueInit();
  {
    SessionLoop loop(se);
    EXPECT_EQ(loop.Run(), -EPROTO);
  }
  fuse_session_destroy(se);
  EXPECT_THAT(capture.lines, Contains(HasSubstr("default_permissions")))
      << absl::StrJoin(capture.lines, "\n");
}

// A DirCacheFS made without mount options (a caller that forgot them) is
// refused like one whose options lack default_permissions: a missing value
// fails closed.
TEST_F(DirCacheFSTest, DefaultOptionsAreRefused) {
  Start();
  MountFds mounts;
  Context ctx{db_, mounts, bitgen_};
  DirCacheFS other(ctx, DirCacheFS::Options{});
  struct fuse_session *se = NewSession(&other, {});
  ASSERT_NE(se, nullptr);
  QueueInit();
  {
    SessionLoop loop(se);
    EXPECT_EQ(loop.Run(), -EPROTO);
  }
  fuse_session_destroy(se);
}

// A kernel that does not offer FUSE_CAP_DONT_MASK (or FUSE_CAP_POSIX_ACL)
// would check permissions differently from the backing filesystem: Init()
// fails, naming it, and the INIT is refused.
TEST_F(DirCacheFSTest, InitWithoutDontMaskIsRefused) {
  Start();
  MountFds mounts;
  Context ctx{db_, mounts, bitgen_};
  DirCacheFS other(ctx, options_);
  struct fuse_session *se = NewSession(&other, {});
  ASSERT_NE(se, nullptr);
  WarningCapture capture;
  QueueInit(FUSE_POSIX_ACL);
  {
    SessionLoop loop(se);
    EXPECT_EQ(loop.Run(), -EPROTO);
  }
  fuse_session_destroy(se);
  EXPECT_THAT(capture.lines,
              Contains(AllOf(HasSubstr("FAILED_PRECONDITION"),
                             HasSubstr("FUSE_CAP_DONT_MASK"))))
      << absl::StrJoin(capture.lines, "\n");
}

// FuseRequest on live requests: a session of the test's own handlers, each
// wrapping its request in a FuseRequest.
struct LiveReplies {
  absl::Status first;
  absl::Status second;
};
LiveReplies &Live() {
  static LiveReplies live;
  return live;
}

class FuseRequestLiveTest : public DirCacheFSTest {
 protected:
  // A session whose GETATTR replies twice and whose STATFS first calls
  // ReplyFailure with an OK status; it has seen FUSE_INIT.
  struct fuse_session *LiveSession() {
    ops_ = {};
    ops_.getattr = [](fuse_req_t req, fuse_ino_t, fuse_file_info *) {
      FuseRequest fr(req);
      Live().first = fr.ReplyErrno(ENOENT);
      Live().second = fr.ReplyErrno(0);
    };
    ops_.statfs = [](fuse_req_t req, fuse_ino_t) {
      FuseRequest fr(req);
      Live().first = fr.ReplyFailure(absl::OkStatus());
      Live().second = fr.ReplyErrno(EPERM);
    };
    struct fuse_session *se = NewSession(fs_.get(), {});
    if (se == nullptr) return nullptr;
    QueueInit();
    SessionLoop loop(se);
    EXPECT_EQ(loop.Run(), -EAGAIN);  // INIT served, nothing more queued
    return se;
  }
};

// A request replied to twice: the first reply goes to the kernel, the second
// is refused (Internal) instead of reaching libfuse with a freed request.
TEST_F(FuseRequestLiveTest, ReplyOnASpentRequestFailsRatherThanCrashing) {
  Start();
  struct fuse_session *se = LiveSession();
  ASSERT_NE(se, nullptr);
  struct fuse_getattr_in in = {};
  std::string body;
  AppendBytes(body, in);
  EXPECT_EQ(SendOn(se, FUSE_GETATTR, kRootInode, body).error, -ENOENT);
  EXPECT_THAT(Live().first, IsOk());
  EXPECT_EQ(Live().second.code(), absl::StatusCode::kInternal);
  fuse_session_destroy(se);
}

// ReplyFailure with an OK status is refused without consuming the request,
// which a later reply still answers.
TEST_F(FuseRequestLiveTest, ReplyFailureRejectsAnOkStatus) {
  Start();
  struct fuse_session *se = LiveSession();
  ASSERT_NE(se, nullptr);
  EXPECT_EQ(SendOn(se, FUSE_STATFS, kRootInode, "").error, -EPERM);
  EXPECT_EQ(Live().first.code(), absl::StatusCode::kInternal);
  EXPECT_THAT(Live().second, IsOk());
  fuse_session_destroy(se);
}

// The kernel sends ACCESS only when default_permissions is not in effect
// (fuse_permission, fs/fuse/dir.c). dcfs checks no permissions itself, so
// one arriving is denied, loudly: ENOSYS would make the kernel allow every
// later access(2) without asking (fc->no_access).
TEST_F(DirCacheFSTest, AccessFailsClosed) {
  Start();
  WarningCapture capture;
  struct fuse_access_in in = {};
  in.mask = R_OK;
  std::string body;
  AppendBytes(body, in);
  EXPECT_EQ(Send(FUSE_ACCESS, kRootInode, body).error, -EACCES);
  EXPECT_THAT(capture.lines,
              Contains(HasSubstr(
                  "ACCESS received: default_permissions is not in effect")))
      << absl::StrJoin(capture.lines, "\n");
}

// Interrupted at a failed mutation's re-resolve (after its syscall, whose
// error it would reply): the request stops there with EINTR. A rename does
// not re-resolve its second name then.
TEST_F(DirCacheFSTest, InterruptedReresolveStopsAFailedRename) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("a"), 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("b"), 0755), IsOk());
  WriteFile(Path("b/x"));
  Start();
  ASSERT_THAT(List(kRootInode, false), IsOk());
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  // The first checkpoint (before renameat2) passes; renameat2 fails
  // (ENOTEMPTY); the re-resolve of "a" is interrupted.
  interrupts.at = 2;
  EXPECT_EQ(Rename(kRootInode, "a", kRootInode, "b").error, -EINTR);
  EXPECT_EQ(interrupts.checkpoints, 2);
  EXPECT_EQ(Cached(kRootInode, "b").first, LookupResult::Kind::kUnknown);
  EXPECT_TRUE(ctx_.fills.inflight.empty());
  EXPECT_FALSE(ctx_.db.InTransaction());
  ctx_.interrupts = &NoInterrupts();
}

// The same for a mkdir whose syscall fails (EEXIST, a step the model has),
// traced: the re-resolve's interrupt is the model's Interrupt at its probe.
TEST_F(DirCacheFSTest, InterruptedReresolveIsTraced) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("a"), 0755), IsOk());
  Start();
  ASSERT_THAT(List(kRootInode, false), IsOk());
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  StartTrace();
  interrupts.at = 2;
  EXPECT_EQ(Mkdir(kRootInode, "a").first.error, -EINTR);
  EXPECT_EQ(Cached(kRootInode, "a").first, LookupResult::Kind::kUnknown);
  EXPECT_TRUE(ctx_.fills.inflight.empty());
  ctx_.interrupts = &NoInterrupts();
}

// What a checkpoint drains besides the interrupt: the first other message
// ends the drain (one read ahead, the rest left to the kernel) and is
// served after the interrupted request (SessionLoop::Run).
TEST_F(DirCacheFSTest, DrainedRequestIsServedAfterTheInterruptedOne) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path("d"), 0755), IsOk());
  for (int i = 0; i < 40; ++i) WriteFile(Path(absl::StrCat("d/f", i)));
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId d, Id("d"));
  SessionLoop loop(se_);
  ctx_.interrupts = &loop;
  const uint64_t readdir = NextUnique();
  struct fuse_getattr_in getattr_in = {};
  std::string getattr_body;
  AppendBytes(getattr_body, getattr_in);
  uint64_t first = 0, second = 0;
  NameToHandleHook() = [&] {
    QueueInterrupt(readdir);
    first = QueueRequest(FUSE_GETATTR, kRootInode, getattr_body);
    second = QueueRequest(FUSE_GETATTR, static_cast<uint64_t>(d),
                          getattr_body);
  };
  EXPECT_EQ(ErrnoOf(List(d, false).status()), EINTR);
  // The interrupt and the first GETATTR were read; the second was not.
  EXPECT_EQ(KernelReadsLeft(), 1u);
  EXPECT_EQ(TakeReply(first).error, -EIO);  // not served yet
  // The loop serves the queued GETATTR, then reads the next one.
  EXPECT_EQ(loop.Run(), -EAGAIN);  // nothing more to read
  EXPECT_EQ(TakeReply(first).error, 0);
  EXPECT_EQ(TakeReply(second).error, 0);
  ctx_.interrupts = &NoInterrupts();
}

// Every checkpoint before a backing syscall (AGENTS.md: each has a
// cancellation test): interrupted at its first checkpoint, the request
// replies EINTR, the backing file is unchanged, no mutation is in flight,
// no transaction is open, and what phase 1 made unknown stays unknown and
// dirty.
struct CheckpointCase {
  std::string name;
  // Sends the request (handles opened before are in `fh`); its reply.
  std::function<Reply(DirCacheFSTest &, InodeId f, uint64_t fh,
                      uint64_t fh2)>
      send;
  bool needs_open = false;  // f open O_RDWR (fh), g O_RDWR (fh2)
  bool attrs_unknown = true;  // f's attributes unknown and f dirty after
};

class CheckpointTest : public DirCacheFSTest,
                       public ::testing::WithParamInterface<CheckpointCase> {
 public:
  using DirCacheFSTest::CopyFileRange;
  using DirCacheFSTest::Fsyncdir;
  using DirCacheFSTest::Ioctl;
  using DirCacheFSTest::Link;
  using DirCacheFSTest::Open;
  using DirCacheFSTest::Rename;
  using DirCacheFSTest::Send;
  using DirCacheFSTest::Setxattr;
  using DirCacheFSTest::Chmod;
};

std::string FuseBody(const auto &in, std::string_view tail = "") {
  std::string body(reinterpret_cast<const char *>(&in), sizeof(in));
  body.append(tail);
  return body;
}

const CheckpointCase kCheckpointCases[] = {
    {.name = "rename",
     .send = [](DirCacheFSTest &t, InodeId, uint64_t, uint64_t) {
       return static_cast<CheckpointTest &>(t).Rename(kRootInode, "f",
                                                      kRootInode, "h");
     },
     .attrs_unknown = false},
    {.name = "link",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t, uint64_t) {
       return static_cast<CheckpointTest &>(t).Link(f, kRootInode, "l");
     }},
    {.name = "setattr",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t, uint64_t) {
       return static_cast<CheckpointTest &>(t).Chmod(f, S_IFREG | 0600);
     }},
    {.name = "setxattr",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t, uint64_t) {
       return static_cast<CheckpointTest &>(t).Setxattr(f, "user.new", "v");
     }},
    {.name = "removexattr",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t, uint64_t) {
       std::string name = "user.old";
       name.push_back('\0');
       return static_cast<CheckpointTest &>(t).Send(
           FUSE_REMOVEXATTR, static_cast<uint64_t>(f), name);
     }},
    {.name = "write",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t fh, uint64_t) {
       struct fuse_write_in in = {};
       in.fh = fh;
       in.size = 1;
       return static_cast<CheckpointTest &>(t).Send(
           FUSE_WRITE, static_cast<uint64_t>(f), FuseBody(in, "x"));
     },
     .needs_open = true},
    {.name = "fallocate",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t fh, uint64_t) {
       struct fuse_fallocate_in in = {};
       in.fh = fh;
       in.length = 1 << 20;
       return static_cast<CheckpointTest &>(t).Send(
           FUSE_FALLOCATE, static_cast<uint64_t>(f), FuseBody(in));
     },
     .needs_open = true},
    {.name = "copy_file_range",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t fh, uint64_t fh2) {
       struct fuse_copy_file_range_in in = {};
       in.fh_in = fh2;
       in.nodeid_out = static_cast<uint64_t>(f);
       in.fh_out = fh;
       in.len = 5;
       in.off_out = 5;
       return static_cast<CheckpointTest &>(t).Send(
           FUSE_COPY_FILE_RANGE, static_cast<uint64_t>(f), FuseBody(in));
     },
     .needs_open = true},
    {.name = "ioctl",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t fh, uint64_t) {
       const int flags = FS_NODUMP_FL;
       return static_cast<CheckpointTest &>(t).Ioctl(
           f, FS_IOC_SETFLAGS,
           std::string_view(reinterpret_cast<const char *>(&flags),
                            sizeof(flags)),
           0);
     },
     .needs_open = true},
    {.name = "cold_open",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t, uint64_t) {
       return static_cast<CheckpointTest &>(t).Open(f, O_RDWR).first;
     },
     .attrs_unknown = false},
    {.name = "fsync",
     .send = [](DirCacheFSTest &t, InodeId f, uint64_t fh, uint64_t) {
       struct fuse_fsync_in in = {};
       in.fh = fh;
       return static_cast<CheckpointTest &>(t).Send(
           FUSE_FSYNC, static_cast<uint64_t>(f), FuseBody(in));
     },
     .needs_open = true},
    {.name = "fsyncdir",
     .send = [](DirCacheFSTest &t, InodeId, uint64_t, uint64_t) {
       return static_cast<CheckpointTest &>(t).Fsyncdir(kRootInode);
     },
     .attrs_unknown = false},
};

TEST_P(CheckpointTest, InterruptedAtItsFirstCheckpoint) {
  const CheckpointCase &c = GetParam();
  WriteFile(Path("f"));
  AppendToFile(Path("f"), "hello");
  WriteFile(Path("g"));
  AppendToFile(Path("g"), "world");
  const uint8_t v[] = {'v'};
  ASSERT_THAT(syscalls::setxattr(Path("f"), "user.old", v, 0), IsOk());
  Start();
  ASSERT_THAT(List(kRootInode, false), IsOk());
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  ASSERT_OK_AND_ASSIGN(InodeId g, Id("g"));
  uint64_t fh = 0, fh2 = 0;
  if (c.needs_open) {
    auto [open, h] = Open(f, O_RDWR);
    ASSERT_EQ(open.error, 0);
    fh = h;
    auto [open2, h2] = Open(g, O_RDWR);
    ASSERT_EQ(open2.error, 0);
    fh2 = h2;
  }
  ASSERT_THAT(Fsyncdir(kRootInode).error, 0);  // A sync point: all clean.
  ASSERT_OK_AND_ASSIGN(struct stat before,
                       syscalls::fstatat(AT_FDCWD, Path("f")));
  FakeInterrupts interrupts;
  ctx_.interrupts = &interrupts;
  interrupts.at = 1;
  EXPECT_EQ(c.send(*this, f, fh, fh2).error, -EINTR);
  EXPECT_EQ(interrupts.checkpoints, 1);
  ctx_.interrupts = &NoInterrupts();

  // The backing file is as it was.
  ASSERT_OK_AND_ASSIGN(struct stat after,
                       syscalls::fstatat(AT_FDCWD, Path("f")));
  EXPECT_EQ(after.st_size, before.st_size);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_EQ(after.st_nlink, before.st_nlink);
  EXPECT_EQ(ReadWholeFile(Path("f")), "hello");
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("h")).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, Path("l")).status(),
              ::absl_testing::StatusIs(absl::StatusCode::kNotFound));
  // Nothing in flight, no transaction open.
  EXPECT_TRUE(ctx_.fills.inflight.empty());
  EXPECT_FALSE(ctx_.db.InTransaction());
  // Phase 1's records stay unknown and dirty.
  if (c.attrs_unknown) {
    ASSERT_OK_AND_ASSIGN(cache::CachedAttr attr, cache::GetAttr(ctx_, f));
    EXPECT_FALSE(attr.valid);
    EXPECT_THAT(Dirty(), Contains(f));
  }
  if (c.name == "rename") {
    EXPECT_EQ(Cached(kRootInode, "f").first, LookupResult::Kind::kUnknown);
    EXPECT_EQ(Cached(kRootInode, "h").first, LookupResult::Kind::kUnknown);
    EXPECT_THAT(Dirty(), Contains(kRootInode));
  }
  if (c.name == "link") {
    EXPECT_EQ(Cached(kRootInode, "l").first, LookupResult::Kind::kUnknown);
  }
  if (c.name == "cold_open") EXPECT_FALSE(fs_->HasOpenFiles(f));
}

INSTANTIATE_TEST_SUITE_P(
    EveryCheckpoint, CheckpointTest, ::testing::ValuesIn(kCheckpointCases),
    [](const ::testing::TestParamInfo<CheckpointCase> &info) {
      return info.param.name;
    });

// --- The fault sweep (step 26.6) ---------------------------------------------
//
// Short workloads, one per operation type, each run once recording the
// backing call sites it reaches (a site: a ProtocolEvents::BackingCall
// hook's location and the k-th wrapped libc call after it, see FaultSweep);
// then, for each site, the workload again on a fresh tree and cache with
// that site's first call failed: with EIO, and with every errno the code
// branches on for that call (ErrnosFor). After each run: the invariant
// checks over everything (CheckAll, the status API), then a recovery (an
// unclean Startup on the same database, as after a crash right then) and
// the checks again. Bounded by call site, never "every N-th call of a long
// workload". A violation is a finding: the test fails, naming it.

// One fresh fixture per sweep run: its own source tree, cache database,
// DirCacheFS and checker (in record mode: a violation is collected, not
// fatal).
class FaultIteration : public DirCacheFSTest {
 public:
  void TestBody() override {}

  void Begin() {
    SetUp();
    MakeTree();
    Start();
    checker_->RecordViolations(true);
    checker_->ObserveBackingCalls(SweepAtHook);
  }

  void End() {
    checker_->ObserveBackingCalls({});
    TearDown();
  }

  // For the self-check: the in-memory flag says the dirty set is empty.
  void BreakDirtyAny() { ctx_.dirty.any = false; }

  // What every workload starts from.
  void MakeTree() {
    for (const char *f : {"f1", "f2", "f3"}) {
      WriteFile(Path(f));
      AppendToFile(Path(f), "contents");
    }
    for (const char *d : {"d", "e1", "e2"}) {
      ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, Path(d), 0755), IsOk());
    }
    WriteFile(Path("d/g1"));
    WriteFile(Path("d/g2"));
    const std::string value = "v";
    ASSERT_THAT(syscalls::setxattr(Path("f1"), "user.k",
                                   std::span<const uint8_t>(
                                       reinterpret_cast<const uint8_t *>(
                                           value.data()),
                                       value.size()),
                                   0),
                IsOk());
    ASSERT_THAT(syscalls::symlinkat("f1", AT_FDCWD, Path("s")), IsOk());
  }

  InodeId L(InodeId parent, std::string_view name) {
    auto [reply, entry] = Lookup(parent, name);
    return reply.error == 0 ? static_cast<InodeId>(entry.nodeid) : 0;
  }

  Reply Simple(uint32_t opcode, InodeId id, std::string_view name) {
    std::string body(name);
    body.push_back('\0');
    return Send(opcode, static_cast<uint64_t>(id), body);
  }

  Reply Write(InodeId id, uint64_t fh, std::string_view data, uint64_t off) {
    struct fuse_write_in in = {};
    in.fh = fh;
    in.offset = off;
    in.size = static_cast<uint32_t>(data.size());
    std::string body;
    AppendBytes(body, in);
    body.append(data);
    return Send(FUSE_WRITE, static_cast<uint64_t>(id), body);
  }

  Reply Fsync(InodeId id, uint64_t fh) {
    struct fuse_fsync_in in = {};
    in.fh = fh;
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_FSYNC, static_cast<uint64_t>(id), body);
  }

  Reply Setattr(InodeId id, uint32_t valid, uint64_t size) {
    struct fuse_setattr_in in = {};
    in.valid = valid;
    in.size = size;
    in.atime = 1000;
    in.mtime = 2000;
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_SETATTR, static_cast<uint64_t>(id), body);
  }

  Reply Listxattr(InodeId id) {
    struct fuse_getxattr_in in = {};
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_LISTXATTR, static_cast<uint64_t>(id), body);
  }

  Reply Releasedir(InodeId id) {
    struct fuse_release_in in = {};
    std::string body;
    AppendBytes(body, in);
    return Send(FUSE_RELEASEDIR, static_cast<uint64_t>(id), body);
  }

  void OpenRelease(InodeId id, int flags) {
    if (id == 0) return;
    auto [open, fh] = Open(id, flags);
    if (open.error == 0) Release(id, fh);
  }

  // The workloads: about ten requests each, ignoring their replies (a
  // failed request is the point; what it leaves behind is what is checked).
  void Run(std::string_view workload) {
    if (workload == "lookup") {
      for (const char *n : {"f1", "f2", "f3", "d", "s", "missing"}) {
        L(kRootInode, n);
      }
      if (InodeId d = L(kRootInode, "d"); d != 0) {
        L(d, "g1");
        L(d, "missing");
      }
      if (InodeId f = L(kRootInode, "f1"); f != 0) Getattr(f);
    } else if (workload == "create") {
      for (const char *n : {"c1", "c2", "c3"}) {
        Created c = Create(kRootInode, n, O_RDWR);
        if (c.reply.error == 0) Release(c.id, c.fh);
      }
      if (InodeId d = L(kRootInode, "d"); d != 0) {
        Created c = Create(d, "c4", O_WRONLY);
        if (c.reply.error == 0) Release(c.id, c.fh);
      }
      Create(kRootInode, "f1", O_RDWR | O_EXCL);  // EEXIST
    } else if (workload == "write") {
      if (InodeId f = L(kRootInode, "f1"); f != 0) {
        auto [open, fh] = Open(f, O_RDWR);
        if (open.error == 0) {
          Write(f, fh, "abc", 0);
          Write(f, fh, "defgh", 3);
          Fsync(f, fh);
          Release(f, fh);
        }
        OpenRelease(f, O_RDONLY);
        Getattr(f);
      }
    } else if (workload == "unlink") {
      L(kRootInode, "f1");
      L(kRootInode, "f2");
      Unlink(kRootInode, "f1");
      Unlink(kRootInode, "f2");
      Unlink(kRootInode, "missing");
      if (InodeId d = L(kRootInode, "d"); d != 0) {
        L(d, "g1");
        Unlink(d, "g1");
        Unlink(d, "g2");
      }
    } else if (workload == "rename") {
      InodeId d = L(kRootInode, "d");
      L(kRootInode, "f1");
      Rename(kRootInode, "f1", kRootInode, "r1");
      Rename(kRootInode, "f2", kRootInode, "f3");  // over an existing file
      if (d != 0) {
        Rename(kRootInode, "f3", d, "g1");
        Rename(d, "g2", kRootInode, "r2");
      }
      Rename(kRootInode, "missing", kRootInode, "r3");
    } else if (workload == "mkdir") {
      Mkdir(kRootInode, "m1");
      Mkdir(kRootInode, "m2");
      Mkdir(kRootInode, "d");  // EEXIST
      if (InodeId d = L(kRootInode, "d"); d != 0) Mkdir(d, "m3");
      if (InodeId m = L(kRootInode, "m1"); m != 0) Mkdir(m, "m4");
    } else if (workload == "rmdir") {
      L(kRootInode, "e1");
      Simple(FUSE_RMDIR, kRootInode, "e1");
      Simple(FUSE_RMDIR, kRootInode, "e2");
      Simple(FUSE_RMDIR, kRootInode, "d");  // ENOTEMPTY
      Simple(FUSE_RMDIR, kRootInode, "missing");
      Simple(FUSE_RMDIR, kRootInode, "f1");  // ENOTDIR
    } else if (workload == "setattr") {
      if (InodeId f = L(kRootInode, "f1"); f != 0) {
        Chmod(f, 0600);
        Setattr(f, FATTR_SIZE, 2);
        Setattr(f, FATTR_ATIME | FATTR_MTIME, 0);
        Getattr(f);
      }
      if (InodeId d = L(kRootInode, "d"); d != 0) Chmod(d, 0700);
      if (InodeId s = L(kRootInode, "s"); s != 0) {
        Setattr(s, FATTR_ATIME | FATTR_MTIME, 0);
      }
    } else if (workload == "xattr") {
      if (InodeId f = L(kRootInode, "f1"); f != 0) {
        Setxattr(f, "user.a", "1");
        Getxattr(f, "user.a");
        Getxattr(f, "user.k");
        Getxattr(f, "user.missing");
        Listxattr(f);
        Simple(FUSE_REMOVEXATTR, f, "user.a");
        Simple(FUSE_REMOVEXATTR, f, "user.missing");
      }
      if (InodeId d = L(kRootInode, "d"); d != 0) {
        Setxattr(d, "user.b", "2");
        Listxattr(d);
      }
    } else if (workload == "readdir") {
      Opendir(kRootInode);
      (void)List(kRootInode, true);
      (void)List(kRootInode, false);
      Releasedir(kRootInode);
      if (InodeId d = L(kRootInode, "d"); d != 0) {
        Opendir(d);
        (void)List(d, true);
        Releasedir(d);
      }
    } else if (workload == "open-release") {
      InodeId f = L(kRootInode, "f1");
      OpenRelease(f, O_RDONLY);
      OpenRelease(f, O_RDWR);
      OpenRelease(f, O_WRONLY | O_TRUNC);
      OpenRelease(L(kRootInode, "f2"), O_RDWR | O_APPEND);
      OpenRelease(L(kRootInode, "d"), O_RDONLY | O_DIRECTORY);
    } else if (workload == "forget") {
      InodeId f1 = L(kRootInode, "f1");
      L(kRootInode, "f1");
      InodeId f2 = L(kRootInode, "f2");
      InodeId d = L(kRootInode, "d");
      InodeId f3 = L(kRootInode, "f3");
      OpenRelease(f3, O_RDWR);  // written: its last FORGET reconciles it
      if (f1 != 0) Forget(f1, 2);
      std::vector<std::pair<InodeId, uint64_t>> batch;
      for (InodeId id : {f2, d}) {
        if (id != 0) batch.emplace_back(id, 1);
      }
      if (!batch.empty()) BatchForget(batch);
      if (f3 != 0) Forget(f3, 1);
    }
  }

  // The invariants now, then after an unclean start on the same database
  // (Startup in a new Context: the process died right here). Every
  // violation, as found.
  std::vector<std::string> CheckAndRecover() {
    std::vector<std::string> found;
    if (absl::Status s = checker_->CheckAll(ctx_, fs_.get()); !s.ok()) {
      found.push_back(absl::StrCat("after the workload: ", s.message()));
    }
    MountFds mounts;
    Context ctx{db_, mounts, bitgen_};
    ctx.events = observers_.get();
    absl::StatusOr<FileDescriptor> source =
        syscalls::openat(AT_FDCWD, source_, O_RDONLY | O_DIRECTORY);
    absl::Status started =
        !source.ok() ? source.status()
                     : SetCleanShutdown(db_, false).ok()
                           ? backing::Startup(ctx, *std::move(source), "boot")
                           : absl::InternalError("SetCleanShutdown");
    if (!started.ok()) {
      found.push_back(absl::StrCat("recovery failed: ", started.ToString()));
    } else if (absl::Status s = checker_->CheckAll(ctx, nullptr); !s.ok()) {
      found.push_back(absl::StrCat("after recovery: ", s.message()));
    }
    for (const std::string &v : checker_->violations()) {
      found.push_back(absl::StrCat("hook: ", v));
    }
    // From here on ctx_ is the dead process's memory (see Restart).
    observers_->Remove(checker_.get());
    return found;
  }
};

// `err`'s name, for a finding.
std::string ErrnoName(int err) {
  for (const auto &[name, value] : ErrnoNameTable()) {
    if (value == err) return name;
  }
  return absl::StrCat("errno ", err);
}

// The errnos to fail `call` with: EIO, and those the code branches on for
// it (backing.cc, dir_cache_fs.cc, device_id.cc): ENOENT (a probe's
// openat: the name vanished), EACCES (MakeBackingFile's read-only
// fallback), ESTALE and EPERM (OpenNode's open_by_handle_at), ENODATA,
// EOPNOTSUPP and ERANGE (the xattr reads and their retry). EEXIST, ENOSPC
// and ENAMETOOLONG have no branch: EIO stands for them.
std::vector<int> ErrnosFor(std::string_view call) {
  if (call == "openat") return {EIO, ENOENT, EACCES};
  if (call == "open_by_handle_at") return {EIO, ESTALE, EPERM};
  if (call == "getxattr" || call == "listxattr" || call == "fgetxattr" ||
      call == "flistxattr") {
    return {EIO, ENODATA, EOPNOTSUPP, ERANGE};
  }
  return {EIO};
}

constexpr const char *kWorkloads[] = {
    "lookup", "create", "write",   "unlink",       "rename", "mkdir",
    "rmdir",  "setattr", "xattr", "readdir", "open-release", "forget"};

struct SweepResult {
  int hooks = 0;      // hook locations reached
  int calls = 0;      // new sites failed
  int iterations = 0;
  std::vector<std::string> silent;  // hooks with no wrapped call after them
  std::vector<std::string> findings;
};

// Records `workload`'s sites, then fails each in turn that is not in
// `done` (a site an earlier workload already failed: bounded by call
// site, each is failed once), adding it. `breaker` (the self-check) runs
// after each faulted workload; at most `max_iterations` if not -1.
SweepResult SweepWorkload(std::string_view workload,
                          absl::flat_hash_set<std::string> &done,
                          const std::function<void(FaultIteration &)> &breaker =
                              {},
                          int max_iterations = -1) {
  SweepResult result;
  FaultSweep &f = Sweep();
  f = FaultSweep();
  {
    FaultIteration it;
    it.Begin();
    f.recording = true;
    it.Run(workload);
    f.recording = false;
    f.at.clear();
    for (const std::string &v : it.CheckAndRecover()) {
      result.findings.push_back(absl::StrCat(workload, " (no fault): ", v));
    }
    it.End();
  }
  const std::vector<FaultSweep::Site> sites = f.sites;
  result.hooks = static_cast<int>(f.hooks.size());
  for (const std::string &hook : f.hooks) {
    bool calls = false;
    for (const FaultSweep::Site &site : sites) {
      calls = calls || site.hook == hook;
    }
    if (!calls && done.insert(hook).second) result.silent.push_back(hook);
  }
  for (const FaultSweep::Site &site : sites) {
    if (result.iterations == max_iterations) break;
    const std::string key =
        absl::StrCat(site.hook, " ", site.call, " #", site.j);
    if (!done.insert(key).second) continue;
    ++result.calls;
    for (int err : ErrnosFor(site.call)) {
      f = FaultSweep();
      FaultIteration it;
      it.Begin();  // (Start's own backing calls are not the workload's.)
      f.target = site;
      f.err = err;
      it.Run(workload);
      const std::string fired = f.fired;
      f = FaultSweep();  // Nothing fails during the checks and recovery.
      if (breaker) breaker(it);
      const std::string what = absl::StrCat(workload, ": ", key,
                                            " failed with ", ErrnoName(err));
      if (fired.empty()) {
        result.findings.push_back(absl::StrCat(what, ": never reached"));
      }
      for (const std::string &v : it.CheckAndRecover()) {
        result.findings.push_back(absl::StrCat(what, ": ", v));
      }
      it.End();
      ++result.iterations;
    }
  }
  return result;
}

TEST(FaultSitesTest, EveryBackingCallSiteFailedOnce) {
  const auto start = std::chrono::steady_clock::now();
  SweepResult all;
  absl::flat_hash_set<std::string> done;
  for (const char *workload : kWorkloads) {
    const auto began = std::chrono::steady_clock::now();
    SweepResult r = SweepWorkload(workload, done);
    std::cout << "FAULT-SWEEP " << workload << ": " << r.hooks
              << " hooks reached, " << r.calls << " new sites, " << r.iterations
              << " iterations, "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - began)
                     .count()
              << " ms" << std::endl;
    all.hooks += r.hooks;
    all.calls += r.calls;
    all.iterations += r.iterations;
    all.silent.insert(all.silent.end(), r.silent.begin(), r.silent.end());
    all.findings.insert(all.findings.end(), r.findings.begin(),
                        r.findings.end());
  }
  std::cout << "FAULT-SWEEP total: " << all.calls
            << " sites, " << all.iterations << " iterations, "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count()
            << " ms; hooks with no wrapped call after them (not failed): "
            << absl::StrJoin(all.silent, " ") << std::endl;
  EXPECT_GT(all.calls, 0);
  EXPECT_THAT(all.findings, ::testing::IsEmpty());
}

// The sweep's self-check: an iteration that breaks an invariant is
// reported, naming the site it failed.
TEST(FaultSitesTest, SweepReportsABrokenInvariant) {
  absl::flat_hash_set<std::string> done;
  SweepResult r = SweepWorkload(
      "mkdir", done,
      [](FaultIteration &it) {
        // A phase 1 left its dirty row; the in-memory flag says none.
        it.BreakDirtyAny();
      },
      /*max_iterations=*/3);
  ASSERT_GT(r.iterations, 0);
  EXPECT_THAT(r.findings,
              Contains(::testing::HasSubstr(
                  "dirty-set: Context::dirty.any is false")));
}

// --- Slopes (step 26.4b) -------------------------------------------------------
//
// Every operation class, N times in one directory at N = 100 and 1000: the
// SQLite steps, transactions, durable transactions (each a WAL fsync:
// Durability::kSync) and backing syscalls (the wrapped libc calls) of the N
// operations are bounded by a * N + b, a and b today's numbers; a cost that
// grows with the directory, or per operation, exceeds them at N = 1000.

struct Slope {
  int64_t steps = 0;
  int64_t transactions = 0;
  int64_t durable = 0;  // WAL fsyncs
  int64_t backing = 0;  // backing syscalls
};

class SlopeRun : public DirCacheFSTest {
 public:
  void TestBody() override {}

  // `extra`, if given, runs after each operation (the self-check's added
  // cost).
  Slope Measure(std::string_view op, int n,
                const std::function<void(SlopeRun &)> &extra = {}) {
    SetUp();
    const bool existing = op != "create" && op != "mkdir";
    if (existing) {
      for (int i = 0; i < n; ++i) WriteFile(Path(absl::StrCat("f", i)));
    }
    Start();
    if (existing && op != "cold-lookup") {
      for (int i = 0; i < n; ++i) Lookup(kRootInode, absl::StrCat("f", i));
    }
    counter_.Reset();
    WrappedCalls() = 0;
    for (int i = 0; i < n; ++i) {
      const std::string name = absl::StrCat("f", i);
      if (op == "create") {
        Created c = Create(kRootInode, name, O_WRONLY);
        EXPECT_EQ(c.reply.error, 0) << name;
        Release(c.id, c.fh);
      } else if (op == "mkdir") {
        EXPECT_EQ(Mkdir(kRootInode, name).first.error, 0) << name;
      } else if (op == "unlink") {
        EXPECT_EQ(Unlink(kRootInode, name).error, 0) << name;
      } else if (op == "rename") {
        EXPECT_EQ(Rename(kRootInode, name, kRootInode, absl::StrCat("r", i))
                      .error,
                  0)
            << name;
      } else if (op == "cold-lookup") {
        EXPECT_EQ(Lookup(kRootInode, name).first.error, 0) << name;
      } else if (op == "setattr") {
        auto [lookup, entry] = Lookup(kRootInode, name);
        EXPECT_EQ(Chmod(static_cast<InodeId>(entry.nodeid), 0600).error, 0)
            << name;
      }
      if (extra) extra(*this);
    }
    const testonly::CostCounter::Counts &c = counter_.counts();
    const Slope slope{.steps = c.steps,
                      .transactions = c.transactions,
                      .durable = c.durable_transactions,
                      .backing = WrappedCalls()};
    TearDown();
    std::cout << "SLOPE " << op << " n=" << n << " steps=" << slope.steps
              << " transactions=" << slope.transactions
              << " durable=" << slope.durable << " backing=" << slope.backing
              << std::endl;
    return slope;
  }

  // Two more statement steps: a row and the end (the self-check's
  // regression).
  void ExtraStep() { ASSERT_THAT(db_.Exec("SELECT 1"), IsOk()); }
};

// The bounds: a per operation and b per run, for steps, transactions, WAL
// fsyncs and backing syscalls; today's numbers exactly (the counts are
// deterministic). Raising one is a deliberate edit whose commit says why.
//
// The fsyncs: a create (O_WRONLY, as creat(2) and every shell redirection
// open), an unlink, a rename and a setattr each cost one WAL fsync, every
// time, in one directory: the phase 1 of an inode that is not yet durably
// dirty is durable, and each of those names a new one (a create's writable
// open, its new file; an unlink or a rename, the object it removes or
// moves; a setattr, its inode). Only the directory's own dirty row is
// durable once per sync interval (mkdir: one fsync for N). docs/design.md's
// "a burst of creates in one directory costs one WAL fsync" holds for
// mkdir, not for create.
struct SlopeBound {
  const char *op;
  int64_t steps_a, steps_b;
  int64_t transactions_a, transactions_b;
  int64_t durable_a, durable_b;
  int64_t backing_a, backing_b;
};
constexpr SlopeBound kSlopeBounds[] = {
    {"create", 82, 3, 7, 0, 1, 1, 14, 64},
    {"mkdir", 48, 3, 3, 0, 0, 1, 8, 0},
    {"unlink", 38, 0, 4, 0, 1, 0, 8, 0},
    {"rename", 59, 0, 4, 0, 1, 0, 9, 0},
    {"cold-lookup", 20, 16, 0, 1, 0, 0, 5, 2},
    {"setattr", 35, 0, 3, 0, 1, 0, 13, 0},
};

// Whether `s`, `bound.op` at N = `n`, is within its bounds (each count
// failing is reported).
bool WithinBounds(const SlopeBound &bound, int64_t n, const Slope &s) {
  bool within = true;
  auto check = [&](const char *what, int64_t count, int64_t a, int64_t b) {
    if (count <= a * n + b) return;
    within = false;
    ADD_FAILURE() << bound.op << " n=" << n << ": " << what << " " << count
                  << " > " << a << " * n + " << b;
  };
  check("steps", s.steps, bound.steps_a, bound.steps_b);
  check("transactions", s.transactions, bound.transactions_a,
        bound.transactions_b);
  check("WAL fsyncs", s.durable, bound.durable_a, bound.durable_b);
  check("backing syscalls", s.backing, bound.backing_a, bound.backing_b);
  return within;
}

TEST(SlopeTest, EveryOperationClassIsBoundedByANPlusB) {
  for (const SlopeBound &bound : kSlopeBounds) {
    for (int64_t n : {100, 1000}) {
      SlopeRun run;
      WithinBounds(bound, n, run.Measure(bound.op, static_cast<int>(n)));
    }
  }
}

// The bounds' self-check: two more statement steps per create (a
// regression's cost) is caught.
TEST(SlopeTest, AnExtraCostPerOperationIsCaught) {
  SlopeRun run;
  const Slope s = run.Measure("create", 100,
                              [](SlopeRun &r) { r.ExtraStep(); });
  bool within = true;
  EXPECT_NONFATAL_FAILURE(within = WithinBounds(kSlopeBounds[0], 100, s),
                          "create n=100: steps 8403 > 82 * n + 3");
  EXPECT_FALSE(within);
}

// Without passthrough for a file (the harness's passthrough_open fails:
// ENOTTY), the kernel sends READ and dcfs serves it from the backing file.
TEST_F(DirCacheFSTest, ReadWithoutPassthroughReadsTheBackingFile) {
  WriteFile(Path("f"));
  AppendToFile(Path("f"), "hello world");
  Start();
  ASSERT_OK_AND_ASSIGN(InodeId f, Id("f"));
  auto [open, fh] = Open(f, O_RDONLY);
  ASSERT_EQ(open.error, 0);
  struct fuse_open_out out {};
  ASSERT_GE(open.payload.size(), sizeof(out));
  std::memcpy(&out, open.payload.data(), sizeof(out));
  EXPECT_EQ(out.backing_id, 0);
  auto fuse_read = [&](uint64_t offset, uint32_t size) {
    struct fuse_read_in in = {};
    in.fh = fh;
    in.offset = offset;
    in.size = size;
    return Send(FUSE_READ, static_cast<uint64_t>(f), FuseBody(in));
  };
  Reply middle = fuse_read(6, 5);
  EXPECT_EQ(middle.error, 0);
  EXPECT_EQ(middle.payload, "world");
  Reply whole = fuse_read(0, 4096);
  EXPECT_EQ(whole.error, 0);
  EXPECT_EQ(whole.payload, "hello world");
  Reply past_end = fuse_read(100, 10);
  EXPECT_EQ(past_end.error, 0);
  EXPECT_EQ(past_end.payload, "");
  EXPECT_EQ(Release(f, fh).error, 0);
}

}  // namespace
}  // namespace dcfs
