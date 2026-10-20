// SPDX-License-Identifier: MIT
#include "dcfs/fixed_string.h"

#include <string_view>
#include <type_traits>

#include "gtest/gtest.h"

namespace dcfs {
namespace {

// FixedString is consteval, so its tests are static_asserts. Not run, for the
// repository has no compile-fail harness, but checked by hand: a
// FixedString from an array that is not NUL-terminated does not compile.
constexpr auto kJoined = FixedString("ab") + FixedString("c");

static_assert(std::string_view(kJoined) == "abc");
static_assert(kJoined.size() == 3);
static_assert(kJoined.chars[3] == '\0');
static_assert(FixedString("").size() == 0);
static_assert(std::is_same_v<decltype(FixedString("ab")), FixedString<2>>);

TEST(FixedStringTest, JoinsAtCompileTimeAndViewsTheJoinedText) {
  EXPECT_EQ(std::string_view(kJoined), "abc");
}

}  // namespace
}  // namespace dcfs
