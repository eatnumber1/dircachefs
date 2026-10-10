// A crash for trace validation (formal/README.md, "Trace validation"): the
// first start's recovery (cache::RecoverDirty) fails before it writes
// anything, as if the daemon had died there, so that the test starts
// again: a crash during recovery (formal/dcfs.tla's CrashRecovering, step
// 12.6). Linked, with -Wl,--wrap of RecoverDirty (its mangled name, see
// dcfs/BUILD.bazel), only into
// //dcfs:dir_cache_fs_crash_during_recovery_test, whose traces must be
// valid.

#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/context.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::StatusOr<int64_t> RealRecoverDirty(Context &ctx)
    asm("__real__ZN4dcfs5cache12RecoverDirtyERNS_7ContextE");

absl::StatusOr<int64_t> WrapRecoverDirty(Context &ctx)
    asm("__wrap__ZN4dcfs5cache12RecoverDirtyERNS_7ContextE");
absl::StatusOr<int64_t> WrapRecoverDirty(Context &ctx) {
  static bool crashed = false;
  if (!crashed) {
    crashed = true;
    return absl::AbortedError("testonly: the daemon died during recovery");
  }
  return RealRecoverDirty(ctx);
}

}  // namespace dcfs::testonly
