// SPDX-License-Identifier: MIT
#include "dcfs/remount.h"

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "dcfs/escape.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/proc_file.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs {

absl::Status RemountDcfs(std::string_view mountpoint, bool read_only) {
  ASSIGN_OR_RETURN(std::string canonical, syscalls::realpath(mountpoint));
  ASSIGN_OR_RETURN(std::string contents, ReadMountinfo());
  std::optional<unsigned long> flags =
      RemountFlags(contents, canonical, read_only);
  if (!flags.has_value()) {
    return FailedPreconditionErrorBuilder()
           << "Cannot remount " << EscapeBytes(canonical)
           << ": it is not a dcfs mount";
  }
  return syscalls::mount(nullptr, canonical, nullptr, *flags, nullptr);
}

}  // namespace dcfs
