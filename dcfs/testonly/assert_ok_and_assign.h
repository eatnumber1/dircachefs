// SPDX-License-Identifier: MIT
#ifndef DCFS_TESTONLY_ASSERT_OK_AND_ASSIGN_H_
#define DCFS_TESTONLY_ASSERT_OK_AND_ASSIGN_H_

#include <utility>

#include "absl/status/status_matchers.h"
#include "dcfs/concat.h"
#include "gmock/gmock.h"

// The pinned Abseil's status_matchers.h has no ASSERT_OK_AND_ASSIGN, so this
// is the one definition for every test:
//
//   ASSERT_OK_AND_ASSIGN(int fd, OpenSomething());
//
// Fails the test (ASSERT_THAT(..., IsOk())) and returns from the test body
// when the StatusOr is not OK; otherwise declares or assigns `lhs`.
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                                   \
  DCFS_ASSERT_OK_AND_ASSIGN_IMPL(CONCAT(_status_or_value_, __LINE__), lhs, \
                                 rexpr)
#define DCFS_ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                                   \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());             \
  lhs = std::move(statusor).value()

#endif  // DCFS_TESTONLY_ASSERT_OK_AND_ASSIGN_H_
