// Step 15.2: the failure paths of CaptureBacking (dcfs/backing_capture.cc),
// which no real mount makes on demand: the helper failing before it answers,
// dying without an answer, answering without the descriptor, and mount(8)
// not running or being killed. Link-time wraps of unshare, open_tree, execv
// and sendmsg (a target of its own: BUILD.bazel) forward to the real call
// unless a test armed a fault for them. The helper is a forked copy of this
// process, so the armed fault is what it sees. The "mount" it runs is
// `sh -c true`, so nothing is mounted; the staging tmpfs is real.

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/backing_capture.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_process.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

extern "C" {
int __real_unshare(int flags);
int __real_open_tree(int dirfd, const char *path, unsigned int flags);
int __real_execv(const char *path, char *const argv[]);
ssize_t __real_sendmsg(int fd, const struct msghdr *message, int flags);
}

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;
using ::testing::Not;

enum class Fault {
  kNone,
  kUnshare,       // unshare fails with EPERM
  kOpenTreeDies,  // the helper exits at open_tree, answering nothing
  kExecSignal,    // mount(8) kills itself with SIGKILL
  kExecMissing,   // mount(8) cannot be run
  kExecFails,     // mount(8) exits 5 with a message
  kNoDescriptor,  // the answer carries no SCM_RIGHTS
};
Fault g_fault = Fault::kNone;

class CaptureFaultTest : public ::testing::Test {
 protected:
  void TearDown() override { g_fault = Fault::kNone; }
  CaptureRequest Request() { return {.source = "fake-device"}; }
};

TEST_F(CaptureFaultTest, TheHelperRunsMountAndCapturesTheStagingTree) {
  g_fault = Fault::kNone;
  absl::StatusOr<CapturedTree> tree = CaptureBacking(Request());
  ASSERT_THAT(tree, IsOk());
  EXPECT_TRUE(tree->root.valid());
  EXPECT_TRUE(tree->tree.valid());
}

// procfs mounted without /proc/sys (ProcSubset=pid): say what is missing.
TEST_F(CaptureFaultTest, AMissingStagingRootNamesWhatDcfsNeeds) {
  CaptureRequest request = Request();
  request.staging_root = "/no/such/root";
  EXPECT_THAT(CaptureBacking(request).status(),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       AllOf(HasSubstr("/no/such/root"), HasSubstr("/proc"))));
}

TEST_F(CaptureFaultTest, AHelperFailureKeepsItsCodeMessageAndErrno) {
  g_fault = Fault::kUnshare;
  absl::StatusOr<CapturedTree> tree = CaptureBacking(Request());
  EXPECT_THAT(tree, StatusIs(absl::StatusCode::kPermissionDenied,
                             HasSubstr("unshare")));
  // Once, not "FAILED_PRECONDITION: PERMISSION_DENIED: ...".
  EXPECT_THAT(tree.status().message(), Not(HasSubstr("PERMISSION_DENIED")));
  EXPECT_EQ(StatusToErrno(tree.status()), EPERM);
}

TEST_F(CaptureFaultTest, AHelperThatDiesWithoutAnAnswerIsReported) {
  g_fault = Fault::kOpenTreeDies;
  EXPECT_THAT(
      CaptureBacking(Request()).status(),
      StatusIs(absl::StatusCode::kInternal, HasSubstr("without an answer")));
}

TEST_F(CaptureFaultTest, AnAnswerWithoutTheDescriptorIsRefused) {
  g_fault = Fault::kNoDescriptor;
  EXPECT_THAT(
      CaptureBacking(Request()).status(),
      StatusIs(absl::StatusCode::kInternal, HasSubstr("without a descriptor")));
}

TEST_F(CaptureFaultTest, MountKilledBySignalIsAFailedMountWithStatus32) {
  g_fault = Fault::kExecSignal;
  absl::Status status = CaptureBacking(Request()).status();
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kFailedPrecondition,
                               HasSubstr("killed by signal 9")));
  EXPECT_EQ(ExitStatusFor(status), 32);
}

TEST_F(CaptureFaultTest, MountThatCannotBeRunSaysSo) {
  g_fault = Fault::kExecMissing;
  absl::Status status = CaptureBacking(Request()).status();
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kFailedPrecondition,
                               HasSubstr("could not be run")));
  EXPECT_EQ(ExitStatusFor(status), 127);
}

TEST_F(CaptureFaultTest, MountsOwnStatusAndTextPassThrough) {
  g_fault = Fault::kExecFails;
  absl::Status status = CaptureBacking(Request()).status();
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kFailedPrecondition,
                               HasSubstr("mount: it went wrong")));
  EXPECT_EQ(ExitStatusFor(status), 5);
}

}  // namespace
}  // namespace dcfs

extern "C" {

int __wrap_unshare(int flags) {
  if (dcfs::g_fault == dcfs::Fault::kUnshare) {
    errno = EPERM;
    return -1;
  }
  return __real_unshare(flags);
}

int __wrap_open_tree(int dirfd, const char *path, unsigned int flags) {
  if (dcfs::g_fault == dcfs::Fault::kOpenTreeDies) {
    dcfs::syscalls::_exit(3);
  }
  return __real_open_tree(dirfd, path, flags);
}

int __wrap_execv(const char *path, char *const argv[]) {
  switch (dcfs::g_fault) {
    case dcfs::Fault::kExecMissing:
      errno = ENOENT;
      return -1;
    case dcfs::Fault::kExecSignal: {
      char *const sh[] = {const_cast<char *>("sh"), const_cast<char *>("-c"),
                          const_cast<char *>("kill -KILL $$"), nullptr};
      return __real_execv("/bin/sh", sh);
    }
    case dcfs::Fault::kExecFails: {
      char *const sh[] = {
          const_cast<char *>("sh"), const_cast<char *>("-c"),
          const_cast<char *>("echo 'mount: it went wrong' >&2; exit 5"),
          nullptr};
      return __real_execv("/bin/sh", sh);
    }
    default: {
      char *const sh[] = {const_cast<char *>("sh"), const_cast<char *>("-c"),
                          const_cast<char *>("true"), nullptr};
      (void)path;
      (void)argv;
      return __real_execv("/bin/sh", sh);
    }
  }
}

ssize_t __wrap_sendmsg(int fd, const struct msghdr *message, int flags) {
  if (dcfs::g_fault == dcfs::Fault::kNoDescriptor) {
    struct msghdr bare = *message;
    bare.msg_control = nullptr;
    bare.msg_controllen = 0;
    return __real_sendmsg(fd, &bare, flags);
  }
  return __real_sendmsg(fd, message, flags);
}

}  // extern "C"
