#ifndef DCFS_TESTONLY_FILES_H_
#define DCFS_TESTONLY_FILES_H_

#include <fcntl.h>

#include <string>
#include <string_view>

#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"

namespace dcfs::testonly {

// The whole contents of the file at `path`, read through the syscalls::
// wrappers (for /proc files and small test files).
inline absl::StatusOr<std::string> ReadFileToString(std::string_view path) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        syscalls::openat(AT_FDCWD, path, O_RDONLY));
  std::string contents;
  char buf[4096];
  while (true) {
    ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::read(*fd, buf, sizeof(buf)));
    if (n == 0) return contents;
    contents.append(buf, n);
  }
}

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_FILES_H_
