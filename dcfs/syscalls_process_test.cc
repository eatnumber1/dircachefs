// SPDX-License-Identifier: MIT
#include "dcfs/syscalls_process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "gmock/gmock.h"
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
  const std::vector<std::string> argv = {"no_such_program"};
  absl::Status status = syscalls::execv("/no/such/program", argv);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(StatusToErrno(status), ENOENT);
}

TEST(SyscallsProcessTest, ExecvRunsTheProgramWithTheGivenArguments) {
  absl::StatusOr<pid_t> pid = syscalls::fork();
  ASSERT_THAT(pid, IsOk());
  const std::vector<std::string> argv = {"sh", "-c", "exit 5"};
  if (*pid == 0) {
    syscalls::execv("/bin/sh", argv).IgnoreError();
    syscalls::_exit(127);
  }
  int status = 0;
  ASSERT_THAT(syscalls::waitpid(*pid, &status, 0), IsOk());
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 5);
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
