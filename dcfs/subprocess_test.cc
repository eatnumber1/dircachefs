// SPDX-License-Identifier: MIT
// Tests of the helper that runs a program and reads its output, over the
// programs the guest has (busybox's).
#include "dcfs/subprocess.h"

#include <fcntl.h>
#include <sys/socket.h>
// WIFEXITED, WEXITSTATUS, WIFSIGNALED and WTERMSIG: misc-include-cleaner
// attributes the POSIX macros to this C header (docs/style.md, "A POSIX
// macro").
// NOLINTNEXTLINE(modernize-deprecated-headers): the macros are defined here.
#include <stdlib.h>

#include <string>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::testing::Not;

absl::StatusOr<ProgramRun> RunSh(const std::string &script, Capture capture) {
  const std::vector<std::string> argv = {"sh", "-c", script};
  return RunProgram("/bin/sh", argv, capture);
}

TEST(RunProgramTest, TheWaitStatusIsTheChildsOwn) {
  ASSERT_OK_AND_ASSIGN(ProgramRun run, RunSh("exit 3", Capture::kNothing));
  ASSERT_TRUE(WIFEXITED(run.wait_status));
  EXPECT_EQ(WEXITSTATUS(run.wait_status), 3);
}

TEST(RunProgramTest, ADeathBySignalIsInTheWaitStatus) {
  ASSERT_OK_AND_ASSIGN(ProgramRun run, RunSh("kill -9 $$", Capture::kNothing));
  ASSERT_TRUE(WIFSIGNALED(run.wait_status));
  EXPECT_EQ(WTERMSIG(run.wait_status), 9);
}

TEST(RunProgramTest, AllOfArgvReachesTheProgram) {
  const std::vector<std::string> argv = {"sh", "-c", "echo $0 $1", "first",
                                         "second"};
  ASSERT_OK_AND_ASSIGN(ProgramRun run,
                       RunProgram("/bin/sh", argv, Capture::kStdout));
  EXPECT_EQ(run.output, "first second\n");
}

TEST(RunProgramTest, NothingCapturedIsNoOutput) {
  ASSERT_OK_AND_ASSIGN(ProgramRun run, RunSh("echo out", Capture::kNothing));
  EXPECT_EQ(run.output, "");
}

TEST(RunProgramTest, StdoutCaptureLeavesStderrTheCallers) {
  ASSERT_OK_AND_ASSIGN(ProgramRun run,
                       RunSh("echo out; echo err >&2", Capture::kStdout));
  EXPECT_EQ(run.output, "out\n");
}

TEST(RunProgramTest, OutputCaptureHasBothStreams) {
  ASSERT_OK_AND_ASSIGN(ProgramRun run,
                       RunSh("echo out; echo err >&2", Capture::kOutput));
  EXPECT_EQ(run.output, "out\nerr\n");
}

TEST(RunProgramTest, OutputLargerThanAPipeBufferIsAllRead) {
  ASSERT_OK_AND_ASSIGN(
      ProgramRun run,
      RunSh("head -c 300000 /dev/zero | tr '\\000' x", Capture::kStdout));
  EXPECT_EQ(run.output.size(), 300000u);
}

TEST(RunProgramTest, AProgramThatCannotBeExecutedExits127) {
  const std::vector<std::string> argv = {"passwd"};
  ASSERT_OK_AND_ASSIGN(ProgramRun run,
                       RunProgram("/etc/passwd", argv, Capture::kNothing));
  ASSERT_TRUE(WIFEXITED(run.wait_status));
  EXPECT_EQ(WEXITSTATUS(run.wait_status), 127);
}

TEST(ReadAllTest, ReadsEverythingUpToTheEnd) {
  ASSERT_OK_AND_ASSIGN(auto pair,
                       syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  const std::string text(10000, 'z');
  ASSERT_THAT(syscalls::write(*pair.first, text.data(), text.size()), IsOk());
  pair.first.Close().IgnoreError();
  EXPECT_THAT(ReadAll(*pair.second), ::absl_testing::IsOkAndHolds(text));
}

TEST(ReadAllTest, AFailedReadIsReturnedNotTakenForTheEnd) {
  // Reading a directory fails with EISDIR.
  ASSERT_OK_AND_ASSIGN(FileDescriptor dir,
                       syscalls::openat(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY));
  EXPECT_THAT(ReadAll(*dir), Not(IsOk()));
}

}  // namespace
}  // namespace dcfs
