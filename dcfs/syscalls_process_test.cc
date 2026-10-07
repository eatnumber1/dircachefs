#include "dcfs/syscalls_process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

TEST(SyscallsProcessTest, ForkWaitpidReportsTheChildsExitStatus) {
  absl::StatusOr<pid_t> pid = syscalls::fork();
  ASSERT_THAT(pid, IsOk());
  if (*pid == 0) syscalls::_exit(7);
  int status = 0;
  ASSERT_THAT(syscalls::waitpid(*pid, &status, 0),
              ::absl_testing::IsOkAndHolds(*pid));
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 7);
}

TEST(SyscallsProcessTest, KillStopsAChildAndWaitpidSeesTheSignal) {
  absl::StatusOr<pid_t> pid = syscalls::fork();
  ASSERT_THAT(pid, IsOk());
  if (*pid == 0) {
    while (true) syscalls::pause().IgnoreError();
  }
  ASSERT_THAT(syscalls::kill(*pid, SIGKILL), IsOk());
  int status = 0;
  ASSERT_THAT(syscalls::waitpid(*pid, &status, 0), IsOk());
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

TEST(SyscallsProcessTest, ExecvOfAMissingProgramIsEnoent) {
  char arg0[] = "no_such_program";
  char *argv[] = {arg0, nullptr};
  absl::Status status = syscalls::execv("/no/such/program", argv);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(StatusToErrno(status), ENOENT);
}

TEST(SyscallsProcessTest, Dup2ReplacesTheTargetDescriptor) {
  absl::StatusOr<FileDescriptor> a =
      syscalls::openat(AT_FDCWD, "/dev/null", O_RDONLY);
  absl::StatusOr<FileDescriptor> b =
      syscalls::openat(AT_FDCWD, "/dev/zero", O_RDONLY);
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  ASSERT_THAT(syscalls::dup2(**a, **b), IsOk());
  char c = 'x';
  // The target now reads like /dev/null: end of file at once.
  EXPECT_THAT(syscalls::read(**b, &c, 1), ::absl_testing::IsOkAndHolds(0u));
  EXPECT_FALSE(syscalls::dup2(-1, **b).ok());
}

TEST(SyscallsProcessTest, WaitpidOfNoChildIsEchild) {
  int status = 0;
  auto rc = syscalls::waitpid(-1, &status, WNOHANG);
  ASSERT_FALSE(rc.ok());
  EXPECT_EQ(StatusToErrno(rc.status()), ECHILD);
}

}  // namespace
}  // namespace dcfs
