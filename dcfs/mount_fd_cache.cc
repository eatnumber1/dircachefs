#include "dcfs/mount_fd_cache.h"

#include <memory>
#include <string_view>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "dcfs/device_uuid.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"

namespace dcfs {

absl::StatusOr<std::shared_ptr<const FileDescriptor>> MountFDCache::Get(
    DeviceUUID uuid, std::string_view mount_target) {
  absl::MutexLock l(&cache_mu_);
  auto iter = cache_.find(uuid);
  if (iter != cache_.end()) {
    std::weak_ptr<const FileDescriptor> &fdp = iter->second;
    return fdp.lock();
  }

  ASSIGN_OR_RETURN(
      FileDescriptor fd,
      syscalls::open(mount_target, O_RDONLY | O_DIRECTORY));

  auto ptr = std::make_shared<const FileDescriptor>(std::move(fd));
  cache_.emplace_hint(iter, uuid, ptr);

  return ptr;
}

}  // namespace dcfs
