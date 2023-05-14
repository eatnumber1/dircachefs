#include "dfs/syscalls.h"

#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/mount.h>

namespace dfs {
namespace syscalls {

absl::Status close(dfs::FileDescriptor fd) {
  // Failure of close is not recoverable... we must leak the fd.
  int fd_i = std::move(fd).Release();
  errno = 0;
  ::close(fd_i);
  return absl::ErrnoToStatus(errno, absl::StrCat("close(", fd_i, ")"));
}

absl::StatusOr<dfs::FileDescriptor> open(
    const char *pathname, int flags, mode_t mode) {
  int fd = ::open(pathname, flags, mode);
  if (fd == -1) {
    return absl::ErrnoToStatus(errno, absl::StrCat("open(", pathname, ")"));
  }
  return dfs::FileDescriptor(fd);
}

absl::StatusOr<size_t> read(int fd, void *buf, size_t count) {
  ssize_t nb = ::read(fd, buf, count);
  if (nb == -1) {
    return absl::ErrnoToStatus(errno, absl::StrCat("read(", fd, ")"));
  }
  return nb;
}

absl::StatusOr<dfs::Mount> mount(
    const char *source, std::string target, const char *filesystemtype,
    unsigned long mountflags, const void *data) {
  if (::mount(source, target.c_str(), filesystemtype, mountflags, data) == -1) {
    return absl::ErrnoToStatus(errno, "mount");
  }
  return Mount(std::move(target));
}

absl::StatusOr<struct stat> stat(const char *pathname) {
  struct stat statbuf;
  if (::stat(pathname, &statbuf) == -1) {
    return absl::ErrnoToStatus(errno, absl::StrCat("stat(", pathname, ")"));
  }
  return statbuf;
}

absl::Status umount(dfs::Mount mount, int flags) {
  if (const std::string &target = mount.GetTarget();
      ::umount2(target.c_str(), flags) == -1) {
    return absl::ErrnoToStatus(errno, absl::StrCat("umount2(", target, ")"));
  }
  std::move(mount).Release();
  return absl::OkStatus();
}

}  // namespace syscalls
}  // namespace dfs
