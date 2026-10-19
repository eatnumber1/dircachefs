// SPDX-License-Identifier: MIT
// Checks a slice of a step branch for test-first evidence (tools/test_first.h):
//
//   bazel run //tools:test_first_tool -- --first=REV --last=REV
//       [--bazel_flag=FLAG]... [--workspace=DIR]
//
// Run it from the lane that holds the branch (the work tree must be clean).
// It prints the report the merge record quotes; the exit code is 0 for an
// accepted slice, 1 for a rejected one and 2 when the check could not be made.
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "tools/process_runner.h"
#include "tools/test_first.h"

namespace {

constexpr int kRejected = 1;
constexpr int kFailed = 2;

void Print(int fd, std::string_view text) {
  while (!text.empty()) {
    absl::StatusOr<size_t> n =
        dcfs::syscalls::write(fd, text.data(), text.size());
    if (!n.ok()) return;
    text.remove_prefix(*n);
  }
}

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

int Run(int argc, char **argv, char **envp) {
  dcfs::test_first::Options options;
  std::string workspace = Env(envp, "BUILD_WORKSPACE_DIRECTORY");
  bool usage_error = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (absl::ConsumePrefix(&arg, "--first=")) {
      options.first = std::string(arg);
    } else if (absl::ConsumePrefix(&arg, "--last=")) {
      options.last = std::string(arg);
    } else if (absl::ConsumePrefix(&arg, "--bazel_flag=")) {
      options.bazel_flags.emplace_back(arg);
    } else if (absl::ConsumePrefix(&arg, "--workspace=")) {
      workspace = std::string(arg);
    } else {
      usage_error = true;
    }
  }
  if (usage_error || options.first.empty() || options.last.empty()) {
    Print(2,
          "usage: test_first_tool --first=REV --last=REV "
          "[--bazel_flag=FLAG]... [--workspace=DIR]\n");
    return kFailed;
  }
  if (!workspace.empty()) {
    if (absl::Status moved = dcfs::syscalls::chdir(workspace); !moved.ok()) {
      Print(2, absl::StrCat("test_first_tool: ", moved.ToString(), "\n"));
      return kFailed;
    }
  }
  dcfs::test_first::ProcessRunner runner(Env(envp, "PATH"));
  absl::StatusOr<dcfs::test_first::Report> report =
      dcfs::test_first::Check(runner, options);
  if (!report.ok()) {
    Print(2,
          absl::StrCat("test_first_tool: ", report.status().ToString(), "\n"));
    return kFailed;
  }
  Print(1, dcfs::test_first::FormatReport(*report));
  return report->Accepted() ? 0 : kRejected;
}

}  // namespace

// Standard-library exceptions are allocation failures: say so and exit.
int main(int argc, char **argv, char **envp) {
  try {
    return Run(argc, argv, envp);
  } catch (const std::exception &error) {
    Print(2, absl::StrCat("test_first_tool: ", error.what(), "\n"));
    return kFailed;
  }
}
