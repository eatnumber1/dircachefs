// A crash for trace validation and step 12.6b: the start's first probe of
// a recovered row (backing::BackingNlink, from ProbeRecoveredRows) fails, as
// if the daemon had died before probing it: the row must stay in the dirty
// set for the next start. Linked, with -Wl,--wrap of BackingNlink (its
// mangled name, see dcfs/BUILD.bazel), only into
// //dcfs:dir_cache_fs_crash_during_recovery_test.

#include <cstdint>
#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/metadata_cache.h"

namespace dcfs::testonly {

// The wrapped function (external linkage: the linker resolves this name).
absl::StatusOr<std::optional<uint64_t>> RealBackingNlink(Context &ctx,
                                                         cache::InodeId id)
    asm("__real__ZN4dcfs7backing12BackingNlinkERNS_7ContextEl");

absl::StatusOr<std::optional<uint64_t>> WrapBackingNlink(Context &ctx,
                                                         cache::InodeId id)
    asm("__wrap__ZN4dcfs7backing12BackingNlinkERNS_7ContextEl");
absl::StatusOr<std::optional<uint64_t>> WrapBackingNlink(Context &ctx,
                                                         cache::InodeId id) {
  static bool crashed = false;
  if (!crashed) {
    crashed = true;
    return absl::UnavailableError(
        "testonly: the daemon died before probing this row");
  }
  return RealBackingNlink(ctx, id);
}

}  // namespace dcfs::testonly
