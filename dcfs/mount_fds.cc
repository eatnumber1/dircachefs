#include "dcfs/mount_fds.h"

#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/status.h"

namespace dcfs {

absl::Status MountFds::Insert(DeviceId id, FileDescriptor fd) {
  auto [it, inserted] = fds_.try_emplace(id, std::move(fd));
  if (!inserted) {
    return AlreadyExistsErrorBuilder()
           << "MountFds already has an fd for " << id.ToString();
  }
  return absl::OkStatus();
}

absl::StatusOr<int> MountFds::Get(const DeviceId &id) const {
  auto it = fds_.find(id);
  if (it == fds_.end()) {
    return NotFoundErrorBuilder() << "No mount fd for " << id.ToString();
  }
  return *it->second;
}

void MountFds::Erase(const DeviceId &id) { fds_.erase(id); }

std::vector<int> MountFds::Fds() const {
  std::vector<int> fds;
  fds.reserve(fds_.size());
  for (const auto &[id, fd] : fds_) fds.push_back(*fd);
  return fds;
}

}  // namespace dcfs
