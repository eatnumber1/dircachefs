// SPDX-License-Identifier: MIT
#ifndef DCFS_FIXED_STRING_H_
#define DCFS_FIXED_STRING_H_

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace dcfs {

// A string literal as a value, so that fragments of a constant string, such
// as the SQL of dcfs/sql_statements.h, are named constants joined at compile
// time (docs/style.md rule-macros-discouraged: no macro splices a literal):
//
//   inline constexpr auto kText = FixedString("a, ") + kFragment;
//   inline constexpr std::string_view kView = kText;
//
// The view is of the constant `kText`, which lives for the whole program.
template <std::size_t N>
struct FixedString {
  // NOLINTNEXTLINE(google-explicit-constructor): a literal is a FixedString.
  consteval FixedString(const char (&text)[N + 1]) {
    std::copy_n(text, N + 1, chars);
  }
  consteval FixedString() = default;

  // NOLINTNEXTLINE(google-explicit-constructor): a view is how callers use it.
  constexpr operator std::string_view() const { return {chars, N}; }
  [[nodiscard]] constexpr std::size_t size() const { return N; }

  char chars[N + 1] = {};
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N - 1>;

template <std::size_t N, std::size_t M>
consteval FixedString<N + M> operator+(const FixedString<N> &left,
                                       const FixedString<M> &right) {
  FixedString<N + M> joined;
  std::copy_n(left.chars, N, joined.chars);
  std::copy_n(right.chars, M + 1, joined.chars + N);
  return joined;
}

}  // namespace dcfs

#endif  // DCFS_FIXED_STRING_H_
