// SPDX-License-Identifier: MIT
#include "tools/test_first_cli.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/syscalls.h"
#include "tools/test_first.h"

namespace dcfs::test_first {

ToolResult RunTool(const ToolArgs &args, CommandRunner &runner) {
  if (args.first.empty() || args.last.empty()) {
    return {.exit_code = 2, .err = kUsage};
  }
  if (!args.workspace.empty()) {
    if (absl::Status moved = syscalls::chdir(args.workspace); !moved.ok()) {
      return {.exit_code = 2,
              .err = absl::StrCat("test_first_tool: ", args.workspace, ": ",
                                  moved.ToString(), "\n")};
    }
  }
  const Options options = {
      .first = args.first, .last = args.last, .bazel_flags = args.bazel_flags};
  absl::StatusOr<Report> report = Check(runner, options);
  if (!report.ok()) {
    return {.exit_code = 2,
            .err = absl::StrCat("test_first_tool: ", report.status().ToString(),
                                "\n")};
  }
  return {.exit_code = report->Accepted() ? 0 : 3,
          .out = FormatReport(*report)};
}

}  // namespace dcfs::test_first
