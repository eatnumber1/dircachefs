// SPDX-License-Identifier: MIT
// Tests of the runner that starts the real git and Bazel processes, over
// programs the guest has (busybox's).
#include "tools/process_runner.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/test_first.h"

namespace dcfs::test_first {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

absl::StatusOr<CommandResult> RunSh(const std::string &script,
                                    const std::string &path = "/bin") {
  ProcessRunner runner(path);
  const std::vector<std::string> argv = {"sh", "-c", script};
  return runner.Run(argv);
}

TEST(ProcessRunnerTest, FindsTheProgramOnThePath) {
  ProcessRunner runner("/nonexistent:/bin");
  const std::vector<std::string> argv = {"echo", "hi"};
  ASSERT_OK_AND_ASSIGN(CommandResult result, runner.Run(argv));
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "hi\n");
}

TEST(ProcessRunnerTest, ARelativeNameWithASlashIsNotSearched) {
  ProcessRunner runner("/nonexistent");
  const std::vector<std::string> argv = {"/bin/echo", "direct"};
  ASSERT_OK_AND_ASSIGN(CommandResult result, runner.Run(argv));
  EXPECT_EQ(result.output, "direct\n");
}

TEST(ProcessRunnerTest, ExitCodeAndBothStreams) {
  ASSERT_OK_AND_ASSIGN(CommandResult result,
                       RunSh("echo out; echo err >&2; exit 3"));
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_THAT(result.output, HasSubstr("out\n"));
  EXPECT_THAT(result.output, HasSubstr("err\n"));
}

TEST(ProcessRunnerTest, ADeathBySignalIsExitCode128PlusTheSignal) {
  ASSERT_OK_AND_ASSIGN(CommandResult result, RunSh("kill -9 $$"));
  EXPECT_EQ(result.exit_code, 128 + 9);
}

TEST(ProcessRunnerTest, OutputLargerThanAPipeBufferIsAllRead) {
  ASSERT_OK_AND_ASSIGN(CommandResult result,
                       RunSh("head -c 300000 /dev/zero | tr '\\000' x"));
  EXPECT_EQ(result.output.size(), 300000u);
}

TEST(ProcessRunnerTest, AProgramNotOnThePathIsNotFound) {
  ProcessRunner runner("/bin");
  const std::vector<std::string> argv = {"no-such-program-here"};
  EXPECT_THAT(runner.Run(argv), StatusIs(absl::StatusCode::kNotFound,
                                         HasSubstr("no-such-program-here")));
}

TEST(ProcessRunnerTest, AProgramThatCannotBeExecutedExits127) {
  ProcessRunner runner("/bin");
  const std::vector<std::string> argv = {"/etc/passwd"};
  ASSERT_OK_AND_ASSIGN(CommandResult result, runner.Run(argv));
  EXPECT_EQ(result.exit_code, 127);
}

}  // namespace
}  // namespace dcfs::test_first
