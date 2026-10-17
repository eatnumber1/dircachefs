// SPDX-License-Identifier: MIT
// A fault for trace validation (formal/README.md, "Trace validation"; step
// 22.5): a checkpoint that sees the request's interrupt (and reports it,
// ProtocolEvents::Interrupted) but whose caller carries on to its backing
// syscall as if it had not. Linked, with -Wl,--wrap of dcfs::Checkpoint
// (its mangled name, see dcfs/BUILD.bazel), only into
// //dcfs:dir_cache_fs_fault_checkpoint_ignored_test, whose trace validation
// must reject the trace at that syscall.

#include <string_view>

#include "absl/status/status.h"
#include "dcfs/context.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::Status RealCheckpoint(Context &ctx, std::string_view where) asm(
    "__real__ZN4dcfs10CheckpointERNS_7ContextENSt3__117basic_string_viewIcNS2_"
    "11char_traitsIcEEEE");

// NOLINTBEGIN(misc-use-internal-linkage): the linker resolves this name, so it
// must be external
absl::Status WrapCheckpoint(Context &ctx, std::string_view where) asm(
    "__wrap__ZN4dcfs10CheckpointERNS_7ContextENSt3__117basic_string_viewIcNS2_"
    "11char_traitsIcEEEE");
// NOLINTEND(misc-use-internal-linkage)
absl::Status WrapCheckpoint(Context &ctx, std::string_view where) {
  // The interrupt it saw, which the caller then ignores.
  RealCheckpoint(ctx, where).IgnoreError();
  return absl::OkStatus();
}

}  // namespace dcfs::testonly
