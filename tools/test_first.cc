// SPDX-License-Identifier: MIT
#include "tools/test_first.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "dcfs/status.h"

namespace dcfs::test_first {
namespace {

// What a slice's first commit may change (the one list).
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
constexpr std::string_view kBuildFile = "BUILD.bazel";

bool IsBuildFile(std::string_view path) {
  return path.substr(path.rfind('/') + 1) == kBuildFile;
}

// A test's own source, as against its data and helpers.
bool IsTestSource(std::string_view path) {
  return absl::c_any_of(kTestSuffixes, [&](std::string_view suffix) {
    return path.ends_with(suffix);
  });
}

bool HasDirectory(std::string_view path, std::string_view directory) {
  return absl::StrContains(absl::StrCat("/", path),
                           absl::StrCat("/", directory, "/"));
}

// The output of a command that has to succeed, without the final newline.
absl::StatusOr<std::string> Capture(CommandRunner &runner,
                                    std::vector<std::string> argv) {
  const std::string line = absl::StrJoin(argv, " ");
  ASSIGN_OR_RETURN(CommandResult result, runner.Run(argv));
  if (result.exit_code != 0) {
    return InternalErrorBuilder() << "`" << line << "` exit "
                                  << result.exit_code << ": " << result.output;
  }
  return std::string(absl::StripTrailingAsciiWhitespace(result.output));
}

constexpr size_t kErrorsQuoted = 5;

struct Change {
  char status;  // A, M, D, T ...
  std::string path;
};

// The test files of a first commit.
struct Changes {
  std::vector<std::string> files;          // paths
  std::vector<std::string> labels;         // their Bazel labels
  std::vector<std::string> source_labels;  // the labels of the test sources
  std::vector<std::string>
      build_packages;  // packages whose BUILD.bazel changed
};

absl::StatusOr<std::vector<Change>> ChangedFiles(CommandRunner &runner,
                                                 const std::string &commit) {
  ASSIGN_OR_RETURN(
      std::string text,
      Capture(runner, {"git", "diff-tree", "--no-commit-id", "--name-status",
                       "-r", "--no-renames", "--root", commit}));
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
                                  ? std::string(kBuildFile)
                                  : absl::StrCat(directory, "/", kBuildFile);
    const std::vector<std::string> argv = {"git", "cat-file", "-e",
                                           absl::StrCat(commit, ":", build)};
    ASSIGN_OR_RETURN(CommandResult found, runner.Run(argv));
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
      return InternalErrorBuilder() << "`bazel test` exit " << result.exit_code
                                    << ": " << result.output;
  }
}

// Bazel's "ERROR: ..." lines.
std::vector<std::string> ErrorLines(std::string_view output) {
  std::vector<std::string> errors;
  for (std::string_view line : absl::StrSplit(output, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    if (line.starts_with("ERROR: ")) errors.emplace_back(line);
  }
  return errors;
}

absl::Status Detach(CommandRunner &runner, const std::string &commit) {
  return Capture(runner, {"git", "checkout", "-q", "--detach", commit})
      .status();
}

// `bazel test` of `targets` at the commit checked out.
absl::StatusOr<Stage> Test(CommandRunner &runner, const std::string &commit,
                           const Options &options,
                           const std::vector<std::string> &targets) {
  std::vector<std::string> argv = {"bazel",        "test",
                                   "--keep_going", "--test_output=errors",
                                   "--color=no",   "--curses=no"};
  argv.insert(argv.end(), options.bazel_flags.begin(),
              options.bazel_flags.end());
  argv.insert(argv.end(), targets.begin(), targets.end());
  ASSIGN_OR_RETURN(CommandResult result, runner.Run(argv));
  ASSIGN_OR_RETURN(Outcome outcome, OutcomeOf(result));
  return Stage{.commit = commit,
               .outcome = outcome,
               .failing = ParseFailures(result.output),
               .passed = ParsePassed(result.output),
               .errors = ErrorLines(result.output)};
}

// The rules of `bazel query //package:all --output=build` at the commit
// checked out.
absl::StatusOr<std::string> QueryBuild(CommandRunner &runner,
                                       const std::string &package) {
  return Capture(
      runner, {"bazel", "query", "--keep_going", "--output=build", "--color=no",
               "--curses=no", absl::StrCat("//", package, ":all")});
}

// The first commit's parent, or empty for a root commit.
absl::StatusOr<std::string> ParentOf(CommandRunner &runner,
                                     const std::string &commit) {
  ASSIGN_OR_RETURN(
      std::string history,
      Capture(runner, {"git", "rev-list", "--parents", "-n", "1", commit}));
  const std::vector<std::string_view> words =
      absl::StrSplit(history, ' ', absl::SkipEmpty());
  return words.size() > 1 ? std::string(words[1]) : std::string();
}

// The build file of `package` in `commit`: whether it has one.
absl::StatusOr<bool> HasBuildFile(CommandRunner &runner,
                                  const std::string &commit,
                                  const std::string &package) {
  const std::vector<std::string> argv = {
      "git", "cat-file", "-e",
      absl::StrCat(commit, ":",
                   package.empty() ? std::string(kBuildFile)
                                   : absl::StrCat(package, "/", kBuildFile))};
  ASSIGN_OR_RETURN(CommandResult found, runner.Run(argv));
  return found.exit_code == 0;
}

// The rules of the packages that had a build file at the parent, queried there.
// The commit checked out is `first` again on return.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>> RulesAtParent(
    CommandRunner &runner, const std::string &first,
    const std::vector<std::string> &packages) {
  absl::flat_hash_map<std::string, std::string> before;
  ASSIGN_OR_RETURN(std::string parent, ParentOf(runner, first));
  if (parent.empty()) return before;
  std::vector<std::string> existing;
  for (const std::string &package : packages) {
    ASSIGN_OR_RETURN(bool has, HasBuildFile(runner, parent, package));
    if (has) existing.push_back(package);
  }
  if (existing.empty()) return before;
  RETURN_IF_ERROR(Detach(runner, parent));
  for (const std::string &package : existing) {
    ASSIGN_OR_RETURN(before[package], QueryBuild(runner, package));
  }
  RETURN_IF_ERROR(Detach(runner, first));
  return before;
}

// What the build files of `packages` say in the first commit against its
// parent: a reason for each rule that may not change in a tests-only commit.
// The commit checked out is the first commit, and is again on return.
absl::StatusOr<std::vector<std::string>> BuildFileViolations(
    CommandRunner &runner, const std::string &first,
    const std::vector<std::string> &packages) {
  std::vector<std::string> reasons;
  absl::flat_hash_map<std::string, std::string> after;
  for (const std::string &package : packages) {
    ASSIGN_OR_RETURN(after[package], QueryBuild(runner, package));
  }
  ASSIGN_OR_RETURN(auto before, RulesAtParent(runner, first, packages));
  for (const std::string &package : packages) {
    absl::c_move(BuildRuleViolations(before[package], after[package], package),
                 std::back_inserter(reasons));
  }
  return reasons;
}

struct Owners {
  std::vector<std::string> all;  // the tests that read any of the files
  std::vector<std::string> own;  // those whose own source is among them
  bool partial = false;          // some package did not load
};

struct QueryResult {
  std::vector<std::string> targets;
  bool partial = false;  // some package did not load
};

// The test targets that own `labels`, found at the commit checked out. Bazel
// exits 3 when it answers from the packages that did load.
absl::StatusOr<QueryResult> Query(CommandRunner &runner,
                                  const std::vector<std::string> &labels) {
  const std::vector<std::string> argv = {
      "bazel",
      "query",
      "--keep_going",
      "--output=label",
      "--color=no",
      "--curses=no",
      absl::StrCat("kind(\"(cc|py|sh)_test\", rdeps(//..., set(",
                   absl::StrJoin(labels, " "), ")))")};
  ASSIGN_OR_RETURN(CommandResult result, runner.Run(argv));
  if (result.exit_code != 0 && result.exit_code != 3) {
    return InternalErrorBuilder()
           << "`" << absl::StrJoin(argv, " ") << "` exit " << result.exit_code
           << ": " << result.output;
  }
  QueryResult found = {.partial = result.exit_code == 3};
  for (std::string_view line :
       absl::StrSplit(result.output, '\n', absl::SkipEmpty())) {
    line = absl::StripAsciiWhitespace(line);
    if (line.starts_with("//") || line.starts_with("@")) {
      found.targets.emplace_back(line);
    }
  }
  return found;
}

absl::StatusOr<Owners> FindOwners(CommandRunner &runner,
                                  const Changes &changes) {
  Owners owners;
  ASSIGN_OR_RETURN(QueryResult all, Query(runner, changes.labels));
  owners.all = std::move(all.targets);
  owners.partial = all.partial;
  if (changes.source_labels.size() == changes.labels.size()) {
    owners.own = owners.all;
  } else if (!changes.source_labels.empty()) {
    ASSIGN_OR_RETURN(QueryResult own, Query(runner, changes.source_labels));
    owners.own = std::move(own.targets);
    owners.partial = owners.partial || own.partial;
  }
  return owners;
}

bool NamesSomethingOfTheSlice(const std::vector<std::string> &errors,
                              const Changes &changes,
                              const std::vector<std::string> &targets) {
  return absl::c_any_of(errors, [&](const std::string &error) {
    return absl::c_any_of(changes.files,
                          [&](const std::string &file) {
                            return absl::StrContains(error, file);
                          }) ||
           absl::c_any_of(targets, [&](const std::string &target) {
             return absl::StrContains(error, target);
           });
  });
}

// What the first stage says against the slice.
void JudgeFirst(const Changes &changes, const Owners &owners, Report &report) {
  const Stage &stage = report.at_first;
  if (stage.outcome == Outcome::kBuildFailed &&
      !NamesSomethingOfTheSlice(stage.errors, changes, owners.all)) {
    report.reasons.emplace_back(
        "the build failed at the first commit with errors that name no test "
        "file or target of the slice");
  }
  bool own_passed = false;
  for (const std::string &target : owners.own) {
    if (!absl::c_linear_search(stage.passed, target)) continue;
    own_passed = true;
    report.reasons.push_back(
        absl::StrCat(target,
                     " passed at the first commit, though its own source "
                     "changed there (it must fail there)"));
  }
  if (stage.outcome == Outcome::kPassed && !own_passed) {
    report.reasons.emplace_back(
        "every test passed at the first commit (one must fail there)");
  }
}

// Runs the tests at both commits of the slice, detached from the branch; the
// report is filled in as far as it got.
absl::Status RunStages(CommandRunner &runner, const Options &options,
                       const Changes &changes, Report &report) {
  RETURN_IF_ERROR(Detach(runner, report.first));
  ASSIGN_OR_RETURN(report.reasons, BuildFileViolations(runner, report.first,
                                                       changes.build_packages));
  if (!report.reasons.empty()) return absl::OkStatus();
  ASSIGN_OR_RETURN(Owners owners, FindOwners(runner, changes));
  if (owners.partial) {
    // A rule of the first commit does not load (it needs what the code adds):
    // the tests are found at the last commit and fail to build at the first.
    RETURN_IF_ERROR(Detach(runner, report.last));
    ASSIGN_OR_RETURN(owners, FindOwners(runner, changes));
    RETURN_IF_ERROR(Detach(runner, report.first));
  }
  report.targets = owners.all;
  if (report.targets.empty()) {
    report.reasons.emplace_back(
        "no test target owns the test files of the first commit");
    return absl::OkStatus();
  }
  ASSIGN_OR_RETURN(report.at_first,
                   Test(runner, report.first, options, report.targets));
  JudgeFirst(changes, owners, report);
  if (report.at_first.errors.size() > kErrorsQuoted) {
    report.at_first.errors.resize(kErrorsQuoted);
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
// a test file (or for no file at all), and the test files a target can own.
absl::StatusOr<Changes> ClassifyFirstCommit(CommandRunner &runner,
                                            Report &report) {
  ASSIGN_OR_RETURN(std::vector<Change> changed,
                   ChangedFiles(runner, report.first));
  if (changed.empty()) {
    report.reasons.emplace_back("the first commit touches no file");
  }
  Changes changes;
  for (const Change &change : changed) {
    if (!IsTestFile(change.path)) {
      report.reasons.push_back(absl::StrCat("the first commit touches ",
                                            change.path,
                                            ", which is not a test file"));
      continue;
    }
    // A deleted file has no target to query, and a build file is not a test.
    if (change.status == 'D') continue;
    if (IsBuildFile(change.path)) {
      changes.build_packages.push_back(std::string(
          change.path.substr(0, change.path.size() > kBuildFile.size()
                                    ? change.path.size() - kBuildFile.size() - 1
                                    : 0)));
      continue;
    }
    ASSIGN_OR_RETURN(std::string label,
                     LabelOf(runner, report.first, change.path));
    if (IsTestSource(change.path)) changes.source_labels.push_back(label);
    changes.labels.push_back(std::move(label));
    changes.files.push_back(change.path);
  }
  return changes;
}

// The branch checked out, or the commit when the head is detached.
absl::StatusOr<std::string> CurrentBranch(CommandRunner &runner) {
  ASSIGN_OR_RETURN(
      std::string branch,
      Capture(runner, {"git", "rev-parse", "--abbrev-ref", "HEAD"}));
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
  for (const std::string &name : stage.passed) {
    absl::StrAppend(&text, "  passed at ", when, ": ", name, "\n");
  }
  for (const std::string &error : stage.errors) {
    absl::StrAppend(&text, "  error: ", error, "\n");
  }
  if (stage.outcome == Outcome::kBuildFailed) {
    absl::StrAppend(&text,
                    "  confirm the error names a symbol or target the code "
                    "commit adds\n");
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
  return !rest.empty() && !absl::ascii_isdigit(rest.front())
             ? rest
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
  const bool failed =
      words.size() > 1 &&
      (words[1] == "FAILED" || words[1] == "TIMEOUT" || words[1] == "NO");
  return failed ? words[0] : std::string_view();
}

struct Rule {
  std::string kind;
  std::string generator;
  std::string visibility;
  std::string body;  // without comments, which carry line numbers
  bool testonly = false;
};

// The value of `  attribute = value,` in a rule's line, or empty.
std::string_view AttributeValue(std::string_view line,
                                std::string_view attribute) {
  line = absl::StripAsciiWhitespace(line);
  if (!absl::ConsumePrefix(&line, attribute)) return {};
  if (!absl::ConsumePrefix(&line, " = ")) return {};
  absl::ConsumeSuffix(&line, ",");
  return line;
}

std::string Unquoted(std::string_view value) {
  return std::string(absl::StripPrefix(absl::StripSuffix(value, "\""), "\""));
}

// Reads a rule line by line from `bazel query --output=build`.
class RuleReader {
 public:
  void Line(std::string_view line) {
    line = absl::StripAsciiWhitespace(line);
    // The location of the macro call shifts with every line added above it.
    if (line.starts_with("generator_location = ")) return;
    absl::StrAppend(&rule_.body, line, "\n");
    if (collecting_visibility_) {
      absl::StrAppend(&rule_.visibility, "\n", line);
      collecting_visibility_ = !line.starts_with("]");
      return;
    }
    if (std::string_view n = AttributeValue(line, "name"); !n.empty()) {
      name_ = Unquoted(n);
    }
    if (std::string_view g = AttributeValue(line, "generator_name");
        !g.empty()) {
      rule_.generator = Unquoted(g);
    }
    if (std::string_view v = AttributeValue(line, "testonly"); !v.empty()) {
      rule_.testonly = v == "True" || v == "1";
    }
    if (std::string_view v = AttributeValue(line, "visibility"); !v.empty()) {
      rule_.visibility = std::string(line);
      collecting_visibility_ = v == "[";
    }
  }

  void Begin(std::string_view kind) {
    rule_ = Rule{.kind = std::string(kind)};
    name_.clear();
    collecting_visibility_ = false;
  }
  const std::string &name() const { return name_; }
  const Rule &rule() const { return rule_; }

 private:
  Rule rule_;
  std::string name_;
  bool collecting_visibility_ = false;
};

absl::btree_map<std::string, Rule> ParseRules(std::string_view output) {
  absl::btree_map<std::string, Rule> rules;
  RuleReader reader;
  bool inside = false;
  for (std::string_view line : absl::StrSplit(output, '\n')) {
    line = absl::StripTrailingAsciiWhitespace(line);
    if (line.starts_with("#") || line.empty()) continue;
    if (!inside) {
      inside = absl::ConsumeSuffix(&line, "(");
      if (inside) reader.Begin(line);
    } else if (line == ")") {
      rules[reader.name()] = reader.rule();
      inside = false;
    } else {
      reader.Line(line);
    }
  }
  return rules;
}

// Whether a rule may change in a tests-only commit: a test, a test_suite (it
// lists tests), testonly, or generated by a test (the files a test macro
// makes).
bool IsTestRule(const Rule &rule,
                const absl::btree_map<std::string, Rule> &known) {
  if (rule.kind.ends_with("_test") || rule.kind == "test_suite" ||
      rule.testonly) {
    return true;
  }
  const auto generator = known.find(rule.generator);
  return generator != known.end() && generator->second.kind.ends_with("_test");
}

// The lines of a rule body, with the srcs list on one line (`srcs = ["a",
// "b"],`) made into its opening line and one `srcs:"entry",` line per entry.
std::vector<std::string> BodyEntries(std::string_view body) {
  std::vector<std::string> entries;
  for (std::string_view line : absl::StrSplit(body, '\n')) {
    constexpr std::string_view kOpen = "srcs = [";
    if (!line.starts_with(kOpen) || !line.ends_with("],") ||
        line.size() < kOpen.size() + 2) {
      entries.emplace_back(line);
      continue;
    }
    entries.emplace_back(kOpen);
    const std::string_view list =
        line.substr(kOpen.size(), line.size() - kOpen.size() - 2);
    for (std::string_view entry : absl::StrSplit(list, ", ")) {
      entries.push_back(absl::StrCat("srcs:", entry, ","));
    }
  }
  return entries;
}

// Whether two rule bodies differ only in srcs entries that are test files (a
// glob of sources gains or loses a test file with the commit that adds or
// removes it).
bool OnlyTestFileEntriesDiffer(std::string_view before,
                               std::string_view after) {
  absl::btree_map<std::string_view, int> changes;
  const std::vector<std::string> old_entries = BodyEntries(before);
  const std::vector<std::string> new_entries = BodyEntries(after);
  for (const std::string &line : old_entries) --changes[line];
  for (const std::string &line : new_entries) ++changes[line];
  return absl::c_all_of(changes, [](const auto &change) {
    if (change.second == 0) return true;
    std::string_view entry = absl::StripSuffix(change.first, ",");
    return absl::ConsumePrefix(&entry, "srcs:") && entry.starts_with("\"") &&
           IsTestFile(absl::StrReplaceAll(
               absl::StripPrefix(Unquoted(entry), "//"), {{":", "/"}}));
  });
}

std::string_view ChangeOf(bool was, bool is) {
  if (!was) return "new";
  return is ? "changed" : "removed";
}

// Why a changed rule is not allowed in a tests-only commit, or empty: `was`
// and `is` are the rule before and after (null when it is new or removed).
std::string RuleViolation(const std::string &label, const Rule *was,
                          const Rule *is,
                          const absl::btree_map<std::string, Rule> &old_rules,
                          const absl::btree_map<std::string, Rule> &known) {
  const Rule &rule = is != nullptr ? *is : *was;
  const bool in_both = was != nullptr && is != nullptr;
  if (in_both && was->visibility != is->visibility) {
    return absl::StrCat(label, " changes visibility");
  }
  if (in_both && was->kind == is->kind &&
      OnlyTestFileEntriesDiffer(was->body, is->body)) {
    return "";
  }
  if (!IsTestRule(rule, known)) {
    return absl::StrCat(label, " (", rule.kind, ") is ",
                        ChangeOf(was != nullptr, is != nullptr),
                        " and is neither a test nor testonly");
  }
  if (in_both && !IsTestRule(*was, old_rules)) {
    return absl::StrCat(label, " (", rule.kind,
                        ") was neither a test nor testonly before");
  }
  return "";
}

}  // namespace

bool IsTestFile(std::string_view path) {
  return IsBuildFile(path) || IsTestSource(path) ||
         absl::c_any_of(kTestDirectories,
                        [&](std::string_view directory) {
                          return HasDirectory(path, directory);
                        }) ||
         absl::c_any_of(kTestTrees, [&](std::string_view tree) {
           return path.starts_with(tree);
         });
}

std::vector<std::string> ParseFailures(std::string_view output) {
  std::vector<std::string> names;
  for (std::string_view line : absl::StrSplit(output, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    for (const std::string_view name :
         {GoogleTestName(line), UnittestName(line), BazelName(line)}) {
      if (!name.empty() && !absl::c_linear_search(names, name)) {
        names.emplace_back(name);
      }
    }
  }
  return names;
}

std::vector<std::string> BuildRuleViolations(std::string_view before,
                                             std::string_view after,
                                             std::string_view package) {
  const absl::btree_map<std::string, Rule> old_rules = ParseRules(before);
  const absl::btree_map<std::string, Rule> new_rules = ParseRules(after);
  absl::btree_map<std::string, Rule> known = old_rules;
  for (const auto &[name, rule] : new_rules) known[name] = rule;
  std::vector<std::string> violations;
  for (const auto &[name, rule] : known) {
    const auto was = old_rules.find(name);
    const auto is = new_rules.find(name);
    const bool in_both = was != old_rules.end() && is != new_rules.end();
    if (in_both && was->second.body == is->second.body) continue;
    const std::string label =
        package.empty() ? name : absl::StrCat("//", package, ":", name);
    if (std::string violation = RuleViolation(
            label, was != old_rules.end() ? &was->second : nullptr,
            is != new_rules.end() ? &is->second : nullptr, old_rules, known);
        !violation.empty()) {
      violations.push_back(std::move(violation));
    }
  }
  return violations;
}

std::vector<std::string> ParsePassed(std::string_view output) {
  std::vector<std::string> names;
  for (std::string_view line : absl::StrSplit(output, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    if (!line.starts_with("//") && !line.starts_with("@")) continue;
    const std::vector<std::string_view> words =
        absl::StrSplit(line, absl::ByAnyChar(" \t"), absl::SkipEmpty());
    const bool passed =
        words.size() > 1 &&
        (words[1] == "PASSED" ||
         (words[1] == "(cached)" && words.size() > 2 && words[2] == "PASSED"));
    if (passed) names.emplace_back(words[0]);
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
  const std::vector<std::string> ancestor_argv = {
      "git", "merge-base", "--is-ancestor", report.first, report.last};
  ASSIGN_OR_RETURN(CommandResult ancestry, runner.Run(ancestor_argv));
  if (ancestry.exit_code != 0) {
    return InvalidArgumentErrorBuilder()
           << options.first << " is not an ancestor of " << options.last;
  }

  ASSIGN_OR_RETURN(Changes changes, ClassifyFirstCommit(runner, report));
  if (!report.reasons.empty()) return report;
  if (changes.labels.empty()) {
    report.reasons.emplace_back(
        "the first commit changes no test file, only build files or deletions");
    return report;
  }

  ASSIGN_OR_RETURN(std::string branch, CurrentBranch(runner));
  absl::Status ran = RunStages(runner, options, changes, report);
  const absl::Status restored =
      Capture(runner, {"git", "checkout", "-q", branch}).status();
  if (!ran.ok()) {
    if (restored.ok()) return ran;
    return absl::StatusBuilder(ran)
           << "; and restoring " << branch << " failed: " << restored;
  }
  RETURN_IF_ERROR(restored);
  return report;
}

std::string FormatReport(const Report &report) {
  std::string text = absl::StrCat(
      "test-first: ", report.Accepted() ? "ACCEPTED" : "REJECTED",
      "\nslice: ", Short(report.first), "..", Short(report.last), "\n");
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
