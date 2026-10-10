#ifndef DCFS_MOUNT_OPTIONS_H_
#define DCFS_MOUNT_OPTIONS_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace dcfs {

// The mount option dcfs requires: the kernel checks permissions itself,
// from the attributes dcfs reported (docs/design.md, "Caller credentials").
inline constexpr std::string_view kDefaultPermissions = "default_permissions";
// The one that lets users other than the mounter (root) reach the mount.
inline constexpr std::string_view kAllowOther = "allow_other";

// The mount options dcfs passes to libfuse as one "-o a,b,c".
struct MountOptions {
  // default_permissions, allow_other, then --fuse_opt's.
  std::vector<std::string> options;
  // A "max_read=N" among them (DirCacheFS::Options::max_read).
  std::optional<unsigned int> max_read;
};

// Builds dcfs's mount options from --fuse_opt (each element one option:
// Abseil splits the flag at commas). default_permissions and allow_other are
// always there (step 15.8: the kernel enforces the mode bits, and every user
// may reach the mount). InvalidArgument for a --fuse_opt element naming
// either, which dcfs always adds itself.
absl::StatusOr<MountOptions> BuildMountOptions(
    std::span<const std::string> fuse_opt);

// Whether `options` (each element one or more comma-separated options)
// has default_permissions.
bool HasDefaultPermissions(std::span<const std::string> options);

}  // namespace dcfs

#endif  // DCFS_MOUNT_OPTIONS_H_
