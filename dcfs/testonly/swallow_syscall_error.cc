// A fault for trace validation (formal/README.md, "Trace validation"): an
// unlink whose unlinkat reports an error the model does not have (EBUSY),
// which the code then ignores, carrying on as if the syscall had succeeded
// and replying OK. (The real unlinkat runs and succeeds; the fault is the
// error it reported and the request swallowed.) Linked, with -Wl,--wrap of
// backing::UnlinkAt (its mangled name, see dcfs/BUILD.bazel), only into
// //dcfs:dir_cache_fs_fault_swallow_syscall_error_test, whose trace
// validation must reject the trace.

#include <cerrno>
#include <string_view>

#include "absl/status/status.h"
#include "dcfs/context.h"
#include "dcfs/credentials.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/status.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::Status
RealUnlinkAt(Context &ctx, const Credentials &caller, cache::InodeId parent, std::string_view name, int flags) asm(
    "__real__ZN4dcfs7backing8UnlinkAtERNS_7ContextERKNS_11CredentialsElNSt3__"
    "117basic_string_viewIcNS6_11char_traitsIcEEEEi");

absl::Status
WrapUnlinkAt(Context &ctx, const Credentials &caller, cache::InodeId parent, std::string_view name, int flags) asm(
    "__wrap__ZN4dcfs7backing8UnlinkAtERNS_7ContextERKNS_11CredentialsElNSt3__"
    "117basic_string_viewIcNS6_11char_traitsIcEEEEi");
absl::Status WrapUnlinkAt(Context &ctx, const Credentials &caller,
                          cache::InodeId parent, std::string_view name,
                          int flags) {
  absl::Status status = RealUnlinkAt(ctx, caller, parent, name, flags);
  // The error the syscall reported, which the request then ignores.
  ctx.events->MutationSyscall(ctx, ErrnoToStatus(EBUSY, "unlinkat"));
  return status;
}

}  // namespace dcfs::testonly
