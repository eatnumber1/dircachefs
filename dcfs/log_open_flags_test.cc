#include "dcfs/log_open_flags.h"

#include <fcntl.h>

#include <string>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

// AbslStringify is a hidden friend: absl::StrCat finds it by ADL on
// dcfs::LogOpenFlags with no declaration visible outside the class.
TEST(LogOpenFlagsTest, PrintsTheNamesOfTheFlagsSet) {
  const std::string text =
      absl::StrCat(LogOpenFlags(O_WRONLY | O_CREAT | O_CLOEXEC));
  EXPECT_NE(text.find("O_WRONLY"), std::string::npos) << text;
  EXPECT_NE(text.find("O_CREAT"), std::string::npos) << text;
  EXPECT_NE(text.find("O_CLOEXEC"), std::string::npos) << text;
  EXPECT_EQ(text.find("O_TRUNC"), std::string::npos) << text;
  EXPECT_EQ(text.find("O_APPEND"), std::string::npos) << text;
}

TEST(LogOpenFlagsTest, SeparatesNamesWithBars) {
  EXPECT_NE(absl::StrCat(LogOpenFlags(O_TRUNC | O_APPEND)).find(" | "),
            std::string::npos);
}

}  // namespace
}  // namespace dcfs
