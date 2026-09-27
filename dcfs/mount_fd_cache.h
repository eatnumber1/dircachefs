#ifndef DCFS_MOUNT_FD_CACHE_H_
#define DCFS_MOUNT_FD_CACHE_H_

#include <memory>
#include <string_view>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "dcfs/device_uuid.h"
#include "dcfs/attributes.h"
#include "dcfs/fd.h"

namespace dcfs {

// MountFDCache is a thread-safe cache of O_PATH fds referring to mount points.
//
// Make sure to treat the FileDescriptor returned as read-only.
class MountFDCache {
 public:
  absl::StatusOr<absl_nonnull std::shared_ptr<const FileDescriptor>>
    Get(DeviceUUID uuid, std::string_view mount_target);

 private:
  absl::Mutex cache_mu_;
  absl::flat_hash_map<DeviceUUID, std::weak_ptr<const FileDescriptor>> cache_ ABSL_GUARDED_BY(cache_mu_);
};

}  // namespace dcfs

#endif  // DCFS_MOUNT_FD_CACHE_H_
