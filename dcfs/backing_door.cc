// SPDX-License-Identifier: MIT
#include "dcfs/backing_door.h"

#include <cerrno>

#include "absl/status/status.h"
#include "dcfs/status.h"

namespace dcfs {
namespace {

// The process's one piece of state outside a Context (russ, 2026-10-11: a
// process-wide poisoned flag at the backing's door). A plain bool: dcfs is
// single-threaded (tools/banned_symbols.txt bans pthread_create), so the
// thread that poisons is the one that asks.
bool poisoned = false;

}  // namespace

void PoisonBacking() { poisoned = true; }

absl::Status BackingDoor() {
  if (!poisoned) return absl::OkStatus();
  return ProducedErrnoToStatus(
      EIO,
      "The backing filesystem is poisoned: root's filesystem credentials could "
      "not be restored after a request");
}

}  // namespace dcfs
