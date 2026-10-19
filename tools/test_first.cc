// SPDX-License-Identifier: MIT
#include "tools/test_first.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "dcfs/status.h"

namespace dcfs::test_first {
namespace {

// What a slice's first commit may change (the one list). A file is a test file
// when it ends with a suffix, has a directory component, begins with a prefix
// or is named like one of these.
constexpr std::array<std::string_view, 3> kTestSuffixes = {
    "_test.cc", "_test.py", "_test.sh"};
// Directories that hold only tests, their data or test-only helpers.
constexpr std::array<std::string_view, 2> kTestDirectories = {"testdata",
                                                              "testonly"};
// Trees that are tests throughout: the QEMU suite and its guest scripts, and
// the TLA+ configurations of known bugs and limitations.
constexpr std::array<std::string_view, 3> kTestTrees = {
    "test/", "formal/known_bugs/", "formal/limitations/"};
// A new test needs its Bazel target, so the build files are allowed; the
// reviewer reads that diff (docs/plan/process.md, rule-reviewed-slices).
constexpr std::array<std::string_view, 2> kBuildFiles = {"BUILD.bazel",
                                                         "BUILD"};

bool IsBuildFile(std::string_view path) {
  const std::string_view base = path.substr(path.rfind('/') + 1);
  return std::ranges::find(kBuildFiles, base) != kBuildFiles.end();
}

bool HasDirectory(std::string_view path, std::string_view directory) {
  return absl::StrContains(absl::StrCat("/", path),
                           absl::StrCat("/", directory, "/"));
}

absl::StatusOr<CommandResult> Run(CommandRunner &runner,
                                  std::vector<std::string> argv) {
  return runner.Run(argv);
}

// The output of a command that has to succeed, without the final newline.
absl::StatusOr<std::string> Capture(CommandRunner &runner,
                                    std::vector<std::string> argv) {
  const std::string line = absl::StrJoin(argv, " ");
  ASSIGN_OR_RETURN(CommandResult result, Run(runner, std::move(argv)));
  if (result.exit_code != 0) {
    return InternalErrorBuilder()
           << "`" << line << "` exit " << result.exit_code << ": "
           << result.output;
  }
  return std::string(absl::StripTrailingAsciiWhitespace(result.output));
}

struct Change {
  char status;  // A, M, D, T ...
  std::string path;
};

absl::StatusOr<std::vector<Change>> ChangedFiles(CommandRunner &runner,
                                                 const std::string &commit) {
  ASSIGN_OR_RETURN(std::string text,
                   Capture(runner, {"git", "diff-tree", "--no-commit-id",
                                    "--name-status", "-r", "--no-renames",
                                    "--root", commit}));
  std::vector<Change> changes;
  for (std::string_view line : absl::StrSplit(text, '\n', absl::SkipEmpty())) {
    const size_t tab = line.find('\t');
    if (tab != 1) {
      return InternalErrorBuilder()
             << "unreadable line of git diff-tree: " << line;
    }
    changes.push_back({.status = line[0], .path = std::string(line.substr(2))});
  }
  return changes;
}

// The Bazel label of `path` in `commit`: the package is the nearest directory
// with a BUILD.bazel.
absl::StatusOr<std::string> LabelOf(CommandRunner &runner,
                                    const std::string &commit,
                                    std::string_view path) {
  std::string_view directory = path;
  while (!directory.empty()) {
    const size_t slash = directory.rfind('/');
    directory = slash == std::string_view::npos ? std::string_view()
                                                : directory.substr(0, slash);
    const std::string build = directory.empty()
                                  ? "BUILD.bazel"
                                  : absl::StrCat(directory, "/BUILD.bazel");
    ASSIGN_OR_RETURN(CommandResult found,
                     Run(runner, {"git", "cat-file", "-e",
                                  absl::StrCat(commit, ":", build)}));
    if (found.exit_code == 0) {
      const std::string_view inside =
          directory.empty() ? path : path.substr(directory.size() + 1);
      return absl::StrCat("//", directory, ":", inside);
    }
  }
  return InvalidArgumentErrorBuilder()
         << path << " is in no Bazel package at " << commit;
}

absl::StatusOr<Outcome> OutcomeOf(const CommandResult &result) {
  switch (result.exit_code) {
    case 0:
      return Outcome::kPassed;
    case 1:
      return Outcome::kBuildFailed;
    case 3:
      return Outcome::kTestsFailed;
    default:
      return InternalErrorBuilder()
             << "`bazel test` exit " << result.exit_code << ": "
             << result.output;
  }
}

absl::Status Detach(CommandRunner &runner, const std::string &commit) {
  return Capture(runner, {"git", "checkout", "-q", "--detach", commit})
      .status();
}

// `bazel test` of `targets` at the commit checked out.
absl::StatusOr<Stage> Test(CommandRunner &runner, const std::string &commit,
                           const Options &options,
                           const std::vector<std::string> &targets) {
  std::vector<std::string> argv = {"bazel",         "test",
                                   "--keep_going",  "--test_output=errors",
                                   "--color=no",    "--curses=no"};
  argv.insert(argv.end(), options.bazel_flags.begin(),
              options.bazel_flags.end());
  argv.insert(argv.end(), targets.begin(), targets.end());
  ASSIGN_OR_RETURN(CommandResult result, Run(runner, std::move(argv)));
  ASSIGN_OR_RETURN(Outcome outcome, OutcomeOf(result));
  return Stage{.commit = commit,
               .outcome = outcome,
               .failing = ParseFailures(result.output)};
}

// The test targets that own `labels`, found at the commit checked out.
absl::StatusOr<std::vector<std::string>> OwningTests(
    CommandRunner &runner, const std::vector<std::string> &labels) {
  ASSIGN_OR_RETURN(
      std::string text,
      Capture(runner,
              {"bazel", "query", "--output=label", "--color=no", "--curses=no",
               absl::StrCat("kind(\"(cc|py|sh)_test\", rdeps(//..., set(",
                            absl::StrJoin(labels, " "), ")))")}));
  std::vector<std::string> targets;
  for (std::string_view line : absl::StrSplit(text, '\n', absl::SkipEmpty())) {
    line = absl::StripAsciiWhitespace(line);
    if (line.starts_with("//") || line.starts_with("@")) {
      targets.emplace_back(line);
    }
  }
  return targets;
}

// Runs the tests at both commits of the slice, detached from the branch; the
// report is filled in as far as it got.
absl::Status RunStages(CommandRunner &runner, const Options &options,
                       const std::vector<std::string> &labels, Report &report) {
  RETURN_IF_ERROR(Detach(runner, report.first));
  ASSIGN_OR_RETURN(report.targets, OwningTests(runner, labels));
  if (report.targets.empty()) {
    report.reasons.emplace_back(
        "no test target owns the test files of the first commit");
    return absl::OkStatus();
  }
  ASSIGN_OR_RETURN(report.at_first,
                   Test(runner, report.first, options, report.targets));
  if (report.at_first.outcome == Outcome::kPassed) {
    report.reasons.emplace_back(
        "the tests passed at the first commit (they must fail there)");
  }
  RETURN_IF_ERROR(Detach(runner, report.last));
  ASSIGN_OR_RETURN(report.at_last,
                   Test(runner, report.last, options, report.targets));
  if (report.at_last.outcome != Outcome::kPassed) {
    report.reasons.emplace_back(
        "the tests still fail at the last commit (they must pass there)");
  }
  return absl::OkStatus();
}

// What the first commit touches: a reason in `report` for each file that is not
// a test file (or for no file at all), and the Bazel labels of the test files
// that a target can own.
absl::StatusOr<std::vector<std::string>> ClassifyFirstCommit(
    CommandRunner &runner, Report &report) {
  ASSIGN_OR_RETURN(std::vector<Change> changes,
                   ChangedFiles(runner, report.first));
  if (changes.empty()) {
    report.reasons.emplace_back("the first commit touches no file");
  }
  std::vector<std::string> labels;
  for (const Change &change : changes) {
    if (!IsTestFile(change.path)) {
      report.reasons.push_back(absl::StrCat("the first commit touches ",
                                            change.path,
                                            ", which is not a test file"));
      continue;
    }
    // A deleted file has no target to query, and a build file is not a test.
    if (change.status == 'D' || IsBuildFile(change.path)) continue;
    ASSIGN_OR_RETURN(std::string label,
                     LabelOf(runner, report.first, change.path));
    labels.push_back(std::move(label));
    report.test_files.push_back(change.path);
  }
  return labels;
}

// The branch checked out, or the commit when the head is detached.
absl::StatusOr<std::string> CurrentBranch(CommandRunner &runner) {
  ASSIGN_OR_RETURN(std::string branch,
                   Capture(runner,
                           {"git", "rev-parse", "--abbrev-ref", "HEAD"}));
  if (branch != "HEAD") return branch;
  return Capture(runner, {"git", "rev-parse", "HEAD"});
}

std::string_view Short(std::string_view commit) { return commit.substr(0, 12); }

std::string_view Describe(Outcome outcome) {
  switch (outcome) {
    case Outcome::kNotRun:
      return "not run";
    case Outcome::kPassed:
      return "passed";
    case Outcome::kTestsFailed:
      return "tests failed";
    case Outcome::kBuildFailed:
      return "build failed";
  }
  return "";
}

void AppendStage(std::string &text, std::string_view when, const Stage &stage) {
  if (stage.outcome == Outcome::kNotRun) return;
  absl::StrAppend(&text, "at ", when, " (", Short(stage.commit),
                  "): ", Describe(stage.outcome), "\n");
  for (const std::string &name : stage.failing) {
    absl::StrAppend(&text, "  failing: ", name, "\n");
  }
}

// The case named by a Google Test failure line, "[  FAILED  ] Suite.Case (3
// ms)", or by its summary's "[  FAILED  ] Suite.Case"; empty for any other
// line, and for the summary's count, "[  FAILED  ] 2 tests, listed below:".
std::string_view GoogleTestName(std::string_view line) {
  constexpr std::string_view kMarker = "[  FAILED  ] ";
  const size_t at = line.find(kMarker);
  if (at == std::string_view::npos) return {};
  std::string_view rest = line.substr(at + kMarker.size());
  rest = rest.substr(0, rest.find_first_of(" ,"));
  return !rest.empty() && !absl::ascii_isdigit(rest.front()) ? rest
                                                             : std::string_view();
}

// The case named by a unittest line, "FAIL: test_one (module.Class.test_one)"
// or "ERROR: ...". Bazel's own "ERROR: /path/BUILD:3:1: ..." lines are not.
std::string_view UnittestName(std::string_view line) {
  std::string_view rest = line;
  if (!absl::ConsumePrefix(&rest, "FAIL: ") &&
      !absl::ConsumePrefix(&rest, "ERROR: ")) {
    return {};
  }
  const size_t open = rest.find(" (");
  if (open == std::string_view::npos || !rest.ends_with(')')) return {};
  if (rest.substr(0, open).find_first_of(" /:") != std::string_view::npos) {
    return {};
  }
  return rest.substr(open + 2, rest.size() - open - 3);
}

// The target of Bazel's summary line, "//pkg:test   FAILED in 0.3s", "...
// TIMEOUT in ...", "... NO STATUS" or "... FAILED TO BUILD".
std::string_view BazelName(std::string_view line) {
  if (!line.starts_with("//") && !line.starts_with("@")) return {};
  const std::vector<std::string_view> words =
      absl::StrSplit(line, absl::ByAnyChar(" \t"), absl::SkipEmpty());
  const bool failed = words.size() > 1 && (words[1] == "FAILED" ||
                                           words[1] == "TIMEOUT" ||
                                           words[1] == "NO");
  return failed ? words[0] : std::string_view();
}

}  // namespace

bool IsTestFile(std::string_view path) {
  return IsBuildFile(path) ||
         std::ranges::any_of(kTestSuffixes,
                             [&](std::string_view suffix) {
                               return path.ends_with(suffix);
                             }) ||
         std::ranges::any_of(kTestDirectories,
                             [&](std::string_view directory) {
                               return HasDirectory(path, directory);
                             }) ||
         std::ranges::any_of(kTestTrees, [&](std::string_view tree) {
           return path.starts_with(tree);
         });
}

std::vector<std::string> ParseFailures(std::string_view output) {
  std::vector<std::string> names;
  for (std::string_view line : absl::StrSplit(output, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    for (const std::string_view name :
         {GoogleTestName(line), UnittestName(line), BazelName(line)}) {
      if (!name.empty() && std::ranges::find(names, name) == names.end()) {
        names.emplace_back(name);
      }
    }
  }
  return names;
}

absl::StatusOr<Report> Check(CommandRunner &runner, const Options &options) {
  ASSIGN_OR_RETURN(std::string dirty,
                   Capture(runner, {"git", "status", "--porcelain"}));
  if (!dirty.empty()) {
    return FailedPreconditionErrorBuilder()
           << "the work tree has uncommitted changes; commit or stash them:\n"
           << dirty;
  }
  Report report;
  ASSIGN_OR_RETURN(report.first,
                   Capture(runner, {"git", "rev-parse", "--verify",
                                    absl::StrCat(options.first, "^{commit}")}));
  ASSIGN_OR_RETURN(report.last,
                   Capture(runner, {"git", "rev-parse", "--verify",
                                    absl::StrCat(options.last, "^{commit}")}));
  ASSIGN_OR_RETURN(CommandResult ancestry,
                   Run(runner, {"git", "merge-base", "--is-ancestor",
                                report.first, report.last}));
  if (ancestry.exit_code != 0) {
    return InvalidArgumentErrorBuilder()
           << options.first << " is not an ancestor of " << options.last;
  }

  ASSIGN_OR_RETURN(std::vector<std::string> labels,
                   ClassifyFirstCommit(runner, report));
  if (!report.reasons.empty()) return report;
  if (labels.empty()) {
    report.reasons.emplace_back(
        "the first commit changes no test file, only build files or deletions");
    return report;
  }

  ASSIGN_OR_RETURN(std::string branch, CurrentBranch(runner));
  absl::Status ran = RunStages(runner, options, labels, report);
  const absl::Status restored =
      Capture(runner, {"git", "checkout", "-q", branch}).status();
  if (!ran.ok()) {
    if (restored.ok()) return ran;
    return absl::StatusBuilder(ran) << "; and restoring " << branch
                                    << " failed: " << restored;
  }
  RETURN_IF_ERROR(restored);
  report.verdict =
      report.reasons.empty() ? Verdict::kAccepted : Verdict::kRejected;
  return report;
}

std::string FormatReport(const Report &report) {
  std::string text =
      absl::StrCat("test-first: ",
                   report.verdict == Verdict::kAccepted ? "ACCEPTED"
                                                        : "REJECTED",
                   "\nslice: ", Short(report.first), "..", Short(report.last),
                   "\n");
  if (!report.targets.empty()) {
    absl::StrAppend(&text, "targets: ", absl::StrJoin(report.targets, ", "),
                    "\n");
  }
  AppendStage(text, "first", report.at_first);
  AppendStage(text, "last", report.at_last);
  for (const std::string &reason : report.reasons) {
    absl::StrAppend(&text, "rejected: ", reason, "\n");
  }
  return text;
}

}  // namespace dcfs::test_first
