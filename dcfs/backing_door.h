// SPDX-License-Identifier: MIT
// The backing filesystem's door, and its poisoned state (docs/style.md 1.6b,
// the poisoned subsystem). Every syscall that reaches the backing filesystem
// is made by a wrapper of syscalls_backing.h, and each wrapper passes
// BackingDoor() before its syscall. A credential switch that cannot put
// root's filesystem credentials back (backing.cc's RestoreRoot) poisons the
// door: from then on every wrapper refuses, whatever a caller does with any
// Status, and SessionLoop serves no further request, so the daemon shuts down
// in order and exits nonzero.
//
// A library of its own, apart from //dcfs:syscalls_backing, so that
// SessionLoop can ask without being able to reach the backing filesystem
// (//tools:syscalls_backing_users_test).
#ifndef DCFS_BACKING_DOOR_H_
#define DCFS_BACKING_DOOR_H_

#include "absl/status/status.h"

namespace dcfs {

// Poisons the door for the rest of the process's life.
void PoisonBacking();

// OK, or once the door is poisoned the refusal every backing syscall wrapper
// returns (EIO, an error dcfs produced).
absl::Status BackingDoor();

}  // namespace dcfs

#endif  // DCFS_BACKING_DOOR_H_
