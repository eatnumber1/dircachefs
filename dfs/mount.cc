#include "dfs/mount.h"

#include <utility>

#include "dfs/syscalls.h"
#include "absl/log/log.h"

namespace dfs {

Mount::Mount(Mount &&o)
    : Mount() {
  *this = std::move(o);
}

Mount &Mount::operator=(Mount &&o) {
  using std::swap;
  swap(target_, o.target_);
  return *this;
}

Mount::~Mount() {
  if (target_.empty()) return;

  if (absl::Status st = syscalls::umount(std::move(*this));
      !st.ok()) {
    LOG(ERROR) << st;
  }
}

Mount::Mount(std::string target) : target_(std::move(target)) {}

const std::string &Mount::GetTarget() const {
  return target_;
}

std::string Mount::Release() && {
  std::string ret = std::move(target_);
  // Put target_ back into a valid empty state.
  target_ = "";
  return ret;
}

}  // namespace dfs
