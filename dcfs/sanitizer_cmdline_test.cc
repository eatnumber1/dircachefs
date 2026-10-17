// SPDX-License-Identifier: MIT
// The guest of a sanitizer build says so on its kernel command line
// (dcfs_sanitizer=asan or =ubsan), which is what run-qemu.sh reads to leave a
// sanitizer run out of the CPU budgets (guest/strace_lib.sh,
// budget_unheld_reason). A unit test whose guest lacks the word is held to a
// budget measured on the plain build. Only built under --config=asan and
// --config=ubsan (see BUILD.bazel).
#include <string>
#include <string_view>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gtest/gtest.h"

namespace {

#if __has_feature(address_sanitizer)
constexpr std::string_view kBuiltWith = "dcfs_sanitizer=asan";
#elif __has_feature(undefined_behavior_sanitizer)
constexpr std::string_view kBuiltWith = "dcfs_sanitizer=ubsan";
#else
#error "built without a sanitizer (see target_compatible_with)"
#endif

TEST(SanitizerCmdlineTest, TheGuestNamesTheSanitizerTheBinaryWasBuiltWith) {
  ASSERT_OK_AND_ASSIGN(std::string cmdline,
                       dcfs::testonly::ReadFileToString("/proc/cmdline"));
  const std::vector<std::string_view> words =
      absl::StrSplit(absl::StripAsciiWhitespace(cmdline), ' ');
  EXPECT_TRUE(absl::c_linear_search(words, kBuiltWith)) << cmdline;
}

}  // namespace
