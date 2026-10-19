// SPDX-License-Identifier: MIT
#include "tools/process_runner.h"

#include <sys/stat.h>

// WIFSIGNALED, WTERMSIG and WEXITSTATUS: misc-include-cleaner attributes the
// POSIX macros to this C header (docs/style.md, "A POSIX macro").
// NOLINTNEXTLINE(modernize-deprecated-headers): the macros are defined here.
#include <stdlib.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "dcfs/status.h"
#include "dcfs/subprocess.h"
#include "dcfs/syscalls.h"
#include "tools/test_first.h"

namespace dcfs::test_first {
// The path of the program `name`: itself when it has a '/', else the first
// executable regular file of that name in a directory of `path`.
absl::StatusOr<std::string> Resolve(std::string_view name,
                                    std::string_view path) {
  if (absl::StrContains(name, '/')) return std::string(name);
  for (std::string_view directory : absl::StrSplit(path, ':')) {
    std::string candidate =
        absl::StrCat(directory.empty() ? "." : directory, "/", name);
    absl::StatusOr<struct stat> found = syscalls::stat(candidate);
    if (found.ok() && S_ISREG(found->st_mode) && (found->st_mode & 0111) != 0) {
      return candidate;
    }
  }
  return NotFoundErrorBuilder()
         << name << " is not in any directory of PATH (" << path << ")";
}

absl::StatusOr<CommandResult> ProcessRunner::Run(
    std::span<const std::string> argv) {
  if (argv.empty()) return InvalidArgumentErrorBuilder() << "no program";
  ASSIGN_OR_RETURN(std::string program, Resolve(argv.front(), path_));
  ASSIGN_OR_RETURN(ProgramRun run, RunProgram(program, argv, Capture::kOutput));
  // As a shell reports it: 128 plus the signal for a death by signal.
  return CommandResult{.exit_code = WIFSIGNALED(run.wait_status)
                                        ? 128 + WTERMSIG(run.wait_status)
                                        : WEXITSTATUS(run.wait_status),
                       .output = std::move(run.output)};
}

}  // namespace dcfs::test_first
