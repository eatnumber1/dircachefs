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
// from the attributes dcfs reported (docs/design.md, "Permissions").
inline constexpr std::string_view kDefaultPermissions = "default_permissions";

// The mount options dcfs passes to libfuse as one "-o a,b,c".
struct MountOptions {
  // default_permissions, then allow_other if asked for, then --fuse_opt's.
  std::vector<std::string> options;
  // A "max_read=N" among them (DirCacheFS::Options::max_read).
  std::optional<unsigned int> max_read;
};

// Builds dcfs's mount options from --allow_other and --fuse_opt (each
// element one option: Abseil splits the flag at commas). InvalidArgument
// for a --fuse_opt element naming default_permissions, which dcfs always
// adds itself.
absl::StatusOr<MountOptions> BuildMountOptions(
    bool allow_other, std::span<const std::string> fuse_opt);

// Whether `options` (each element one or more comma-separated options)
// has default_permissions.
bool HasDefaultPermissions(std::span<const std::string> options);

}  // namespace dcfs

#endif  // DCFS_MOUNT_OPTIONS_H_
