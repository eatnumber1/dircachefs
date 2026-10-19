// SPDX-License-Identifier: MIT
// Checks a slice of a step branch for test-first evidence (tools/test_first.h):
//
//   bazel run //tools:test_first_tool -- --first=REV --last=REV
//       [--bazel_flag=FLAG[,FLAG...]] [--workspace=DIR]
//
// Run it from the lane that holds the branch (the work tree must be clean).
// It prints the report the merge record quotes; the exit code is 0 for an
// accepted slice and 3 for a rejected one. 2 (usage, or the check could not be
// made) and 1 (a flag Abseil does not know) mean there is no verdict.
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "tools/process_runner.h"
#include "tools/test_first_cli.h"

// NOLINTBEGIN(misc-use-internal-linkage): the flag objects ABSL_FLAG defines
// are looked up by name from other translation units
// (absl::FindCommandLineFlag).
ABSL_FLAG(std::string, first, "",
          "The slice's first commit: only tests, which must fail there.");
ABSL_FLAG(std::string, last, "",
          "The slice's last commit: the tests must pass there.");
ABSL_FLAG(std::vector<std::string>, bazel_flag, {},
          "Flags for `bazel test`, comma-separated (the flag is not "
          "cumulative: the last occurrence wins).");
ABSL_FLAG(std::string, workspace, "",
          "The checkout to work in (default: $BUILD_WORKSPACE_DIRECTORY, "
          "which `bazel run` sets, else the current directory).");
// NOLINTEND(misc-use-internal-linkage)

namespace {

// The value of `name` in the environment `envp` (main's third argument), or "".
std::string Env(char **envp, std::string_view name) {
  for (; *envp != nullptr; ++envp) {
    std::string_view entry = *envp;
    if (absl::ConsumePrefix(&entry, name) && absl::ConsumePrefix(&entry, "=")) {
      return std::string(entry);
    }
  }
  return "";
}

}  // namespace

// The standard library throws only on allocation failure, which this tool does
// not handle (no catch: style 1.6).
// NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc can escape.
int main(int argc, char **argv, char **envp) {
  absl::SetProgramUsageMessage(dcfs::test_first::kUsage);
  const std::vector<char *> rest = absl::ParseCommandLine(argc, argv);
  if (rest.size() > 1) {
    absl::FPrintF(stderr, "%s", dcfs::test_first::kUsage);
    return 2;
  }
  dcfs::test_first::ToolArgs args = {
      .first = absl::GetFlag(FLAGS_first),
      .last = absl::GetFlag(FLAGS_last),
      .bazel_flags = absl::GetFlag(FLAGS_bazel_flag),
      .workspace = absl::GetFlag(FLAGS_workspace)};
  if (args.workspace.empty()) {
    args.workspace = Env(envp, "BUILD_WORKSPACE_DIRECTORY");
  }
  dcfs::test_first::ProcessRunner runner(Env(envp, "PATH"));
  const dcfs::test_first::ToolResult result =
      dcfs::test_first::RunTool(args, runner);
  absl::PrintF("%s", result.out);
  absl::FPrintF(stderr, "%s", result.err);
  return result.exit_code;
}
