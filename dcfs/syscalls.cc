#include "dcfs/syscalls.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>

#include "absl/log/log.h"
#include "absl/status/status_macros.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/status.h"
#include "dcfs/ret_check.h"

namespace dcfs {
namespace syscalls {

absl::Status close(FileDescriptor fd) {
  // Failure of close is not recoverable... we must leak the fd.
  int fd_i = std::move(fd).Release();
  errno = 0;
  ::close(fd_i);
  return ErrnoToStatus(errno, absl::StrCat("close(", fd_i, ")"));
}

absl::StatusOr<FileDescriptor> openat(
    int dirfd, std::string_view pathname, int flags, mode_t mode) {
  int fd = ::openat(
      dirfd, std::string(pathname).c_str(), flags | O_CLOEXEC, mode);
  if (fd == -1) {
    return ErrnoToStatus(errno, absl::StrFormat("openat(%d, %s)", dirfd, pathname));
  }
  return FileDescriptor(fd);
}

absl::StatusOr<size_t> read(int fd, void *buf, size_t count) {
  ssize_t nb = ::read(fd, buf, count);
  if (nb == -1) {
    return ErrnoToStatus(errno, absl::StrCat("read(", fd, ")"));
  }
  return nb;
}

absl::StatusOr<struct statvfs> fstatvfs(int fd) {
  struct statvfs buf;
  int ret = ::fstatvfs(fd, &buf);
  if (ret == -1) return ErrnoToStatus(errno, "fstatvfs");
  return buf;
}

absl::StatusOr<struct stat> fstat(int fd) {
  struct stat buf;
  int ret = ::fstat(fd, &buf);
  if (ret == -1) return ErrnoToStatus(errno, "fstat");
  return buf;
}

absl::StatusOr<struct stat> fstatat(
    int dirfd, std::string_view pathname, int flags) {
  struct stat buf;
  int ret = ::fstatat(dirfd, std::string(pathname).c_str(), &buf, flags);
  if (ret == -1) return ErrnoToStatus(errno, "fstatat");
  return buf;
}

absl::StatusOr<FileDescriptor> openat2(
    int dirfd, std::string_view pathname, open_how how, size_t size) {
  how.flags |= O_CLOEXEC;
  long fd = syscall(
      SYS_openat2, dirfd, std::string(pathname).c_str(), &how, size);
  if (fd == -1) return ErrnoToStatus(errno, "openat2");
  return FileDescriptor(fd);
}

absl::Status name_to_handle_at(
    int dirfd, std::string_view pathname, file_handle &handle,
    int &mount_id, int flags) {
  int rc = ::name_to_handle_at(
      dirfd, std::string(pathname).c_str(), &handle, &mount_id, flags);
  if (rc != 0) {
    return ErrnoToStatus(
        errno, absl::StrFormat("name_to_handle_at(%d, %s)", dirfd, pathname));
  }
  return absl::OkStatus();
}

absl::StatusOr<FileDescriptor> open_by_handle_at(
    int mount_fd, const file_handle &handle, int flags) {
  int fd = ::open_by_handle_at(
      mount_fd, const_cast<file_handle *>(&handle), flags | O_CLOEXEC);
  if (fd == -1) return ErrnoToStatus(errno, "open_by_handle_at");
  return FileDescriptor(fd);
}


absl::StatusOr<ssize_t> getdents64(int fd, void *dirp, size_t count) {
  ssize_t nb = ::syscall(SYS_getdents64, fd, dirp, count);
  if (nb < 0) return ErrnoToStatus(errno, "getdents64");
  return nb;
}

absl::StatusOr<off_t> lseek(int fd, off_t offset, int whence) {
  off_t rc = ::lseek(fd, offset, whence);
  if (rc == static_cast<off_t>(-1)) return ErrnoToStatus(errno, "lseek");
  return rc;
}

absl::StatusOr<struct statx> statx(int dirfd, std::string_view path, int flags,
                                    unsigned int mask) {
  struct statx buf;
  int ret = ::statx(dirfd, std::string(path).c_str(), flags, mask, &buf);
  if (ret == -1) return ErrnoToStatus(errno, "statx");
  return buf;
}

absl::StatusOr<struct statfs> fstatfs(int fd) {
  struct statfs buf;
  int ret = ::fstatfs(fd, &buf);
  if (ret == -1) return ErrnoToStatus(errno, "fstatfs");
  return buf;
}

absl::StatusOr<std::string> readlinkat(int dirfd, std::string_view path) {
  std::string result;
  size_t bufsize = 256;
  while (true) {
    result.resize(bufsize);
    ssize_t nbytes = ::readlinkat(dirfd, std::string(path).c_str(),
                                   result.data(), result.size());
    if (nbytes == -1) {
      return ErrnoToStatus(errno, "readlinkat");
    }
    if (static_cast<size_t>(nbytes) < bufsize) {
      result.resize(nbytes);
      return result;
    }
    // Buffer too small, double it (cap at PATH_MAX*4)
    if (bufsize >= PATH_MAX * 4) {
      result.resize(nbytes);
      return result;
    }
    bufsize *= 2;
  }
}

absl::StatusOr<std::string> fgetxattr(int fd, std::string_view name) {
  // Query size with a null buffer first
  ssize_t size = ::fgetxattr(fd, std::string(name).c_str(), nullptr, 0);
  if (size == -1) {
    int err = errno;
    if (err == ERANGE) {
      // Size changed, retry once with a larger buffer
      size = ::fgetxattr(fd, std::string(name).c_str(), nullptr, 0);
      if (size == -1) {
        return ErrnoToStatus(errno, "fgetxattr");
      }
    } else {
      return ErrnoToStatus(err, "fgetxattr");
    }
  }
  if (size == 0) {
    return std::string();
  }
  // Now read the actual value
  std::string result(size, '\0');
  ssize_t nbytes = ::fgetxattr(fd, std::string(name).c_str(), result.data(),
                               result.size());
  if (nbytes == -1) {
    return ErrnoToStatus(errno, "fgetxattr");
  }
  result.resize(nbytes);
  return result;
}

absl::StatusOr<std::vector<std::string>> flistxattr(int fd) {
  // Query size with a null buffer first
  ssize_t size = ::flistxattr(fd, nullptr, 0);
  if (size == -1) {
    return ErrnoToStatus(errno, "flistxattr");
  }
  if (size == 0) {
    return std::vector<std::string>();
  }
  // Now read the actual list
  std::string buf(size, '\0');
  ssize_t nbytes = ::flistxattr(fd, buf.data(), buf.size());
  if (nbytes == -1) {
    return ErrnoToStatus(errno, "flistxattr");
  }
  // Parse the NUL-separated list
  std::vector<std::string> result;
  size_t pos = 0;
  while (pos < static_cast<size_t>(nbytes)) {
    const char *str = buf.data() + pos;
    size_t len = std::strlen(str);
    result.emplace_back(str, len);
    pos += len + 1;
  }
  return result;
}

absl::Status fsetxattr(int fd, std::string_view name,
                       std::span<const uint8_t> value, int flags) {
  if (::fsetxattr(fd, std::string(name).c_str(),
                  reinterpret_cast<const void *>(value.data()), value.size(),
                  flags) == -1) {
    return ErrnoToStatus(errno, "fsetxattr");
  }
  return absl::OkStatus();
}

absl::Status fremovexattr(int fd, std::string_view name) {
  if (::fremovexattr(fd, std::string(name).c_str()) == -1) {
    return ErrnoToStatus(errno, "fremovexattr");
  }
  return absl::OkStatus();
}

absl::StatusOr<FileDescriptor> ReopenPathFd(int fd, int flags) {
  // Opens /proc/self/fd/<fd> with the given flags (| O_CLOEXEC).
  // This is needed because xattr/ioctl syscalls reject O_PATH fds.
  std::string path = absl::StrFormat("/proc/self/fd/%d", fd);
  int new_fd = ::open(path.c_str(), flags | O_CLOEXEC);
  if (new_fd == -1) {
    return ErrnoToStatus(errno, absl::StrFormat("open(%s)", path));
  }
  return FileDescriptor(new_fd);
}

absl::Status linkat(int olddirfd, std::string_view oldpath, int newdirfd,
                    std::string_view newpath, int flags) {
  if (::linkat(olddirfd, std::string(oldpath).c_str(), newdirfd,
               std::string(newpath).c_str(), flags) == -1) {
    return ErrnoToStatus(errno, "linkat");
  }
  return absl::OkStatus();
}

absl::Status unlinkat(int dirfd, std::string_view path, int flags) {
  if (::unlinkat(dirfd, std::string(path).c_str(), flags) == -1) {
    return ErrnoToStatus(errno, "unlinkat");
  }
  return absl::OkStatus();
}

absl::Status renameat2(int olddirfd, std::string_view oldpath, int newdirfd,
                       std::string_view newpath, unsigned int flags) {
  if (::renameat2(olddirfd, std::string(oldpath).c_str(), newdirfd,
                  std::string(newpath).c_str(), flags) == -1) {
    return ErrnoToStatus(errno, "renameat2");
  }
  return absl::OkStatus();
}

absl::Status mkdirat(int dirfd, std::string_view path, mode_t mode) {
  if (::mkdirat(dirfd, std::string(path).c_str(), mode) == -1) {
    return ErrnoToStatus(errno, "mkdirat");
  }
  return absl::OkStatus();
}

absl::Status mknodat(int dirfd, std::string_view path, mode_t mode, dev_t dev) {
  if (::mknodat(dirfd, std::string(path).c_str(), mode, dev) == -1) {
    return ErrnoToStatus(errno, "mknodat");
  }
  return absl::OkStatus();
}

absl::Status symlinkat(std::string_view target, int newdirfd,
                       std::string_view linkpath) {
  if (::symlinkat(std::string(target).c_str(), newdirfd,
                  std::string(linkpath).c_str()) == -1) {
    return ErrnoToStatus(errno, "symlinkat");
  }
  return absl::OkStatus();
}

absl::Status fchmod(int fd, mode_t mode) {
  if (::fchmod(fd, mode) == -1) {
    return ErrnoToStatus(errno, "fchmod");
  }
  return absl::OkStatus();
}

absl::Status fchownat(int dirfd, std::string_view path, uid_t owner,
                      gid_t group, int flags) {
  if (::fchownat(dirfd, std::string(path).c_str(), owner, group, flags) == -1) {
    return ErrnoToStatus(errno, "fchownat");
  }
  return absl::OkStatus();
}

absl::Status futimens(int fd, const struct timespec times[2]) {
  if (::futimens(fd, times) == -1) {
    return ErrnoToStatus(errno, "futimens");
  }
  return absl::OkStatus();
}

absl::Status ftruncate(int fd, off_t length) {
  if (::ftruncate(fd, length) == -1) {
    return ErrnoToStatus(errno, "ftruncate");
  }
  return absl::OkStatus();
}

absl::Status fsync(int fd) {
  if (::fsync(fd) == -1) {
    return ErrnoToStatus(errno, "fsync");
  }
  return absl::OkStatus();
}

absl::Status fdatasync(int fd) {
  if (::fdatasync(fd) == -1) {
    return ErrnoToStatus(errno, "fdatasync");
  }
  return absl::OkStatus();
}

absl::Status fallocate(int fd, int mode, off_t offset, off_t len) {
  if (::fallocate(fd, mode, offset, len) == -1) {
    return ErrnoToStatus(errno, "fallocate");
  }
  return absl::OkStatus();
}

absl::StatusOr<size_t> pread(int fd, void *buf, size_t count, off_t offset) {
  ssize_t nbytes = ::pread(fd, buf, count, offset);
  if (nbytes == -1) {
    return ErrnoToStatus(errno, "pread");
  }
  return nbytes;
}

absl::StatusOr<size_t> pwrite(int fd, const void *buf, size_t count,
                              off_t offset) {
  ssize_t nbytes = ::pwrite(fd, buf, count, offset);
  if (nbytes == -1) {
    return ErrnoToStatus(errno, "pwrite");
  }
  return nbytes;
}

absl::StatusOr<size_t> write(int fd, const void *buf, size_t count) {
  ssize_t nbytes = ::write(fd, buf, count);
  if (nbytes == -1) {
    return ErrnoToStatus(errno, "write");
  }
  return nbytes;
}

}  // namespace syscalls

absl::StatusOr<uint32_t> GetInodeGeneration(int fd) {
  uint32_t generation = 0;
  ABSL_RETURN_IF_ERROR(
      syscalls::ioctl(fd, FS_IOC_GETVERSION, &generation).status());
  return generation;
}

LogOpenFlags::LogOpenFlags(int flags) : flags_(flags) {}

}  // namespace dcfs
