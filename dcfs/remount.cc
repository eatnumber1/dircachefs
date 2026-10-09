#include "dcfs/remount.h"

#include <fcntl.h>

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs {

absl::Status RemountDcfs(std::string_view mountpoint, bool read_only) {
  ABSL_ASSIGN_OR_RETURN(std::string canonical, syscalls::realpath(mountpoint));
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor mountinfo,
      syscalls::openat(AT_FDCWD, "/proc/self/mountinfo", O_RDONLY));
  std::string contents;
  char buf[65536];
  while (true) {
    ABSL_ASSIGN_OR_RETURN(size_t n,
                          syscalls::read(*mountinfo, buf, sizeof(buf)));
    if (n == 0) break;
    contents.append(buf, n);
  }
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
