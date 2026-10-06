// A fault for trace validation (formal/README.md, "Trace validation"): an
// unlink whose phase 3 (the name absent) and Mutation::End run right after
// its phase 1, before its backing unlinkat, as if they had been moved there
// in DirCacheFS::RemoveChild. Linked, with -Wl,--wrap of cache::BeginRemove
// (its mangled name, see dcfs/BUILD.bazel), only into
// //dcfs:dir_cache_fs_fault_phase3_before_syscall_test, whose trace
// validation must reject the trace.

#include <cstdint>
#include <string_view>

#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/metadata_cache.h"

namespace dcfs::testonly {

using cache::FillSnapshot;
using cache::InodeId;
using cache::Mutation;

// The wrapped functions (external linkage: the linker resolves these names).

absl::StatusOr<Mutation> RealBeginRemove(Context &ctx, InodeId parent,
                                         std::string_view name, InodeId child,
                                         FillSnapshot resolved)
    asm("__real__ZN4dcfs5cache11BeginRemoveERNS_7ContextElSt17basic_string_viewIcSt11char_traitsIcEElNS0_12FillSnapshotE");


absl::StatusOr<Mutation> WrapBeginRemove(Context &ctx, InodeId parent,
                                         std::string_view name, InodeId child,
                                         FillSnapshot resolved)
    asm("__wrap__ZN4dcfs5cache11BeginRemoveERNS_7ContextElSt17basic_string_viewIcSt11char_traitsIcEElNS0_12FillSnapshotE");
absl::StatusOr<Mutation> WrapBeginRemove(Context &ctx, InodeId parent,
                                         std::string_view name, InodeId child,
                                         FillSnapshot resolved) {
  absl::StatusOr<Mutation> mutation =
      RealBeginRemove(ctx, parent, name, child, resolved);
  if (!mutation.ok()) return mutation;
  // Phase 3 and the end, moved before the syscall. (The caller's own phase
  // 3 then finds it no longer Owns the parent, and its End does nothing.)
  if (mutation->Owns(parent)) {
    cache::SetNegative(ctx, parent, name).IgnoreError();
  }
  mutation->End();
  return mutation;
}

}  // namespace dcfs::testonly
