#include "dcfs/syscalls.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <string_view>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/fsuid.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>

#include "absl/log/log.h"
#include "absl/status/status_macros.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/escape.h"
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
    return ErrnoToStatus(errno, absl::StrFormat("openat(%d, %s)", dirfd, EscapeBytes(pathname)));
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
  std::string pathname_str(pathname);
  struct stat buf;
  int ret = ::fstatat(dirfd, pathname_str.c_str(), &buf, flags);
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
        errno, absl::StrFormat("name_to_handle_at(%d, %s)", dirfd,
                        EscapeBytes(pathname)));
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
  std::string path_str(path);
  struct statx buf;
  int ret = ::statx(dirfd, path_str.c_str(), flags, mask, &buf);
  if (ret == -1) return ErrnoToStatus(errno, "statx");
  return buf;
}

absl::StatusOr<struct statfs> fstatfs(int fd) {
  struct statfs buf;
  int ret = ::fstatfs(fd, &buf);
  if (ret == -1) return ErrnoToStatus(errno, "fstatfs");
  return buf;
}

absl::StatusOr<size_t> readlinkat(int dirfd, std::string_view path, char *buf,
                                  size_t size) {
  std::string path_str(path);
  ssize_t nbytes = ::readlinkat(dirfd, path_str.c_str(), buf, size);
  if (nbytes == -1) return ErrnoToStatus(errno, "readlinkat");
  return static_cast<size_t>(nbytes);
}

absl::StatusOr<size_t> fgetxattr(int fd, std::string_view name, void *value,
                                 size_t size) {
  std::string name_str(name);
  ssize_t nbytes = ::fgetxattr(fd, name_str.c_str(), value, size);
  if (nbytes == -1) return ErrnoToStatus(errno, "fgetxattr");
  return static_cast<size_t>(nbytes);
}

absl::StatusOr<size_t> flistxattr(int fd, char *list, size_t size) {
  ssize_t nbytes = ::flistxattr(fd, list, size);
  if (nbytes == -1) return ErrnoToStatus(errno, "flistxattr");
  return static_cast<size_t>(nbytes);
}

absl::Status fsetxattr(int fd, std::string_view name,
                       std::span<const uint8_t> value, int flags) {
  std::string name_str(name);
  if (::fsetxattr(fd, name_str.c_str(),
                  reinterpret_cast<const void *>(value.data()), value.size(),
                  flags) == -1) {
    return ErrnoToStatus(errno, "fsetxattr");
  }
  return absl::OkStatus();
}

absl::Status fremovexattr(int fd, std::string_view name) {
  std::string name_str(name);
  if (::fremovexattr(fd, name_str.c_str()) == -1) {
    return ErrnoToStatus(errno, "fremovexattr");
  }
  return absl::OkStatus();
}

absl::StatusOr<size_t> getxattr(std::string_view path, std::string_view name,
                                void *value, size_t size) {
  const std::string path_str(path);
  const std::string name_str(name);
  ssize_t nbytes = ::getxattr(path_str.c_str(), name_str.c_str(), value, size);
  if (nbytes == -1) {
    return ErrnoToStatus(
        errno, absl::StrCat("getxattr(", path, ", ", EscapeBytes(name), ")"));
  }
  return static_cast<size_t>(nbytes);
}

absl::StatusOr<size_t> listxattr(std::string_view path, char *list,
                                 size_t size) {
  const std::string path_str(path);
  ssize_t nbytes = ::listxattr(path_str.c_str(), list, size);
  if (nbytes == -1) {
    return ErrnoToStatus(errno, absl::StrCat("listxattr(", path, ")"));
  }
  return static_cast<size_t>(nbytes);
}

absl::Status setxattr(std::string_view path, std::string_view name,
                      std::span<const uint8_t> value, int flags) {
  const std::string path_str(path);
  const std::string name_str(name);
  if (::setxattr(path_str.c_str(), name_str.c_str(),
                 reinterpret_cast<const void *>(value.data()), value.size(),
                 flags) == -1) {
    return ErrnoToStatus(
        errno, absl::StrCat("setxattr(", path, ", ", EscapeBytes(name), ")"));
  }
  return absl::OkStatus();
}

absl::Status removexattr(std::string_view path, std::string_view name) {
  const std::string path_str(path);
  const std::string name_str(name);
  if (::removexattr(path_str.c_str(), name_str.c_str()) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("removexattr(", path, ", ",
                                             EscapeBytes(name), ")"));
  }
  return absl::OkStatus();
}

absl::Status fchmodat(int dirfd, std::string_view path, mode_t mode,
                      int flags) {
  const std::string path_str(path);
  if (::fchmodat(dirfd, path_str.c_str(), mode, flags) == -1) {
    return ErrnoToStatus(
        errno, absl::StrFormat("fchmodat(%d, %s)", dirfd, EscapeBytes(path)));
  }
  return absl::OkStatus();
}

absl::Status utimensat(int dirfd, std::string_view path,
                       const struct timespec times[2], int flags) {
  const std::string path_str(path);
  if (::utimensat(dirfd, path_str.c_str(), times, flags) == -1) {
    return ErrnoToStatus(
        errno, absl::StrFormat("utimensat(%d, %s)", dirfd, EscapeBytes(path)));
  }
  return absl::OkStatus();
}

absl::StatusOr<FileDescriptor> dup(int fd) {
  int new_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (new_fd == -1) return ErrnoToStatus(errno, absl::StrCat("dup(", fd, ")"));
  return FileDescriptor(new_fd);
}

absl::Status linkat(int olddirfd, std::string_view oldpath, int newdirfd,
                    std::string_view newpath, int flags) {
  std::string oldpath_str(oldpath);
  std::string newpath_str(newpath);
  if (::linkat(olddirfd, oldpath_str.c_str(), newdirfd,
               newpath_str.c_str(), flags) == -1) {
    return ErrnoToStatus(errno, "linkat");
  }
  return absl::OkStatus();
}

absl::Status unlinkat(int dirfd, std::string_view path, int flags) {
  std::string path_str(path);
  if (::unlinkat(dirfd, path_str.c_str(), flags) == -1) {
    return ErrnoToStatus(errno, "unlinkat");
  }
  return absl::OkStatus();
}

absl::Status renameat2(int olddirfd, std::string_view oldpath, int newdirfd,
                       std::string_view newpath, unsigned int flags) {
  std::string oldpath_str(oldpath);
  std::string newpath_str(newpath);
  if (::renameat2(olddirfd, oldpath_str.c_str(), newdirfd,
                  newpath_str.c_str(), flags) == -1) {
    return ErrnoToStatus(errno, "renameat2");
  }
  return absl::OkStatus();
}

absl::Status mkdirat(int dirfd, std::string_view path, mode_t mode) {
  std::string path_str(path);
  if (::mkdirat(dirfd, path_str.c_str(), mode) == -1) {
    return ErrnoToStatus(errno, "mkdirat");
  }
  return absl::OkStatus();
}

absl::Status mknodat(int dirfd, std::string_view path, mode_t mode, dev_t dev) {
  std::string path_str(path);
  if (::mknodat(dirfd, path_str.c_str(), mode, dev) == -1) {
    return ErrnoToStatus(errno, "mknodat");
  }
  return absl::OkStatus();
}

absl::Status symlinkat(std::string_view target, int newdirfd,
                       std::string_view linkpath) {
  std::string target_str(target);
  std::string linkpath_str(linkpath);
  if (::symlinkat(target_str.c_str(), newdirfd,
                  linkpath_str.c_str()) == -1) {
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
  std::string path_str(path);
  if (::fchownat(dirfd, path_str.c_str(), owner, group, flags) == -1) {
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

absl::Status syncfs(int fd) {
  if (::syncfs(fd) == -1) {
    return ErrnoToStatus(errno, "syncfs");
  }
  return absl::OkStatus();
}

absl::Status fallocate(int fd, int mode, off_t offset, off_t len) {
  if (::fallocate(fd, mode, offset, len) == -1) {
    return ErrnoToStatus(errno, "fallocate");
  }
  return absl::OkStatus();
}

absl::StatusOr<size_t> copy_file_range(int fd_in, off_t off_in, int fd_out,
                                       off_t off_out, size_t len,
                                       unsigned int flags) {
  loff_t in = off_in;
  loff_t out = off_out;
  ssize_t nbytes = ::copy_file_range(fd_in, &in, fd_out, &out, len, flags);
  if (nbytes == -1) return ErrnoToStatus(errno, "copy_file_range");
  return static_cast<size_t>(nbytes);
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

uid_t setfsuid(uid_t uid) { return static_cast<uid_t>(::setfsuid(uid)); }

gid_t setfsgid(gid_t gid) { return static_cast<gid_t>(::setfsgid(gid)); }

mode_t umask(mode_t mask) { return ::umask(mask); }

absl::Status setgroups(std::span<const gid_t> groups) {
  if (::syscall(SYS_setgroups, groups.size(), groups.data()) == -1) {
    return ErrnoToStatus(errno, "setgroups");
  }
  return absl::OkStatus();
}

absl::StatusOr<int> getgroups(int size, gid_t *list) {
  int n = ::getgroups(size, list);
  if (n == -1) return ErrnoToStatus(errno, "getgroups");
  return n;
}

absl::Status mount(const char *source, std::string_view target,
                   const char *fstype, unsigned long flags, const void *data) {
  const std::string target_str(target);
  if (::mount(source, target_str.c_str(), fstype, flags, data) == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("mount(", EscapeBytes(target), ")"));
  }
  return absl::OkStatus();
}

absl::Status umount2(std::string_view target, int flags) {
  const std::string target_str(target);
  if (::umount2(target_str.c_str(), flags) == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("umount2(", EscapeBytes(target), ")"));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> mkdtemp(std::string_view pattern) {
  std::string name(pattern);
  if (::mkdtemp(name.data()) == nullptr) {
    return ErrnoToStatus(errno,
                         absl::StrCat("mkdtemp(", EscapeBytes(pattern), ")"));
  }
  return name;
}

absl::StatusOr<FileDescriptor> mkstemp(std::string &pattern) {
  const std::string original = pattern;
  int fd = ::mkostemp(pattern.data(), O_CLOEXEC);
  if (fd == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("mkstemp(", EscapeBytes(original), ")"));
  }
  return FileDescriptor(fd);
}

absl::StatusOr<int> fcntl(int fd, int cmd, int arg) {
  int rc = ::fcntl(fd, cmd, arg);
  if (rc == -1) return ErrnoToStatus(errno, absl::StrCat("fcntl(", fd, ")"));
  return rc;
}

absl::StatusOr<std::string> realpath(std::string_view path) {
  const std::string path_str(path);
  char *resolved = ::realpath(path_str.c_str(), nullptr);
  if (resolved == nullptr) {
    return ErrnoToStatus(
        errno, absl::StrCat("realpath(", EscapeBytes(path), ")"));
  }
  std::string result(resolved);
  ::free(resolved);
  return result;
}

absl::StatusOr<struct timespec> clock_gettime(clockid_t clock) {
  struct timespec now {};
  if (::clock_gettime(clock, &now) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("clock_gettime(", clock, ")"));
  }
  return now;
}

absl::StatusOr<struct rlimit> getrlimit(int resource) {
  struct rlimit limit {};
  if (::getrlimit(static_cast<__rlimit_resource_t>(resource), &limit) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("getrlimit(", resource, ")"));
  }
  return limit;
}

absl::Status setrlimit(int resource, const struct rlimit &limit) {
  if (::setrlimit(static_cast<__rlimit_resource_t>(resource), &limit) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("setrlimit(", resource, ")"));
  }
  return absl::OkStatus();
}

absl::Status flock(int fd, int operation) {
  if (::flock(fd, operation) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("flock(", fd, ")"));
  }
  return absl::OkStatus();
}

}  // namespace syscalls

}  // namespace dcfs
