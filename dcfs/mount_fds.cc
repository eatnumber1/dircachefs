#include "dcfs/mount_fds.h"

#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/device_id.h"

namespace dcfs {

absl::Status MountFds::Insert(DeviceId id, FileDescriptor fd) {
  auto [it, inserted] = fds_.try_emplace(id, std::move(fd));
  if (!inserted) {
    return absl::AlreadyExistsError(
        absl::StrCat("MountFds already has an fd for ", id.ToString()));
  }
  return absl::OkStatus();
}

absl::StatusOr<int> MountFds::Get(const DeviceId &id) const {
  auto it = fds_.find(id);
  if (it == fds_.end()) {
    return absl::NotFoundError(
        absl::StrCat("No mount fd for ", id.ToString()));
  }
  return *it->second;
}

void MountFds::Erase(const DeviceId &id) { fds_.erase(id); }

}  // namespace dcfs
