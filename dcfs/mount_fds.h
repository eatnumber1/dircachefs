#ifndef DCFS_MOUNT_FDS_H_
#define DCFS_MOUNT_FDS_H_

#include <cstddef>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"

namespace dcfs {

// MountFds holds one O_PATH fd per underlying filesystem (DeviceId),
// letting callers reach a file by handle without re-resolving a path
// through a mount point that may have moved or gone away.
//
// Not thread-safe; callers needing concurrent access must synchronize
// externally.
class MountFds {
 public:
  MountFds() = default;

  // Moveable, but not copyable: copying would require dup'ing every fd.
  MountFds(MountFds &&) = default;
  MountFds(const MountFds &) = delete;
  MountFds &operator=(MountFds &&) = default;
  MountFds &operator=(const MountFds &) = delete;

  // Adds `fd` as the mount fd for `id`. Returns AlreadyExists if `id` is
  // already present.
  absl::Status Insert(DeviceId id, FileDescriptor fd);

  // Returns the raw fd for `id`, still owned by *this. Returns NotFound
  // (with id.ToString() in the message) if no fd is registered for `id`.
  absl::StatusOr<int> Get(const DeviceId &id) const;

  // Removes the fd for `id`, if any. A no-op if `id` is not present.
  void Erase(const DeviceId &id);

  size_t size() const { return fds_.size(); }

 private:
  absl::flat_hash_map<DeviceId, FileDescriptor> fds_;
};

}  // namespace dcfs

#endif  // DCFS_MOUNT_FDS_H_
