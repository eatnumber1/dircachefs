// SPDX-License-Identifier: MIT
#include "dcfs/interruption_point.h"

#include <cerrno>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/protocol_events.h"
#include "dcfs/status.h"

namespace dcfs {

absl::Status CheckInterrupted(Context &ctx, InterruptionPoint point) {
  if (!ctx.interrupts->Interrupted()) return absl::OkStatus();
  // Model: Interrupt.
  ctx.events->Interrupted(ctx);
  return ErrnoToStatus(EINTR, absl::StrCat("Interrupted before ", point));
}

}  // namespace dcfs
