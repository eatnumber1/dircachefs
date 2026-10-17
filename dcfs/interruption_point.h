// SPDX-License-Identifier: MIT
#ifndef DCFS_INTERRUPTION_POINT_H_
#define DCFS_INTERRUPTION_POINT_H_

// An interruption point (docs/design.md, "Cancellation"; formal/dcfs.tla's
// Interrupt): OK, or, if the request being served was interrupted
// (Context::interrupts), ProtocolEvents::Interrupted and EINTR naming
// `where`, which the request replies. Called only just before a backing
// syscall, where stopping leaves the cache sound: before a lookup's probe
// or a directory's population, between a population's probe batches
// (nothing of it is committed), before a mutation's phase-2 syscall (the
// caller Ends the mutation: phase 1's unknown records stay, the backing
// filesystem is unchanged), before a cold open, before an fsync or a
// request's sync point. Never between a mutation's syscall and its phase 3.

#include <string_view>

#include "absl/status/status.h"
#include "dcfs/context.h"

namespace dcfs {

absl::Status CheckInterrupted(Context &ctx, std::string_view where);

}  // namespace dcfs

#endif  // DCFS_INTERRUPTION_POINT_H_
