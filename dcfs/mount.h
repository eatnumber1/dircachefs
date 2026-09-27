#ifndef DCFS_MOUNT_H_
#define DCFS_MOUNT_H_

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {

class MountedFS {
 public:
  MountedFS() = default;
  ~MountedFS();

  static absl::StatusOr<MountedFS> PerformMount(
    std::string_view source, std::string_view target,
    std::string_view filesystemtype, unsigned long mountflags = 0,
    const void *data = nullptr);

  const std::string &GetTarget() const;
  std::string Release() &&;

  absl::Status Unmount() &&;

  MountedFS(MountedFS &&);
  MountedFS(const MountedFS &) = delete;
  MountedFS &operator=(MountedFS &&);
  MountedFS &operator=(const MountedFS &) = delete;

 private:
  MountedFS(std::string target);

  // Empty string when moved-from.
  std::string target_;
};

}  // namespace dcfs

#endif  // DCFS_MOUNT_H_
