// A fault for trace validation (formal/README.md, "Trace validation"): a
// sync point whose snapshot (cache::BeginSync) is taken after its syncfs
// calls, as if BeginSync had been moved below the syncfs loop in
// backing::SyncBacking, with the protocol events that mark each (the
// syncfs calls' start, which precedes them; the snapshot's, which follows
// BeginSync and stays in SyncBacking). Linked, with -Wl,--wrap of BeginSync
// (its mangled name, see dcfs/BUILD.bazel), only into
// //dcfs:dir_cache_fs_fault_snapshot_after_syncfs_test, whose trace
// validation must reject the trace.

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/syscalls.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::StatusOr<cache::SyncSnapshot> RealBeginSync(Context &ctx)
    asm("__real__ZN4dcfs5cache9BeginSyncERNS_7ContextE");

absl::StatusOr<cache::SyncSnapshot> WrapBeginSync(Context &ctx)
    asm("__wrap__ZN4dcfs5cache9BeginSyncERNS_7ContextE");
absl::StatusOr<cache::SyncSnapshot> WrapBeginSync(Context &ctx) {
  // The syncfs calls, moved above the snapshot with their event. (The
  // calls SyncBacking still makes afterwards find nothing more to write.)
  ctx.events->SyncfsStarting(ctx);
  for (int fd : ctx.mounts.Fds()) {
    syscalls::syncfs(fd).IgnoreError();
  }
  return RealBeginSync(ctx);
}

}  // namespace dcfs::testonly
