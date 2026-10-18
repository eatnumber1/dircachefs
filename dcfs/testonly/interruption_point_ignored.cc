// SPDX-License-Identifier: MIT
// A fault for trace validation (formal/README.md, "Trace validation"; step
// 22.5): an interruption point that sees the request's interrupt (and
// reports it, ProtocolEvents::Interrupted) but whose caller carries on to its
// backing syscall as if it had not. Linked, with -Wl,--wrap of
// dcfs::CheckInterrupted (its mangled name, see dcfs/BUILD.bazel), only into
// the two fault builds of dcfs/BUILD.bazel's DIR_CACHE_FS_FAULTS that use it:
// //dcfs:dir_cache_fs_fault_interruption_point_ignored_test (an unlink's
// syscall) and ..._interruption_point_ignored_population_test (a readdir's
// population), whose trace validation must reject the trace there.

#include "absl/status/status.h"
#include "dcfs/context.h"
#include "dcfs/interruption_point.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::Status RealCheckInterrupted(Context &ctx, InterruptionPoint point) asm(
    "__real__ZN4dcfs16CheckInterruptedERNS_7ContextENS_17InterruptionPointE");

// NOLINTBEGIN(misc-use-internal-linkage): the linker resolves this name, so it
// must be external
absl::Status WrapCheckInterrupted(Context &ctx, InterruptionPoint point) asm(
    "__wrap__ZN4dcfs16CheckInterruptedERNS_7ContextENS_17InterruptionPointE");
// NOLINTEND(misc-use-internal-linkage)
absl::Status WrapCheckInterrupted(Context &ctx, InterruptionPoint point) {
  // The interrupt it saw, which the caller then ignores.
  RealCheckInterrupted(ctx, point).IgnoreError();
  return absl::OkStatus();
}

}  // namespace dcfs::testonly
