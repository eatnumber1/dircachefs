// SPDX-License-Identifier: MIT
#include "dcfs/interruption_point.h"

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

// Built at compile time from its literal.
static_assert(InterruptionPoint("a create's syscall").where() ==
              "a create's syscall");

// Not run, for the repository has no compile-fail harness, but checked by
// hand (2026-10-18): InterruptionPoint("") fails its static_assert ("an
// interruption point needs a name"), and one from a char array that is not
// a constant ("call to consteval function ... is not a constant
// expression").

TEST(InterruptionPointTest, PrintsItsName) {
  constexpr InterruptionPoint kPoint("listing a directory");
  EXPECT_EQ(absl::StrCat("before ", kPoint), "before listing a directory");
}

}  // namespace
}  // namespace dcfs
