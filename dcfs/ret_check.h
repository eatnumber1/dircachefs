#ifndef DCFS_RET_CHECK_H_
#define DCFS_RET_CHECK_H_

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/types/source_location.h"
#include "dcfs/status.h"

// RET_CHECK* build a kInternal `absl::StatusBuilder` and return it from the
// current function when the check fails. Like ABSL_RETURN_IF_ERROR, the
// macro's value is a StatusBuilder, so it can be extended with `<<` to add
// context, and it is usable as a statement in functions returning either
// absl::Status or absl::StatusOr<T>:
//
//   RET_CHECK(ptr != nullptr) << "while opening " << path;
//   RET_CHECK_EQ(a, b);
//   RET_CHECK_OK(SomeStatusReturningCall());
//
// On success, the macros are no-ops (control falls through).

namespace dcfs {
namespace internal {

// Builds the kInternal StatusBuilder for a failed RET_CHECK(expr).
inline absl::StatusBuilder RetCheckFail(
    std::string_view expr_str,
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return InternalErrorBuilder(loc) << "RET_CHECK failure: " << expr_str;
}

// operator<< on absl::StatusBuilder has no overload for std::nullptr_t (a
// common RET_CHECK_NE(ptr, nullptr) operand), so route it through a name
// that does: print "nullptr" for the null-pointer-constant type, and pass
// everything else through unchanged.
inline const char *RetCheckStreamable(std::nullptr_t) { return "nullptr"; }

template <typename T,
          typename = std::enable_if_t<
              !std::is_same_v<std::decay_t<T>, std::nullptr_t>>>
const T &RetCheckStreamable(const T &value) {
  return value;
}

// Evaluates and stores both RET_CHECK_{EQ,NE,...} operands (by value) via a
// single declaration. This lets the comparison macros use just one
// if/else (like RET_CHECK and RET_CHECK_OK) instead of nesting one if per
// operand -- nesting would trip -Wdangling-else, and separately binding two
// `auto&&` operands of possibly-different types isn't expressible in a
// single declaration anyway. Operands are still evaluated exactly once.
// NOTE: the member names deliberately avoid "lhs"/"rhs" -- those are the
// DCFS_RET_CHECK_OP_ macro's parameter names, and the preprocessor would
// substitute them even after a '.', mangling `ops.lhs` into e.g. `ops.res`.
template <typename Lhs, typename Rhs>
struct RetCheckOperands {
  Lhs lhs_value;
  Rhs rhs_value;
};
template <typename Lhs, typename Rhs>
RetCheckOperands(Lhs, Rhs) -> RetCheckOperands<Lhs, Rhs>;

// Builds the kInternal StatusBuilder for a failed RET_CHECK_{EQ,NE,...},
// including both operand values (each already evaluated exactly once by the
// caller).
template <typename Lhs, typename Rhs>
absl::StatusBuilder RetCheckFailOp(
    std::string_view lhs_str, std::string_view op_str,
    std::string_view rhs_str, const Lhs &lhs_value, const Rhs &rhs_value,
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return InternalErrorBuilder(loc)
      << "RET_CHECK failure: " << lhs_str << " " << op_str << " " << rhs_str
      << " (" << RetCheckStreamable(lhs_value) << " vs "
      << RetCheckStreamable(rhs_value) << ")";
}

// Builds the kInternal StatusBuilder for a failed RET_CHECK_OK(status_expr),
// embedding the original status (code and message).
inline absl::StatusBuilder RetCheckFailStatus(
    std::string_view expr_str, const absl::Status &status,
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return InternalErrorBuilder(loc)
      << "RET_CHECK_OK failure: " << expr_str << " is not OK: " << status;
}

}  // namespace internal
}  // namespace dcfs

// Suppresses "dangling else" warnings/ambiguity, following the same idiom
// absl's status macros use.
#define DCFS_RET_CHECK_ELSE_BLOCKER_ \
  switch (0)                        \
  case 0:                           \
  default:  // NOLINT

#define RET_CHECK(expr)                                                    \
  DCFS_RET_CHECK_ELSE_BLOCKER_                                              \
  if (ABSL_PREDICT_TRUE(expr)) {                                            \
  } else /* NOLINT */                                                       \
    return ::dcfs::internal::RetCheckFail(#expr)

#define DCFS_RET_CHECK_OP_(unique, op, lhs, rhs)                            \
  DCFS_RET_CHECK_ELSE_BLOCKER_                                               \
  if (auto dcfs_rc_ops_##unique =                                          \
          ::dcfs::internal::RetCheckOperands{(lhs), (rhs)};                \
      ABSL_PREDICT_TRUE(dcfs_rc_ops_##unique.lhs_value                     \
                             op dcfs_rc_ops_##unique.rhs_value)) {          \
  } else /* NOLINT */                                                      \
    return ::dcfs::internal::RetCheckFailOp(                              \
        #lhs, #op, #rhs, dcfs_rc_ops_##unique.lhs_value,                   \
        dcfs_rc_ops_##unique.rhs_value)

#define RET_CHECK_EQ(lhs, rhs) DCFS_RET_CHECK_OP_(eq, ==, lhs, rhs)
#define RET_CHECK_NE(lhs, rhs) DCFS_RET_CHECK_OP_(ne, !=, lhs, rhs)
#define RET_CHECK_GT(lhs, rhs) DCFS_RET_CHECK_OP_(gt, >, lhs, rhs)
#define RET_CHECK_LT(lhs, rhs) DCFS_RET_CHECK_OP_(lt, <, lhs, rhs)
#define RET_CHECK_GE(lhs, rhs) DCFS_RET_CHECK_OP_(ge, >=, lhs, rhs)
#define RET_CHECK_LE(lhs, rhs) DCFS_RET_CHECK_OP_(le, <=, lhs, rhs)

#define RET_CHECK_OK(status_expr)                                          \
  DCFS_RET_CHECK_ELSE_BLOCKER_                                              \
  if (absl::Status dcfs_rc_status_ = (status_expr);                        \
      ABSL_PREDICT_TRUE(dcfs_rc_status_.ok())) {                           \
  } else /* NOLINT */                                                      \
    return ::dcfs::internal::RetCheckFailStatus(                          \
        #status_expr, dcfs_rc_status_)

#endif  // DCFS_RET_CHECK_H_
