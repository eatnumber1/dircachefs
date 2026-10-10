// A fault for trace validation (formal/README.md, "Trace validation"): a
// create whose phase-2 syscall is started before its phase 1, as if the
// syscall (with MutationSyscallStarting, which precedes it) had been moved
// above cache::BeginCreate in DirCacheFS::CreateChild. (The syscall itself
// stays where it is: the event is what the recorder judges.) Linked, with
// -Wl,--wrap of BeginCreate (its mangled name, see dcfs/BUILD.bazel), only
// into //dcfs:dir_cache_fs_fault_create_syscall_before_phase1_test, whose
// scenario runs the create inside a readdir's population of the same
// directory: the trace must be rejected although its lines are held there.

#include <string_view>

#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/metadata_cache.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::StatusOr<cache::Mutation>
RealBeginCreate(Context &ctx, cache::InodeId parent, std::string_view name) asm(
    "__real__ZN4dcfs5cache11BeginCreateERNS_7ContextElNSt3__117basic_string_"
    "viewIcNS3_11char_traitsIcEEEE");

absl::StatusOr<cache::Mutation>
WrapBeginCreate(Context &ctx, cache::InodeId parent, std::string_view name) asm(
    "__wrap__ZN4dcfs5cache11BeginCreateERNS_7ContextElNSt3__117basic_string_"
    "viewIcNS3_11char_traitsIcEEEE");
absl::StatusOr<cache::Mutation> WrapBeginCreate(Context &ctx,
                                                cache::InodeId parent,
                                                std::string_view name) {
  ctx.events->MutationSyscallStarting(ctx);
  return RealBeginCreate(ctx, parent, name);
}

}  // namespace dcfs::testonly
