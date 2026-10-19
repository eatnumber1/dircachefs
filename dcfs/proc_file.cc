// SPDX-License-Identifier: MIT
#include "dcfs/proc_file.h"

#include <fcntl.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs {

absl::StatusOr<std::string> ReadProcFile(std::string_view path) {
  // procfs is not the backing filesystem, so the open is syscalls.h's, which
  // counts as no backing syscall. read(2) has one wrapper only,
  // syscalls_backing.h's.
  ASSIGN_OR_RETURN(FileDescriptor fd, syscalls::open(path, O_RDONLY));
  std::string contents;
  char buf[65536];
  while (true) {
    ASSIGN_OR_RETURN(size_t n, syscalls::read(*fd, buf, sizeof(buf)));
    if (n == 0) return contents;
    contents.append(buf, n);
  }
}

absl::StatusOr<std::string> ReadMountinfo() {
  return ReadProcFile("/proc/self/mountinfo");
}

}  // namespace dcfs
