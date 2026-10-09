#ifndef DCFS_REMOUNT_H_
#define DCFS_REMOUNT_H_

// `mount -o remount` of a dcfs mount (decision 10): changes only the dcfs
// mount's read-only flag; the underlying mount is not reachable from here.

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {

// The text of /proc/self/mountinfo.
[[nodiscard]] absl::StatusOr<std::string> ReadMountinfo();

// Remounts the dcfs mount at `mountpoint` read-only or read-write, keeping
// the per-mount flags it has. NotFound (with the errno) if the mount point
// does not resolve, FailedPrecondition if what is mounted there is not a
// fuse.dcfs mount (a remount would change another filesystem's flags).
[[nodiscard]] absl::Status RemountDcfs(std::string_view mountpoint,
                                       bool read_only);

}  // namespace dcfs

#endif  // DCFS_REMOUNT_H_
