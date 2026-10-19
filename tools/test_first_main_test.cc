// SPDX-License-Identifier: MIT
// Tests of the test-first command as a program (tools/test_first_main.cc): its
// flags, exit codes and environment, with a fake git and bazel on PATH.
#include <fcntl.h>

#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/process_runner.h"
#include "tools/test_first.h"

namespace dcfs::test_first {
namespace {

using ::absl_testing::IsOk;
using ::testing::HasSubstr;
using ::testing::Not;

constexpr char kTool[] = "/test/data/test_first_tool_static";
constexpr char kFakeBin[] = "/tmp/fake-bin";
constexpr char kCalls[] = "/tmp/fake-calls";
constexpr char kWorkspace[] = "/tmp/fake-workspace";

// A git that answers the commands of a one-commit slice whose first commit
// adds a test, and a bazel whose tests fail; both log what they were run
// with, and git where.
constexpr char kFakeGit[] = R"sh(#!/bin/sh
echo "git $* (in $(pwd))" >> /tmp/fake-calls
case "$1 $2" in
  "rev-parse --abbrev-ref") echo main ;;
  "rev-parse --verify") echo aaaa ;;
  "diff-tree --no-commit-id") printf 'A\tdcfs/x_test.cc\n' ;;
esac
)sh";
constexpr char kFakeBazel[] = R"sh(#!/bin/sh
echo "bazel $*" >> /tmp/fake-calls
case "$1" in
  query) echo //dcfs:x_test ;;
  test) echo "//dcfs:x_test FAILED in 1s"; exit 3 ;;
esac
)sh";

absl::Status WriteExecutable(const std::string &path, std::string_view text) {
  ASSIGN_OR_RETURN(
      FileDescriptor fd,
      syscalls::openat(AT_FDCWD, path, O_CREAT | O_TRUNC | O_WRONLY, 0755));
  RETURN_IF_ERROR(syscalls::write(*fd, text.data(), text.size()).status());
  return syscalls::fchmod(*fd, 0755);
}

class MainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The directories may be there from an earlier test of this run.
    syscalls::mkdirat(AT_FDCWD, kFakeBin, 0755).IgnoreError();
    syscalls::mkdirat(AT_FDCWD, kWorkspace, 0755).IgnoreError();
    syscalls::unlink(kCalls).IgnoreError();
    ASSERT_THAT(WriteExecutable(absl::StrCat(kFakeBin, "/git"), kFakeGit),
                IsOk());
    ASSERT_THAT(WriteExecutable(absl::StrCat(kFakeBin, "/bazel"), kFakeBazel),
                IsOk());
  }

  // Runs the tool with the fake git and bazel first on PATH, in an
  // environment that has BUILD_WORKSPACE_DIRECTORY (as `bazel run` sets).
  static absl::StatusOr<CommandResult> Tool(std::vector<std::string> flags) {
    std::vector<std::string> argv = {
        "env", absl::StrCat("PATH=", kFakeBin, ":/bin"),
        absl::StrCat("BUILD_WORKSPACE_DIRECTORY=", kWorkspace), kTool};
    argv.insert(argv.end(), flags.begin(), flags.end());
    ProcessRunner runner("/bin");
    return runner.Run(argv);
  }

  static std::string Calls() {
    absl::StatusOr<std::string> calls = ReadFile(kCalls);
    return calls.ok() ? *calls : std::string();
  }

  static absl::StatusOr<std::string> ReadFile(const std::string &path) {
    ASSIGN_OR_RETURN(FileDescriptor fd,
                     syscalls::openat(AT_FDCWD, path, O_RDONLY));
    return ReadAll(*fd);
  }
};

TEST_F(MainTest, AnUnknownFlagIsAbseilsExitOne) {
  ASSERT_OK_AND_ASSIGN(CommandResult result, Tool({"--bogus"}));
  EXPECT_EQ(result.exit_code, 1);
}

TEST_F(MainTest, APositionalArgumentIsAUsageErrorExitTwo) {
  ASSERT_OK_AND_ASSIGN(CommandResult result,
                       Tool({"--first=A", "--last=B", "stray"}));
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_THAT(result.output, HasSubstr("usage: test_first_tool"));
}

TEST_F(MainTest, AMissingFirstIsAUsageErrorExitTwo) {
  ASSERT_OK_AND_ASSIGN(CommandResult result, Tool({"--last=B"}));
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_THAT(result.output, HasSubstr("usage: test_first_tool"));
}

TEST_F(MainTest, BazelFlagsArriveAsSeparateArguments) {
  ASSERT_OK_AND_ASSIGN(
      CommandResult result,
      Tool({"--first=A", "--last=B", "--bazel_flag=--config=fast,--jobs=2"}));
  EXPECT_EQ(result.exit_code, 3) << result.output;  // the fake's tests fail
  EXPECT_THAT(Calls(), HasSubstr("--curses=no --config=fast --jobs=2 "
                                 "//dcfs:x_test"));
}

TEST_F(MainTest, TheWorkspaceDefaultsToBuildWorkspaceDirectory) {
  ASSERT_OK_AND_ASSIGN(CommandResult result, Tool({"--first=A", "--last=B"}));
  EXPECT_EQ(result.exit_code, 3) << result.output;
  EXPECT_THAT(Calls(), HasSubstr(absl::StrCat("(in ", kWorkspace, ")")));
}

TEST_F(MainTest, TheWorkspaceFlagBeatsTheEnvironment) {
  syscalls::mkdirat(AT_FDCWD, "/tmp/other", 0755).IgnoreError();
  ASSERT_OK_AND_ASSIGN(CommandResult result, Tool({"--first=A", "--last=B",
                                                   "--workspace=/tmp/other"}));
  EXPECT_EQ(result.exit_code, 3) << result.output;
  EXPECT_THAT(Calls(), HasSubstr("(in /tmp/other)"));
  EXPECT_THAT(Calls(), Not(HasSubstr(absl::StrCat("(in ", kWorkspace, ")"))));
}

}  // namespace
}  // namespace dcfs::test_first
