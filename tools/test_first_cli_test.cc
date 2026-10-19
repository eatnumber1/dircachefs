// SPDX-License-Identifier: MIT
// Tests of the test-first command's behaviour (tools/test_first_cli.h): exit
// codes, usage, --workspace and the text it prints, over a scripted runner.
#include "tools/test_first_cli.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/test_first.h"

namespace dcfs::test_first {
namespace {

using ::absl_testing::IsOk;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

// Answers the commands of one slice: the first commit adds
// dcfs/x_test.cc (or changes `touched`), //dcfs:x_test fails there and passes
// at the last commit.
class SliceRunner : public CommandRunner {
 public:
  absl::StatusOr<CommandResult> Run(
      std::span<const std::string> argv) override {
    calls.push_back(absl::StrJoin(argv, " "));
    if (cwd_at_first_call.empty()) {
      ASSIGN_OR_RETURN(cwd_at_first_call, syscalls::getcwd());
    }
    if (std::optional<CommandResult> git = Git(argv)) return *git;
    if (std::optional<CommandResult> bazel = Bazel(argv)) return *bazel;
    return CommandResult{};  // merge-base, cat-file
  }

  // The answer of git to `argv`, or none for a command it does not answer.
  std::optional<CommandResult> Git(std::span<const std::string> argv) {
    if (argv[0] != "git") return std::nullopt;
    const std::string &verb = argv[1];
    if (verb == "status") return CommandResult{};
    if (verb == "rev-parse" && argv[2] == "--abbrev-ref") {
      return CommandResult{.output = "main\n"};
    }
    if (verb == "rev-parse") {
      return CommandResult{.output =
                               argv[3].starts_with("A") ? "aaaa" : "bbbb"};
    }
    if (verb == "diff-tree") return CommandResult{.output = touched};
    if (verb == "checkout") {
      head = argv.back();
      return CommandResult{};
    }
    return std::nullopt;
  }

  std::optional<CommandResult> Bazel(std::span<const std::string> argv) const {
    if (argv[0] != "bazel") return std::nullopt;
    if (argv[1] == "query") return CommandResult{.output = "//dcfs:x_test\n"};
    if (argv[1] != "test") return std::nullopt;
    if (head != "aaaa") return CommandResult{};
    return CommandResult{.exit_code = 3, .output = "//dcfs:x_test FAILED\n"};
  }

  std::string touched = "A\tdcfs/x_test.cc\n";
  std::string head;
  std::vector<std::string> calls;
  std::string cwd_at_first_call;
};

ToolArgs Slice() { return {.first = "A", .last = "B"}; }

TEST(RunToolTest, AnAcceptedSliceExitsZeroAndPrintsTheReport) {
  SliceRunner runner;
  const ToolResult result = RunTool(Slice(), runner);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("test-first: ACCEPTED"));
  EXPECT_THAT(result.out, HasSubstr("  failing: //dcfs:x_test"));
  EXPECT_THAT(result.err, IsEmpty());
}

TEST(RunToolTest, ARejectedSliceExitsOne) {
  SliceRunner runner;
  runner.touched = "M\tdcfs/x.cc\n";
  const ToolResult result = RunTool(Slice(), runner);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_THAT(result.out, HasSubstr("test-first: REJECTED"));
}

TEST(RunToolTest, ACheckThatCannotBeMadeExitsTwoAndSaysWhy) {
  class Broken : public CommandRunner {
   public:
    absl::StatusOr<CommandResult> Run(std::span<const std::string>) override {
      return InternalErrorBuilder() << "no git here";
    }
  } runner;
  const ToolResult result = RunTool(Slice(), runner);
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_THAT(result.err, HasSubstr("no git here"));
  EXPECT_THAT(result.out, IsEmpty());
}

TEST(RunToolTest, MissingRevisionsAreAUsageErrorBeforeAnyCommandRuns) {
  SliceRunner runner;
  const ToolResult result = RunTool({.first = "A"}, runner);
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_THAT(result.err, HasSubstr("usage: test_first_tool --first=REV"));
  EXPECT_THAT(runner.calls, IsEmpty());
}

TEST(RunToolTest, BazelFlagsReachBazelTest) {
  SliceRunner runner;
  ToolArgs args = Slice();
  args.bazel_flags = {"--config=fast", "--keep_state_after_build"};
  ASSERT_EQ(RunTool(args, runner).exit_code, 0);
  EXPECT_THAT(runner.calls,
              testing::Contains(HasSubstr("--config=fast "
                                          "--keep_state_after_build")));
}

TEST(RunToolTest, WorkspaceIsTheDirectoryTheCommandsRunIn) {
  ASSERT_OK_AND_ASSIGN(std::string dir,
                       syscalls::mkdtemp("/tmp/workspace.XXXXXX"));
  ASSERT_OK_AND_ASSIGN(std::string real, syscalls::realpath(dir));
  SliceRunner runner;
  ToolArgs args = Slice();
  args.workspace = dir;
  EXPECT_EQ(RunTool(args, runner).exit_code, 0);
  EXPECT_EQ(runner.cwd_at_first_call, real);
  EXPECT_THAT(syscalls::chdir("/"), IsOk());
}

TEST(RunToolTest, AWorkspaceThatCannotBeEnteredExitsTwoWithoutRunning) {
  SliceRunner runner;
  ToolArgs args = Slice();
  args.workspace = "/nonexistent-workspace";
  const ToolResult result = RunTool(args, runner);
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_THAT(result.err, HasSubstr("/nonexistent-workspace"));
  EXPECT_THAT(runner.calls, IsEmpty());
}

}  // namespace
}  // namespace dcfs::test_first
