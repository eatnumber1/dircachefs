// SPDX-License-Identifier: MIT
// Unit tests of the test-first check (tools/test_first.h) over a scripted
// runner: no git and no Bazel is run.
#include "tools/test_first.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "dcfs/status.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs::test_first {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;
using ::testing::StartsWith;

constexpr char kFirstSha[] = "1111111111111111111111111111111111111111";
constexpr char kLastSha[] = "2222222222222222222222222222222222222222";
constexpr char kParentSha[] = "0000000000000000000000000000000000000000";
constexpr char kQuery[] =
    "kind(\"(cc|py|sh)_test\", rdeps(//..., set(//dcfs:foo_test.cc "
    "//dcfs:testdata/case.txt)))";
// The same for the test sources among them (not the test data).
constexpr char kSourceQuery[] =
    "kind(\"(cc|py|sh)_test\", rdeps(//..., set(//dcfs:foo_test.cc)))";

// The query of a package's rules as BUILD text, which the check reads at the
// parent and at the first commit.
std::string BuildQueryLine(std::string_view package) {
  return absl::StrCat(
      "bazel query --keep_going --output=build --color=no --curses=no //",
      package, ":all");
}

std::string QueryLine(std::string_view expression) {
  return absl::StrCat(
      "bazel query --keep_going --output=label --color=no --curses=no ",
      expression);
}

CommandResult Exit(int exit_code, std::string output = "") {
  return {.exit_code = exit_code, .output = std::move(output)};
}

// A runner that answers the way git and Bazel would for one scripted step
// branch: `answers` maps a command line to its result, and `git checkout`
// moves `head`. `bazel test` answers by the commit checked out.
class ScriptedRunner : public CommandRunner {
 public:
  absl::StatusOr<CommandResult> Run(
      std::span<const std::string> argv) override {
    const std::string line = absl::StrJoin(argv, " ");
    calls.push_back(line);
    if (line.starts_with("git checkout -q ")) {
      if (failing_checkouts.contains(line)) return Exit(1, "cannot checkout");
      head = argv.back();
      return CommandResult{};
    }
    if (auto at = query_at_head.find(absl::StrCat(head, "|", line));
        at != query_at_head.end()) {
      return at->second;
    }
    if (line.starts_with("bazel test ")) {
      auto found = test_results.find(head);
      if (found == test_results.end()) {
        return InternalErrorBuilder() << "no scripted test result at " << head;
      }
      return found->second;
    }
    auto found = answers.find(line);
    if (found == answers.end()) {
      return InternalErrorBuilder() << "unscripted command: " << line;
    }
    return found->second;
  }

  // The checkouts, in order.
  std::vector<std::string> Checkouts() const {
    std::vector<std::string> out;
    for (const std::string &call : calls) {
      if (call.starts_with("git checkout ")) out.push_back(call);
    }
    return out;
  }

  absl::flat_hash_map<std::string, CommandResult> answers;
  absl::flat_hash_map<std::string, CommandResult> test_results;  // by commit
  // The answer to a command while a commit is checked out ("sha|command"),
  // before `answers`.
  absl::flat_hash_map<std::string, CommandResult> query_at_head;
  absl::flat_hash_set<std::string>
      failing_checkouts;  // the whole `git checkout` line
  std::vector<std::string> calls;
  std::string head = "step-x";
};

// The answers for a clean tree on branch step-x, a slice whose first commit
// adds dcfs/foo_test.cc and dcfs/testdata/case.txt, and the test target
// //dcfs:foo_test; the tests fail at the first commit and pass at the last.
ScriptedRunner HappyRunner() {
  ScriptedRunner r;
  r.answers["git status --porcelain"] = Exit(0, "");
  r.answers["git rev-parse --abbrev-ref HEAD"] = Exit(0, "step-x\n");
  r.answers["git rev-parse --verify FIRST^{commit}"] = Exit(0, kFirstSha);
  r.answers["git rev-parse --verify LAST^{commit}"] = Exit(0, kLastSha);
  r.answers[std::string("git merge-base --is-ancestor ") + kFirstSha + " " +
            kLastSha] = Exit(0, "");
  r.answers[std::string("git diff-tree --no-commit-id --name-status -r "
                        "--no-renames --root ") +
            kFirstSha] = Exit(0,
                              "A\tdcfs/foo_test.cc\n"
                              "A\tdcfs/testdata/case.txt\n"
                              "M\tdcfs/BUILD.bazel\n");
  r.answers[std::string("git cat-file -e ") + kFirstSha +
            ":dcfs/testdata/BUILD.bazel"] = Exit(128, "");
  r.answers[std::string("git cat-file -e ") + kFirstSha + ":dcfs/BUILD.bazel"] =
      Exit(0, "");
  r.answers[absl::StrCat("git rev-list --parents -n 1 ", kFirstSha)] =
      Exit(0, absl::StrCat(kFirstSha, " ", kParentSha, "\n"));
  r.answers[absl::StrCat("git cat-file -e ", kParentSha, ":dcfs/BUILD.bazel")] =
      Exit(0, "");
  r.answers[BuildQueryLine("dcfs")] = Exit(0, "");
  r.answers[QueryLine(kQuery)] = Exit(0, "//dcfs:foo_test\n");
  r.answers[QueryLine(kSourceQuery)] = Exit(0, "//dcfs:foo_test\n");
  r.test_results[kFirstSha] =
      Exit(3,
           "//dcfs:foo_test                                      FAILED in 9s\n"
           "[  FAILED  ] FooTest.Works (0 ms)\n"
           "[  FAILED  ] 1 test, listed below:\n"
           "[  FAILED  ] FooTest.Works\n");
  r.test_results[kLastSha] = Exit(
      0, "//dcfs:foo_test                                      PASSED in 9s\n");
  return r;
}

TEST(IsTestFileTest, RecognisesTests) {
  EXPECT_TRUE(IsTestFile("dcfs/foo_test.cc"));
  EXPECT_TRUE(IsTestFile("tools/foo_test.py"));
  EXPECT_TRUE(IsTestFile("test/qemu/guest/xattr.sh"));
  EXPECT_TRUE(IsTestFile("test/qemu/foo_test.sh"));
  EXPECT_TRUE(IsTestFile("tools/testdata/a/b.txt"));
  EXPECT_TRUE(IsTestFile("dcfs/testonly/files.cc"));
  EXPECT_TRUE(IsTestFile("formal/known_bugs/atime.cfg"));
  EXPECT_TRUE(IsTestFile("formal/limitations/x.cfg"));
  EXPECT_TRUE(IsTestFile("dcfs/BUILD.bazel"));
}

TEST(IsTestFileTest, RefusesCode) {
  EXPECT_FALSE(IsTestFile("dcfs/foo.cc"));
  EXPECT_FALSE(IsTestFile("dcfs/foo.h"));
  EXPECT_FALSE(IsTestFile("tools/test_first.cc"));
  EXPECT_FALSE(IsTestFile("tools/repo_shape.py"));
  EXPECT_FALSE(IsTestFile("docs/style.md"));
  EXPECT_FALSE(IsTestFile("formal/dcfs.tla"));
  EXPECT_FALSE(IsTestFile("dcfs/contest_runner.cc"));
}

TEST(ParseFailuresTest, GoogleTestNamesOnceEach) {
  EXPECT_THAT(
      ParseFailures("[  FAILED  ] FooTest.Works (0 ms)\n"
                    "[  FAILED  ] 2 tests, listed below:\n"
                    "[  FAILED  ] FooTest.Works\n"
                    "[  FAILED  ] BarTest.Breaks, where GetParam() = 3\n"),
      ElementsAre("FooTest.Works", "BarTest.Breaks"));
}

TEST(ParseFailuresTest, UnittestAndBazelSummary) {
  EXPECT_THAT(ParseFailures("FAIL: test_one (mod.Case.test_one)\n"
                            "ERROR: test_two (mod.Case.test_two)\n"
                            "//tools:thing_test            FAILED in 0.3s\n"
                            "//tools:other_test       TIMEOUT in 60.0s\n"
                            "//tools:good_test        PASSED in 1.0s\n"),
              ElementsAre("mod.Case.test_one", "mod.Case.test_two",
                          "//tools:thing_test", "//tools:other_test"));
}

TEST(ParseFailuresTest, NothingFailedIsEmpty) {
  EXPECT_THAT(ParseFailures("//tools:good_test PASSED in 1.0s\n"), IsEmpty());
}

TEST(CheckTest, AcceptsTestsThatFailFirstAndPassLast) {
  ScriptedRunner runner = HappyRunner();
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_TRUE(report.Accepted());
  EXPECT_THAT(report.targets, ElementsAre("//dcfs:foo_test"));
  EXPECT_EQ(report.at_first.outcome, Outcome::kTestsFailed);
  EXPECT_THAT(report.at_first.failing,
              ElementsAre("//dcfs:foo_test", "FooTest.Works"));
  EXPECT_EQ(report.at_last.outcome, Outcome::kPassed);
  // Left as found: the starting branch is checked out again.
  EXPECT_THAT(runner.Checkouts(),
              ElementsAre(std::string("git checkout -q --detach ") + kFirstSha,
                          std::string("git checkout -q --detach ") + kParentSha,
                          std::string("git checkout -q --detach ") + kFirstSha,
                          std::string("git checkout -q --detach ") + kLastSha,
                          "git checkout -q step-x"));
}

TEST(CheckTest, RunsBazelWithTheCallersFlags) {
  ScriptedRunner runner = HappyRunner();
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST",
                                      .last = "LAST",
                                      .bazel_flags = {"--config=fast"}}));
  EXPECT_TRUE(report.Accepted());
  EXPECT_THAT(runner.calls,
              Contains("bazel test --keep_going "
                       "--test_output=errors --color=no --curses=no "
                       "--config=fast //dcfs:foo_test"));
}

TEST(CheckTest, RejectsTestsThatPassAtTheFirstCommit) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] = Exit(0, "//dcfs:foo_test PASSED in 9s\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_EQ(report.at_first.outcome, Outcome::kPassed);
  EXPECT_THAT(report.reasons,
              ElementsAre(HasSubstr("passed at the first commit")));
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q step-x");
}

TEST(CheckTest, RejectsTestsThatStillFailAtTheLastCommit) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kLastSha] =
      Exit(3, "[  FAILED  ] FooTest.Works (0 ms)\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_EQ(report.at_last.outcome, Outcome::kTestsFailed);
  EXPECT_THAT(report.at_last.failing, ElementsAre("FooTest.Works"));
  EXPECT_THAT(report.reasons, ElementsAre(HasSubstr("fail at the last")));
}

TEST(CheckTest, ABuildFailureAtTheFirstCommitCountsAsFailingAndIsLabelled) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] =
      Exit(1, "ERROR: dcfs/foo_test.cc: no member 'Bar'\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_TRUE(report.Accepted());
  EXPECT_EQ(report.at_first.outcome, Outcome::kBuildFailed);
  EXPECT_THAT(FormatReport(report), HasSubstr("build failed"));
}

TEST(CheckTest, RejectsAFirstCommitThatTouchesCode) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[std::string("git diff-tree --no-commit-id --name-status -r "
                             "--no-renames --root ") +
                 kFirstSha] = Exit(0,
                                   "A\tdcfs/foo_test.cc\n"
                                   "M\tdcfs/foo.cc\n"
                                   "D\tdocs/old.md\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons,
              ElementsAre(HasSubstr("dcfs/foo.cc"), HasSubstr("docs/old.md")));
  // Nothing was built or checked out.
  EXPECT_THAT(runner.Checkouts(), IsEmpty());
  EXPECT_THAT(runner.calls, Not(Contains(StartsWith("bazel"))));
}

TEST(CheckTest, RejectsAFirstCommitWithNoFiles) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[std::string("git diff-tree --no-commit-id --name-status -r "
                             "--no-renames --root ") +
                 kFirstSha] = Exit(0, "");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons, ElementsAre(HasSubstr("touches no file")));
}

TEST(CheckTest, RejectsTestFilesNoTestTargetOwns) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[QueryLine(kQuery)] = Exit(0, "");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons, ElementsAre(HasSubstr("no test target")));
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q step-x");
}

TEST(CheckTest, TestFilesAtTheRepositoryRootAreInPackageRoot) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[std::string("git diff-tree --no-commit-id --name-status -r "
                             "--no-renames --root ") +
                 kFirstSha] = Exit(0, "A\tx_test.py\n");
  runner.answers[std::string("git cat-file -e ") + kFirstSha + ":BUILD.bazel"] =
      Exit(0, "");
  runner.answers[QueryLine(
      "kind(\"(cc|py|sh)_test\", rdeps(//..., set(//:x_test.py)))")] =
      Exit(0, "//:x_test\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_THAT(report.targets, ElementsAre("//:x_test"));
}

TEST(CheckTest, RefusesADirtyTreeBeforeTouchingGit) {
  ScriptedRunner runner = HappyRunner();
  runner.answers["git status --porcelain"] = Exit(0, " M dcfs/foo.cc\n");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("uncommitted")));
  EXPECT_THAT(runner.Checkouts(), IsEmpty());
}

TEST(CheckTest, RefusesAFirstCommitThatIsNotAnAncestorOfTheLast) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[std::string("git merge-base --is-ancestor ") + kFirstSha +
                 " " + kLastSha] = Exit(1, "");
  EXPECT_THAT(
      Check(runner, {.first = "FIRST", .last = "LAST"}),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("ancestor")));
  EXPECT_THAT(runner.Checkouts(), IsEmpty());
}

TEST(CheckTest, RestoresTheStartingCommitWhenDetached) {
  ScriptedRunner runner = HappyRunner();
  runner.head = "3333";
  runner.answers["git rev-parse --abbrev-ref HEAD"] = Exit(0, "HEAD\n");
  runner.answers["git rev-parse HEAD"] = Exit(0, "3333\n");
  ASSERT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}), IsOk());
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q 3333");
}

TEST(CheckTest, RestoresTheStartingBranchWhenBazelItselfBreaks) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] = Exit(2, "FATAL: bazel crashed\n");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("exit 2")));
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q step-x");
}

std::string DiffTree() {
  return absl::StrCat(
      "git diff-tree --no-commit-id --name-status -r --no-renames --root ",
      kFirstSha);
}

TEST(CheckTest, AQueryThatCannotLoadAPackageAtTheFirstCommitIsABuildFailure) {
  // At the first commit the test's rule does not load (it needs a library the
  // code commit adds): the query is partial, and the target is found at the
  // last commit.
  ScriptedRunner runner = HappyRunner();
  runner.query_at_head[absl::StrCat(kFirstSha, "|", QueryLine(kQuery))] =
      Exit(3, "ERROR: dcfs/BUILD.bazel:9:1: no such target '//dcfs:foo'\n");
  runner.query_at_head[absl::StrCat(kFirstSha, "|", QueryLine(kSourceQuery))] =
      Exit(3, "ERROR: dcfs/BUILD.bazel:9:1: no such target '//dcfs:foo'\n");
  runner.test_results[kFirstSha] =
      Exit(1, "ERROR: dcfs/foo_test.cc:3:1: no such target '//dcfs:foo'\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_TRUE(report.Accepted());
  EXPECT_THAT(report.targets, ElementsAre("//dcfs:foo_test"));
  EXPECT_EQ(report.at_first.outcome, Outcome::kBuildFailed);
}

TEST(CheckTest, ATargetThatPassesWhileAnotherFailsIsReportedNotRejected) {
  // //dcfs:data_test only reads the changed test data; its own sources did not
  // change, so it may pass at the first commit.
  ScriptedRunner runner = HappyRunner();
  runner.answers[QueryLine(kQuery)] =
      Exit(0, "//dcfs:foo_test\n//dcfs:data_test\n");
  runner.test_results[kFirstSha] =
      Exit(3,
           "//dcfs:data_test                  PASSED in 1s\n"
           "//dcfs:foo_test                   FAILED in 1s\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_TRUE(report.Accepted());
  EXPECT_THAT(report.at_first.passed, ElementsAre("//dcfs:data_test"));
  EXPECT_THAT(FormatReport(report),
              HasSubstr("passed at first: //dcfs:data_test"));
}

TEST(CheckTest, RejectsATargetWhoseOwnSourceChangedAndStillPasses) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[QueryLine(kQuery)] =
      Exit(0, "//dcfs:foo_test\n//dcfs:data_test\n");
  runner.test_results[kFirstSha] =
      Exit(3,
           "//dcfs:foo_test                   PASSED in 1s\n"
           "//dcfs:data_test                  FAILED in 1s\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons,
              ElementsAre(HasSubstr("//dcfs:foo_test passed at the first "
                                    "commit")));
}

TEST(CheckTest, ABuildFailureNamingNeitherATestFileNorATargetIsRefused) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] = Exit(1, "ERROR: unrelated/BUILD:3: boom\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons,
              ElementsAre(HasSubstr("name no test file or target")));
}

TEST(CheckTest, TheReportQuotesTheBuildErrorsForTheReviewerToConfirm) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] =
      Exit(1,
           "ERROR: dcfs/foo_test.cc:3:1: no member named 'Bar'\n"
           "ERROR: dcfs/foo_test.cc:4:1: no member named 'Baz'\n"
           "ERROR: a\nERROR: b\nERROR: c\nERROR: d\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_THAT(report.at_first.errors, SizeIs(5));
  const std::string text = FormatReport(report);
  EXPECT_THAT(text, HasSubstr("error: ERROR: dcfs/foo_test.cc:3:1"));
  EXPECT_THAT(text, HasSubstr("confirm the error names a symbol or target "
                              "the code commit adds"));
}

TEST(ParsePassedTest, ReadsPassedAndCachedTargets) {
  EXPECT_THAT(ParsePassed("//a:t   PASSED in 1s\n"
                          "//b:u   FAILED in 2s\n"
                          "//c:v   (cached) PASSED in 0.1s\n"),
              ElementsAre("//a:t", "//c:v"));
}

TEST(CheckErrorsTest, GitStatusFailing) {
  ScriptedRunner runner = HappyRunner();
  runner.answers["git status --porcelain"] = Exit(128, "not a repository");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("exit 128")));
}

TEST(CheckErrorsTest, UnknownRevision) {
  ScriptedRunner runner = HappyRunner();
  runner.answers["git rev-parse --verify LAST^{commit}"] = Exit(128, "bad");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("exit 128")));
  EXPECT_THAT(runner.Checkouts(), IsEmpty());
}

TEST(CheckErrorsTest, UnreadableDiffTree) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[DiffTree()] = Exit(0, "garbage\n");
  EXPECT_THAT(
      Check(runner, {.first = "FIRST", .last = "LAST"}),
      StatusIs(absl::StatusCode::kInternal, HasSubstr("unreadable line")));
}

TEST(CheckErrorsTest, TestFileInNoBazelPackage) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[absl::StrCat("git cat-file -e ", kFirstSha,
                              ":dcfs/BUILD.bazel")] = Exit(128, "");
  runner.answers[absl::StrCat("git cat-file -e ", kFirstSha, ":BUILD.bazel")] =
      Exit(128, "");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("no Bazel package")));
}

TEST(CheckErrorsTest, QueryThatFailsOutright) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[QueryLine(kQuery)] = Exit(7, "server died");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("exit 7")));
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q step-x");
}

TEST(CheckErrorsTest, CheckoutOfTheFirstCommitFailsAndTheBranchIsRestored) {
  ScriptedRunner runner = HappyRunner();
  runner.failing_checkouts.insert(
      absl::StrCat("git checkout -q --detach ", kFirstSha));
  EXPECT_THAT(
      Check(runner, {.first = "FIRST", .last = "LAST"}),
      StatusIs(absl::StatusCode::kInternal, HasSubstr("cannot checkout")));
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q step-x");
}

TEST(CheckErrorsTest, RestoringTheBranchFailsAfterASuccessfulCheck) {
  ScriptedRunner runner = HappyRunner();
  runner.failing_checkouts.insert("git checkout -q step-x");
  EXPECT_THAT(
      Check(runner, {.first = "FIRST", .last = "LAST"}),
      StatusIs(absl::StatusCode::kInternal, HasSubstr("cannot checkout")));
}

TEST(CheckErrorsTest, RestoringFailsAfterBazelBroke) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] = Exit(2, "FATAL: bazel crashed\n");
  runner.failing_checkouts.insert("git checkout -q step-x");
  EXPECT_THAT(Check(runner, {.first = "FIRST", .last = "LAST"}),
              StatusIs(absl::StatusCode::kInternal,
                       AllOf(HasSubstr("exit 2"),
                             HasSubstr("restoring step-x failed"))));
}

// One rule as `bazel query --output=build` prints it.
std::string BuildRule(std::string_view kind, std::string_view name,
                      std::string_view extra = "", int line = 10) {
  return absl::StrCat("# /w/dcfs/BUILD.bazel:", line, ":1\n", kind, "(\n",
                      "  name = \"", name, "\",\n", extra, ")\n", "# Rule ",
                      name, " instantiated at (most recent call):\n",
                      "#   /w/dcfs/BUILD.bazel:", line, ":1 in <toplevel>\n");
}

TEST(BuildRuleViolationsTest, NewTestsAndTheirTestonlyPartsAreAllowed) {
  const std::string after = absl::StrCat(
      BuildRule("sh_test", "foo_test"),
      BuildRule("cc_binary", "foo_test_bin", "  testonly = True,\n"),
      "# /w/dcfs/BUILD.bazel:30:1\ngenrule(\n  name = \"foo_test_cpio\",\n"
      "  generator_name = \"foo_test\",\n)\n");
  EXPECT_THAT(BuildRuleViolations("", after), IsEmpty());
}

TEST(BuildRuleViolationsTest, ANewLibraryThatIsNotTestonlyIsRefused) {
  EXPECT_THAT(BuildRuleViolations("", BuildRule("cc_library", "helper")),
              ElementsAre(HasSubstr("helper")));
}

TEST(BuildRuleViolationsTest, ANewTestonlyLibraryIsAllowed) {
  EXPECT_THAT(BuildRuleViolations(
                  "", BuildRule("cc_library", "helper", "  testonly = 1,\n")),
              IsEmpty());
}

TEST(BuildRuleViolationsTest, ChangingAProductionRuleIsRefused) {
  EXPECT_THAT(BuildRuleViolations(
                  BuildRule("cc_library", "lib", "  deps = [],\n"),
                  BuildRule("cc_library", "lib", "  deps = [\"x\"],\n")),
              ElementsAre(HasSubstr("lib")));
}

TEST(BuildRuleViolationsTest, ChangingVisibilityIsRefusedEvenOnATestRule) {
  EXPECT_THAT(
      BuildRuleViolations(
          BuildRule("sh_test", "t", "  visibility = [\"//a:__pkg__\"],\n"),
          BuildRule("sh_test", "t", "  visibility = [\"//b:__pkg__\"],\n")),
      ElementsAre(HasSubstr("visibility")));
}

TEST(BuildRuleViolationsTest, AddingASourceToATestIsAllowed) {
  EXPECT_THAT(BuildRuleViolations(
                  BuildRule("sh_test", "t", "  srcs = [\"a\"],\n"),
                  BuildRule("sh_test", "t", "  srcs = [\"a\", \"b\"],\n")),
              IsEmpty());
}

TEST(BuildRuleViolationsTest, MovedRulesAreNotChangedRules) {
  EXPECT_THAT(BuildRuleViolations(BuildRule("cc_library", "lib", "", 10),
                                  BuildRule("cc_library", "lib", "", 25)),
              IsEmpty());
}

TEST(BuildRuleViolationsTest, RemovingAProductionRuleIsRefused) {
  EXPECT_THAT(BuildRuleViolations(BuildRule("cc_library", "lib"), ""),
              ElementsAre(HasSubstr("lib")));
}

TEST(CheckTest, RejectsABuildFileThatAddsAProductionLibrary) {
  ScriptedRunner runner = HappyRunner();
  runner.query_at_head[absl::StrCat(kFirstSha, "|", BuildQueryLine("dcfs"))] =
      Exit(0, BuildRule("cc_library", "helper"));
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons, ElementsAre(HasSubstr("//dcfs:helper")));
  EXPECT_EQ(runner.Checkouts().back(), "git checkout -q step-x");
}

TEST(CheckTest, ABuildFileNewAtTheFirstCommitHasNothingToCompareWith) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[absl::StrCat("git cat-file -e ", kParentSha,
                              ":dcfs/BUILD.bazel")] = Exit(128, "");
  runner.query_at_head[absl::StrCat(kFirstSha, "|", BuildQueryLine("dcfs"))] =
      Exit(0, BuildRule("sh_test", "foo_test"));
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_TRUE(report.Accepted());
}

// A real `bazel query --output=build` of a library with a multi-element
// visibility, a macro's test binary and its generated rule.
std::string RealBuildQuery() {
  absl::StatusOr<std::string> text =
      testonly::ReadFileToString("/test/data/testdata/build_query.txt");
  return text.ok() ? *text : std::string();
}

TEST(RealBuildQueryTest, TheFileIsThere) {
  EXPECT_THAT(RealBuildQuery(), HasSubstr("name = \"status\""));
}

TEST(RealBuildQueryTest, ComparedWithItselfNothingChanged) {
  EXPECT_THAT(BuildRuleViolations(RealBuildQuery(), RealBuildQuery()),
              IsEmpty());
}

TEST(RealBuildQueryTest, GeneratorLocationsMoveWithTheLineNumbers) {
  const std::string moved = absl::StrReplaceAll(
      RealBuildQuery(), {{"BUILD.bazel:811:13", "BUILD.bazel:900:1"},
                         {"BUILD.bazel:196:11", "BUILD.bazel:20:1"}});
  ASSERT_NE(moved, RealBuildQuery());
  EXPECT_THAT(BuildRuleViolations(RealBuildQuery(), moved), IsEmpty());
}

TEST(RealBuildQueryTest, OnlyTheProductionLibraryIsAViolation) {
  EXPECT_THAT(BuildRuleViolations("", RealBuildQuery(), "dcfs"),
              ElementsAre(HasSubstr("//dcfs:status")));
}

TEST(RealBuildQueryTest, ATestBinaryGainingOrChangingVisibilityIsRefused) {
  const std::string with_visibility = absl::StrReplaceAll(
      RealBuildQuery(), {{"testonly = True,\n  features",
                          "testonly = True,\n  visibility = [\"//a:__pkg__\", "
                          "\"//b:__pkg__\"],\n  features"}});
  const std::string changed = absl::StrReplaceAll(
      with_visibility, {{"\"//b:__pkg__\"", "\"//c:__pkg__\""}});
  EXPECT_THAT(BuildRuleViolations(with_visibility, changed),
              ElementsAre(HasSubstr("test_first_test_bin changes visibility")));
}

TEST(BuildRuleViolationsTest, AVisibilityListOverSeveralLinesIsReadToItsEnd) {
  const std::string before = BuildRule(
      "sh_test", "t",
      "  visibility = [\n    \"//a:__pkg__\",\n    \"//b:__pkg__\",\n  ],\n");
  const std::string after = BuildRule(
      "sh_test", "t",
      "  visibility = [\n    \"//a:__pkg__\",\n    \"//c:__pkg__\",\n  ],\n");
  EXPECT_THAT(BuildRuleViolations(before, after),
              ElementsAre(HasSubstr("t changes visibility")));
}

TEST(BuildRuleViolationsTest, ARuleBecomingTestonlyWasProductionBefore) {
  EXPECT_THAT(BuildRuleViolations(
                  BuildRule("cc_library", "lib"),
                  BuildRule("cc_library", "lib", "  testonly = True,\n")),
              ElementsAre(HasSubstr("lib")));
}

TEST(CheckTest, RejectsACommitThatOnlyChangesABuildFile) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[DiffTree()] = Exit(0, "M\tdcfs/BUILD.bazel\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons, ElementsAre(HasSubstr("changes no test file")));
  EXPECT_THAT(runner.Checkouts(), IsEmpty());
  EXPECT_THAT(runner.calls, Not(Contains(StartsWith("bazel"))));
}

TEST(CheckTest, ADeletedTestFileIsNotQueried) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[DiffTree()] =
      Exit(0, "A\tdcfs/foo_test.cc\nD\tdcfs/old_test.cc\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_TRUE(report.Accepted());
  for (const std::string &call : runner.calls) {
    EXPECT_THAT(call, Not(HasSubstr("old_test.cc")));
  }
}

TEST(CheckTest, ACommitThatOnlyDeletesTestsIsRejected) {
  ScriptedRunner runner = HappyRunner();
  runner.answers[DiffTree()] = Exit(0, "D\tdcfs/old_test.cc\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  EXPECT_FALSE(report.Accepted());
  EXPECT_THAT(report.reasons, ElementsAre(HasSubstr("changes no test file")));
  EXPECT_THAT(runner.Checkouts(), IsEmpty());
}

TEST(FormatReportTest, QuotesTheFailingNamesForTheMergeRecord) {
  ScriptedRunner runner = HappyRunner();
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  const std::string text = FormatReport(report);
  EXPECT_THAT(text, HasSubstr("test-first: ACCEPTED"));
  EXPECT_THAT(text, HasSubstr("targets: //dcfs:foo_test"));
  EXPECT_THAT(text, HasSubstr("at first (111111111111): tests failed"));
  EXPECT_THAT(text, HasSubstr("  failing: FooTest.Works"));
  EXPECT_THAT(text, HasSubstr("at last (222222222222): passed"));
}

TEST(FormatReportTest, NamesTheReasonsOfARejection) {
  ScriptedRunner runner = HappyRunner();
  runner.test_results[kFirstSha] = Exit(0, "//dcfs:foo_test PASSED in 9s\n");
  ASSERT_OK_AND_ASSIGN(Report report,
                       Check(runner, {.first = "FIRST", .last = "LAST"}));
  const std::string text = FormatReport(report);
  EXPECT_THAT(text, HasSubstr("test-first: REJECTED"));
  EXPECT_THAT(text, HasSubstr("passed at the first commit"));
}

}  // namespace
}  // namespace dcfs::test_first
