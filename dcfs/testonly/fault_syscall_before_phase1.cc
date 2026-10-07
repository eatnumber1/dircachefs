// A fault for trace validation (formal/README.md, "Trace validation"): an
// unlink whose backing unlinkat runs before its phase 1, as if the syscall
// (and its protocol event, which follows it) had been moved above
// cache::BeginRemove in DirCacheFS::RemoveChild. Linked, with
// -Wl,--wrap of BeginRemove and backing::UnlinkAt (their mangled names, see
// dcfs/BUILD.bazel), only into //dcfs:dir_cache_fs_fault_syscall_before_phase1_test,
// whose trace validation must reject the trace at that syscall.

#include <cstdint>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/credentials.h"
#include "dcfs/metadata_cache.h"

namespace dcfs::testonly {

using cache::FillSnapshot;
using cache::InodeId;
using cache::Mutation;

// The wrapped functions (external linkage: the linker resolves these names).

absl::StatusOr<Mutation> RealBeginRemove(Context &ctx, InodeId parent,
                                         std::string_view name, InodeId child,
                                         FillSnapshot resolved)
    asm("__real__ZN4dcfs5cache11BeginRemoveERNS_7ContextElNSt3__117basic_string_viewIcNS3_11char_traitsIcEEEElNS0_12FillSnapshotE");
absl::Status RealUnlinkAt(Context &ctx, const Credentials &caller,
                          InodeId parent, std::string_view name, int flags)
    asm("__real__ZN4dcfs7backing8UnlinkAtERNS_7ContextERKNS_11CredentialsElNSt3__117basic_string_viewIcNS6_11char_traitsIcEEEEi");


// The unlinkat, moved before phase 1 with its event; then phase 1.
absl::StatusOr<Mutation> WrapBeginRemove(Context &ctx, InodeId parent,
                                         std::string_view name, InodeId child,
                                         FillSnapshot resolved)
    asm("__wrap__ZN4dcfs5cache11BeginRemoveERNS_7ContextElNSt3__117basic_string_viewIcNS3_11char_traitsIcEEEElNS0_12FillSnapshotE");
absl::StatusOr<Mutation> WrapBeginRemove(Context &ctx, InodeId parent,
                                         std::string_view name, InodeId child,
                                         FillSnapshot resolved) {
  ctx.events->MutationSyscallStarting(ctx);
  const absl::Status unlinked =
      RealUnlinkAt(ctx, Credentials{.uid = 0, .gid = 0}, parent, name, 0);
  ctx.events->MutationSyscall(ctx, unlinked);
  return RealBeginRemove(ctx, parent, name, child, resolved);
}

// Where the syscall used to be: nothing left to do.
absl::Status WrapUnlinkAt(Context &ctx, const Credentials &caller,
                          InodeId parent, std::string_view name, int flags)
    asm("__wrap__ZN4dcfs7backing8UnlinkAtERNS_7ContextERKNS_11CredentialsElNSt3__117basic_string_viewIcNS6_11char_traitsIcEEEEi");
absl::Status WrapUnlinkAt(Context &ctx, const Credentials &caller,
                          InodeId parent, std::string_view name, int flags) {
  return absl::OkStatus();
}

}  // namespace dcfs::testonly
