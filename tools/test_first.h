// SPDX-License-Identifier: MIT
// The test-first check (plan step 25.52): a slice's first commit changes only
// tests, and the tests that commit adds or changes fail there and pass at the
// slice's last commit. The logic only; the git and Bazel processes are run by
// a CommandRunner, so the unit tests script them.
#ifndef DCFS_TOOLS_TEST_FIRST_H_
#define DCFS_TOOLS_TEST_FIRST_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace dcfs::test_first {

struct CommandResult {
  int exit_code = 0;
  std::string output;  // standard output and standard error together
};

// Runs `argv` (a program found on PATH, then its arguments) to the end. A
// non-zero exit code is a result; an error Status means the program could not
// be run.
class CommandRunner {
 public:
  virtual ~CommandRunner() = default;
  virtual absl::StatusOr<CommandResult> Run(
      std::span<const std::string> argv) = 0;
};

// Whether a first commit may change `path`: a test, its data or its Bazel
// target. The one list of what counts is in test_first.cc.
bool IsTestFile(std::string_view path);

// The failing tests named in the output of `bazel test --test_output=errors`:
// Google Test cases, unittest cases and Bazel's own summary lines (a target
// that failed, timed out or did not build), each once, in output order.
std::vector<std::string> ParseFailures(std::string_view output);

struct Options {
  std::string first;  // the slice's first commit (any revision git resolves)
  std::string last;   // the slice's last commit
  std::vector<std::string> bazel_flags;  // added to `bazel test`
};

enum class Verdict { kAccepted, kRejected };

enum class Outcome { kNotRun, kPassed, kTestsFailed, kBuildFailed };

// What `bazel test` did at one commit.
struct Stage {
  std::string commit;
  Outcome outcome = Outcome::kNotRun;
  std::vector<std::string> failing;
};

struct Report {
  Verdict verdict = Verdict::kRejected;
  std::vector<std::string> reasons;  // why kRejected
  std::string first;                 // full commit ids
  std::string last;
  std::vector<std::string> test_files;  // the first commit's files, as labels
  std::vector<std::string> targets;     // the test targets that own them
  Stage at_first;
  Stage at_last;
};

// Checks the slice from the first commit to the last. Needs a clean work tree
// and leaves it on the commit it started on. An error Status means the check
// could not be made; a slice that fails it is a Report that is kRejected.
absl::StatusOr<Report> Check(CommandRunner &runner, const Options &options);

// The report as the text a merge record quotes.
std::string FormatReport(const Report &report);

}  // namespace dcfs::test_first

#endif  // DCFS_TOOLS_TEST_FIRST_H_
