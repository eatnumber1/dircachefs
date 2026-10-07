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

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/uio.h>
#include <unistd.h>

#include "absl/status/status_matchers.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_request.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "fuse_kernel.h"
#include "fuse_lowlevel.h"
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
  auto *channel =
      static_cast<FakeChannel *>(fuse_req_userdata(req));
  FuseRequest fr(req);
  channel->reply_status = fr.ReplyErrno(ENOENT);
}

// Appends the raw bytes of `value` to `out`.
template <typename T>
void AppendBytes(std::string &out, const T &value) {
  out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

TEST(FuseRequestChannelTest, ReplyErrnoPreservesWriteFailureErrno) {
  FakeChannel channel;

  struct fuse_lowlevel_ops ops = {};
  ops.getattr = FakeGetattr;

  // fuse_session_new() insists on a non-empty argv[0] (used only for its
  // own error messages); nothing else about these args matters here.
  char arg0[] = "fuse_request_channel_test";
  char *argv[] = {arg0};
  struct fuse_args args = FUSE_ARGS_INIT(1, argv);
  struct fuse_session *se =
      fuse_session_new(&args, &ops, sizeof(ops), &channel);
  ASSERT_NE(se, nullptr);

  struct fuse_custom_io io = {};
  io.read = FakeReadNeverCalled;
  io.writev = FakeWritev;
  // fuse_session_custom_io() insists on a non-negative fd, even though
  // this test's read()/writev() callbacks ignore it entirely (everything
  // goes through fuse_session_process_buf() with hand-built buffers
  // below, never through this fd); /dev/null is a harmless real fd to
  // satisfy that check. fuse_session_destroy() closes it.
  ASSERT_OK_AND_ASSIGN(FileDescriptor dummy,
                       syscalls::openat(AT_FDCWD, "/dev/null", O_RDWR));
  const int dummy_fd = std::move(dummy).Release();  // the session owns it
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
  channel.next_writev_errno = EIO;
  {
    std::string getattr_req;
    struct fuse_in_header hdr = {};
    struct fuse_getattr_in getattr_in = {};
    hdr.opcode = FUSE_GETATTR;
    hdr.unique = 2;
    hdr.nodeid = 1;  // root; FakeGetattr ignores it.
    hdr.len = static_cast<uint32_t>(sizeof(hdr) + sizeof(getattr_in));
    AppendBytes(getattr_req, hdr);
    AppendBytes(getattr_req, getattr_in);

    struct fuse_buf buf = {};
    buf.mem = getattr_req.data();
    buf.size = getattr_req.size();
    fuse_session_process_buf(se, &buf);
  }

  ASSERT_TRUE(channel.reply_status.has_value())
      << "FUSE_GETATTR did not reach the .getattr op handler";
  EXPECT_FALSE(channel.reply_status->ok());
  auto errno_val = GetErrnoFromStatus(*channel.reply_status);
  ASSERT_THAT(errno_val, IsOk())
      << "ReplyErrno()'s failure Status carries no errno payload: "
      << *channel.reply_status;
  EXPECT_EQ(*errno_val, EIO);

  fuse_session_destroy(se);
}

}  // namespace
}  // namespace dcfs
