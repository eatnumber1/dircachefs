// Regression test for 0795d12: FuseRequest::Reply*() helpers must build
// their failure Status via dcfs::ErrnoToStatus() (which attaches the
// kErrnoTypeUrl payload GetErrnoFromStatus() needs to recover the original
// errno), not absl::ErrnoToStatus() (which carries no payload) -- so a
// caller that needs to know *why* sending a reply failed (e.g. a broken
// FUSE channel) can actually recover the specific errno from the returned
// Status, rather than just an opaque failure.
//
// Exercising this needs a real fuse_req_t: every Reply* method RET_CHECKs
// that it has one, and there is no way to manufacture one without a real
// libfuse session -- but not a real kernel mount. libfuse's
// fuse_session_custom_io() (added for virtiofsd and other non-kernel
// transports) lets a caller supply its own read()/writev() instead of a
// /dev/fuse fd; combined with fuse_session_process_buf() (which accepts a
// forged request buffer directly, bypassing read() entirely), this test
// forges a minimal wire-format FUSE_INIT (mandatory before any other
// opcode) followed by one FUSE_GETATTR, and makes the *reply* write for
// the GETATTR fail with a chosen errno by returning -1/errno from its own
// writev callback -- deterministic fault injection with no real kernel or
// mountpoint involved.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "dcfs/credentials.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_request.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

// State the test's fake io/op callbacks share, reached via the
// fuse_session's userdata pointer.
struct FakeChannel {
  // Set by the .getattr op handler below once FuseRequest::ReplyErrno()
  // returns.
  std::optional<absl::Status> reply_status;
  // The errno the *next* writev() call should fail with (0 means: succeed
  // normally, as the FUSE_INIT reply must for the session to reach
  // got_init at all).
  int next_writev_errno = 0;
  // If set, FakeGetattr replies with ReplyFailureAndLogIfNotOk(*fail_with),
  // the way fuse_ops.cc's Serve() does, instead of ReplyErrno(ENOENT).
  std::optional<absl::Status> fail_with;
  // If set, FakeGetattr sends a success reply (which the channel fails with
  // next_writev_errno) and hands its status to ReplyFailureAndLogIfNotOk.
  bool reply_attr_then_fail = false;
  // The pid, uid and gid the forged GETATTR carries.
  uint32_t request_pid = 0;
  uint32_t request_uid = 0;
  uint32_t request_gid = 0;
  // If set, FakeGetattr stores FuseRequest::Caller()'s result in `caller`.
  bool read_caller = false;
  std::optional<absl::StatusOr<Credentials>> caller;
};

// Never actually called: this test drives the session entirely via
// fuse_session_process_buf() with hand-built buffers, never
// fuse_session_receive_buf(), but fuse_session_custom_io() requires a
// non-null read callback to accept the io vtable at all.
ssize_t FakeReadNeverCalled(int, void *, size_t, void *) {
  ADD_FAILURE() << "custom io read() should never be called by this test";
  errno = ENOSYS;
  return -1;
}

ssize_t FakeWritev(int, struct iovec *iov, int count, void *userdata) {
  auto *channel = static_cast<FakeChannel *>(userdata);
  if (channel->next_writev_errno != 0) {
    errno = channel->next_writev_errno;
    return -1;
  }
  ssize_t total = 0;
  for (int i = 0; i < count; ++i) total += static_cast<ssize_t>(iov[i].iov_len);
  return total;
}

// The op handler under test: wraps the incoming request in a real
// dcfs::FuseRequest, exactly as DirCacheFS's trampolines (fuse_ops.cc) do,
// and calls ReplyErrno() -- one representative of every Reply* method,
// all of which follow the identical
// `dcfs::ErrnoToStatus(-fuse_reply_X(...), ...)` pattern this commit
// covers.
void FakeGetattr(fuse_req_t req, fuse_ino_t, struct fuse_file_info *) {
  auto *channel = static_cast<FakeChannel *>(fuse_req_userdata(req));
  FuseRequest fr(req);
  if (channel->read_caller) channel->caller = fr.Caller();
  if (channel->reply_attr_then_fail) {
    struct stat st = {};
    absl::Status sent = fr.ReplyAttr(st, absl::Seconds(1));
    fr.ReplyFailureAndLogIfNotOk(sent);
    channel->reply_status = absl::OkStatus();
    return;
  }
  if (channel->fail_with.has_value()) {
    fr.ReplyFailureAndLogIfNotOk(*channel->fail_with);
    channel->reply_status = absl::OkStatus();
    return;
  }
  channel->reply_status = fr.ReplyErrno(ENOENT);
}

// Appends the raw bytes of `value` to `out`.
template <typename T>
void AppendBytes(std::string &out, const T &value) {
  out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

// Runs one forged GETATTR through a session whose handler is FakeGetattr,
// with `channel` as its state. The session is destroyed before returning.
void RunGetattr(FakeChannel &channel) {
  struct fuse_lowlevel_ops ops = {};
  ops.getattr = FakeGetattr;

  // fuse_session_new() insists on a non-empty argv[0] (used only for its
  // own error messages); nothing else about these args matters here.
  char arg0[] = "fuse_request_channel_test";
  char *argv[] = {arg0};
  struct fuse_args args = FUSE_ARGS_INIT(1, argv);
  struct fuse_session *se =
      fuse_session_new(&args, &ops, sizeof(ops), &channel);
  ASSERT_NE(se, nullptr) << "fuse_session_new failed";

  struct fuse_custom_io io = {};
  io.read = FakeReadNeverCalled;
  io.writev = FakeWritev;
  // fuse_session_custom_io() insists on a non-negative fd, even though
  // this test's read()/writev() callbacks ignore it entirely (everything
  // goes through fuse_session_process_buf() with hand-built buffers
  // below, never through this fd); /dev/null is a harmless real fd to
  // satisfy that check. fuse_session_destroy() closes it.
  absl::StatusOr<FileDescriptor> dummy =
      syscalls::openat(AT_FDCWD, "/dev/null", O_RDWR);
  ASSERT_THAT(dummy, IsOk());
  const int dummy_fd = std::move(*dummy).Release();  // the session owns it
  ASSERT_EQ(fuse_session_custom_io(se, &io, sizeof(io), dummy_fd), 0);

  // FUSE_INIT: mandatory first request, and its reply must succeed (via
  // FakeWritev's next_writev_errno == 0 default) or the session never
  // reaches got_init, and the GETATTR below would be rejected before ever
  // reaching FakeGetattr.
  {
    std::string init_req;
    struct fuse_in_header hdr = {};
    struct fuse_init_in init_in = {};
    init_in.major = FUSE_KERNEL_VERSION;
    init_in.minor = FUSE_KERNEL_MINOR_VERSION;
    hdr.opcode = FUSE_INIT;
    hdr.unique = 1;
    hdr.nodeid = 0;
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + sizeof(init_in));
    AppendBytes(init_req, hdr);
    AppendBytes(init_req, init_in);

    struct fuse_buf buf = {};
    buf.mem = init_req.data();
    buf.size = init_req.size();
    fuse_session_process_buf(se, &buf);
  }
  ASSERT_FALSE(channel.reply_status.has_value())
      << "FUSE_INIT must not reach the .getattr op handler";

  // FUSE_GETATTR, with the next (and only) writev() call -- the GETATTR
  // reply itself -- forced to fail with EIO, simulating a broken channel.
  channel.next_writev_errno = channel.fail_with.has_value() ? 0 : EIO;
  {
    std::string getattr_req;
    struct fuse_in_header hdr = {};
    struct fuse_getattr_in getattr_in = {};
    hdr.opcode = FUSE_GETATTR;
    hdr.unique = 2;
    hdr.nodeid = 1;  // root; FakeGetattr ignores it.
    hdr.pid = channel.request_pid;
    hdr.uid = channel.request_uid;
    hdr.gid = channel.request_gid;
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + sizeof(getattr_in));
    AppendBytes(getattr_req, hdr);
    AppendBytes(getattr_req, getattr_in);

    struct fuse_buf buf = {};
    buf.mem = getattr_req.data();
    buf.size = getattr_req.size();
    fuse_session_process_buf(se, &buf);
  }

  fuse_session_destroy(se);
}

TEST(FuseRequestChannelTest, ReplyErrnoPreservesWriteFailureErrno) {
  FakeChannel channel;
  RunGetattr(channel);
  ASSERT_TRUE(channel.reply_status.has_value())
      << "FUSE_GETATTR did not reach the .getattr op handler";
  EXPECT_FALSE(channel.reply_status->ok());
  auto errno_val = GetErrnoFromStatus(*channel.reply_status);
  ASSERT_THAT(errno_val, IsOk())
      << "ReplyErrno()'s failure Status carries no errno payload: "
      << *channel.reply_status;
  EXPECT_EQ(*errno_val, EIO);
}

// Collects the log lines at ERROR and above while it lives.
class ErrorCapture : public absl::LogSink {
 public:
  ErrorCapture() { absl::AddLogSink(this); }
  ~ErrorCapture() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    if (entry.log_severity() >= absl::LogSeverity::kError) {
      lines.emplace_back(entry.text_message());
    }
  }
  std::vector<std::string> lines;
};

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

// Step 26.14d: Caller() reads the supplementary groups of the request's pid
// (libfuse's fuse_req_getgroups: /proc/<pid>/task/<pid>/status), takes the
// ids from the header, and logs nothing when it can read them.
TEST(FuseRequestChannelTest, CallerHasTheGroupsOfTheRequestsPid) {
  // The guest's root, on the main thread, whose /proc entry
  // (/proc/<pid>/task/<pid>) is the one libfuse reads.
  std::vector<gid_t> before(64);
  ASSERT_OK_AND_ASSIGN(
      int n,
      syscalls::getgroups(static_cast<int>(before.size()), before.data()));
  before.resize(static_cast<size_t>(n));
  const gid_t mine[] = {4242, 4343};
  ASSERT_THAT(syscalls::setgroups(mine), IsOk());
  FakeChannel channel;
  channel.read_caller = true;
  channel.request_pid = static_cast<uint32_t>(syscalls::getpid());
  channel.request_uid = 1000;
  channel.request_gid = 2000;
  WarningCapture capture;
  RunGetattr(channel);
  ASSERT_THAT(syscalls::setgroups(before), IsOk());
  ASSERT_TRUE(channel.caller.has_value());
  ASSERT_THAT(*channel.caller, IsOk());
  EXPECT_EQ((*channel.caller)->uid, 1000u);
  EXPECT_EQ((*channel.caller)->gid, 2000u);
  EXPECT_THAT((*channel.caller)->groups,
              testing::UnorderedElementsAre(4242u, 4343u));
  EXPECT_THAT(capture.lines, testing::IsEmpty())
      << absl::StrJoin(capture.lines, "\n");
}

// A request with pid 0 -- the kernel sends it for a caller outside the
// daemon's pid namespace -- has no /proc entry to read the groups from:
// the caller is served with its ids and no groups, and the administrator is
// told (logged once per 60 s for the process, so no other test in this
// binary may send a pid-0 request that reads its caller).
TEST(FuseRequestChannelTest, CallerOfPidZeroHasNoGroupsAndIsWarnedAbout) {
  FakeChannel channel;
  channel.read_caller = true;
  channel.request_pid = 0;
  channel.request_uid = 1000;
  channel.request_gid = 2000;
  WarningCapture capture;
  RunGetattr(channel);
  ASSERT_TRUE(channel.caller.has_value());
  ASSERT_THAT(*channel.caller, IsOk());
  EXPECT_EQ((*channel.caller)->uid, 1000u);
  EXPECT_EQ((*channel.caller)->gid, 2000u);
  EXPECT_THAT((*channel.caller)->groups, testing::IsEmpty());
  EXPECT_THAT(capture.lines, testing::ElementsAre(testing::HasSubstr(
                                 "pid 0: supplementary groups unreadable")))
      << absl::StrJoin(capture.lines, "\n");
}

// docs/style.md 1.7: the handler that replies is the one place that logs a
// request's failure, and an errno the backing filesystem answered with
// (ENOENT from openat) is the answer, not a failure of dcfs.
TEST(FuseRequestChannelTest, ABackingErrnoIsNotLoggedAtError) {
  FakeChannel channel;
  channel.fail_with = ErrnoToStatus(ENOENT, "openat(5, \"x\")");
  ErrorCapture capture;
  RunGetattr(channel);
  EXPECT_THAT(capture.lines, testing::IsEmpty())
      << absl::StrJoin(capture.lines, "\n");
}

// A success reply that fails (the request was aborted: the channel's write
// fails) is a failure of dcfs, logged once at ERROR with what failed, and the
// used request is not replied to a second time.
TEST(FuseRequestChannelTest, AFailedSuccessReplyIsLoggedOnceAtError) {
  FakeChannel channel;
  channel.reply_attr_then_fail = true;
  ErrorCapture capture;
  RunGetattr(channel);
  EXPECT_THAT(capture.lines,
              testing::ElementsAre(testing::HasSubstr("fuse_reply_attr")))
      << absl::StrJoin(capture.lines, "\n");
}

}  // namespace
}  // namespace dcfs
