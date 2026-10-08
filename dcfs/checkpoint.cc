#include "dcfs/checkpoint.h"

#include <cerrno>
#include <string_view>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/protocol_events.h"
#include "dcfs/status.h"

namespace dcfs {

absl::Status Checkpoint(Context &ctx, std::string_view where) {
  if (!ctx.interrupts->Interrupted()) return absl::OkStatus();
  // Model: Interrupt.
  ctx.events->Interrupted(ctx);
  return ErrnoToStatus(EINTR, absl::StrCat("Interrupted before ", where));
}

}  // namespace dcfs
