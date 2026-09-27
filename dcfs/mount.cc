#include "dcfs/mount.h"

#include <utility>

#include "absl/log/log.h"
#include "dcfs/syscalls.h"

namespace dcfs {

MountedFS::MountedFS(MountedFS &&o)
    : MountedFS() {
  *this = std::move(o);
}

MountedFS &MountedFS::operator=(MountedFS &&o) {
  using std::swap;
  swap(target_, o.target_);
  return *this;
}

MountedFS::~MountedFS() {
  if (target_.empty()) return;

  if (absl::Status st = std::move(*this).Unmount(); !st.ok()) {
    LOG(ERROR) << st;
  }
}

MountedFS::MountedFS(std::string target) : target_(std::move(target)) {}

absl::StatusOr<MountedFS> MountedFS::PerformMount(
    std::string_view source, std::string target,
    std::string_view filesystemtype, unsigned long mountflags,
    const void *data) {
  RETURN_IF_ERROR(
      syscalls::mount(source, target, filesystemtype, mountflags, data));
  return MountedFS(std::move(target));
}

absl::Status MountedFS::Unmount() && {
  RETURN_IF_ERROR(syscalls::umount(target_));
  target_ = "";
}

const std::string &MountedFS::GetTarget() const {
  return target_;
}

std::string MountedFS::Release() && {
  std::string ret = std::move(target_);
  // Put target_ back into a valid empty state.
  target_ = "";
  return ret;
}

}  // namespace dcfs
