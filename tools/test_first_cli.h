// SPDX-License-Identifier: MIT
// The test-first command (tools/test_first_main.cc) without its flags and its
// process: what it does with the values the flags gave.
#ifndef DCFS_TOOLS_TEST_FIRST_CLI_H_
#define DCFS_TOOLS_TEST_FIRST_CLI_H_

#include <string>
#include <vector>

#include "tools/test_first.h"

namespace dcfs::test_first {

struct ToolArgs {
  std::string first;
  std::string last;
  std::vector<std::string> bazel_flags;
  std::string workspace;  // the directory to work in; empty: the current one
};

// What the command writes and returns.
struct ToolResult {
  int exit_code = 0;  // 0 accepted, 1 rejected, 2 the check could not be made
  std::string out;    // the report, for standard output
  std::string err;    // the reason it could not be made, for standard error
};

inline constexpr char kUsage[] =
    "usage: test_first_tool --first=REV --last=REV "
    "[--bazel_flag=FLAG[,FLAG...]] [--workspace=DIR]\n";

ToolResult RunTool(const ToolArgs &args, CommandRunner &runner);

}  // namespace dcfs::test_first

#endif  // DCFS_TOOLS_TEST_FIRST_CLI_H_
