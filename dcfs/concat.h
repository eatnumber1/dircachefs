// SPDX-License-Identifier: MIT
#ifndef DCFS_CONCAT_H_
#define DCFS_CONCAT_H_

// Pastes two tokens into one name (docs/style.md rule-macros-discouraged: a
// function cannot generate an identifier). Two levels, because ## pastes
// before it expands its arguments, so `CONCAT(x_, __LINE__)` needs __LINE__
// expanded first, by the outer macro:
//
//   int CONCAT(value_, __LINE__) = 0;  // value_42
#define DCFS_CONCAT_INNER(x, y) x##y
#define CONCAT(x, y) DCFS_CONCAT_INNER(x, y)

#endif  // DCFS_CONCAT_H_
