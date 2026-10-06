#include "dcfs/syscalls.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <string_view>
#include <sys/fsuid.h>
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

absl::StatusOr<std::string> readlinkat(int dirfd, std::string_view path) {
  std::string path_str(path);
  std::string result;
  size_t bufsize = 256;
  while (true) {
    result.resize(bufsize);
    ssize_t nbytes = ::readlinkat(dirfd, path_str.c_str(),
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
      return ErrnoToStatus(ENAMETOOLONG, "readlinkat: target longer than PATH_MAX*4");
    }
    bufsize *= 2;
  }
}

absl::StatusOr<std::string> fgetxattr(int fd, std::string_view name) {
  std::string name_str(name);
  // Query size with a null buffer first
  ssize_t size = ::fgetxattr(fd, name_str.c_str(), nullptr, 0);
  if (size == -1) {
    return ErrnoToStatus(errno, "fgetxattr");
  }
  if (size == 0) {
    return std::string();
  }
  // Now read the actual value; retry once if size grows (ERANGE on second call)
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string result(size, '\0');
    ssize_t nbytes = ::fgetxattr(fd, name_str.c_str(), result.data(),
                                 result.size());
    if (nbytes == -1) {
      int err = errno;
      if (err == ERANGE && attempt == 0) {
        // Value grew between query and read; retry by re-querying
        size = ::fgetxattr(fd, name_str.c_str(), nullptr, 0);
        if (size == -1) {
          return ErrnoToStatus(errno, "fgetxattr");
        }
        if (size == 0) {
          return std::string();
        }
        continue;
      }
      return ErrnoToStatus(err, "fgetxattr");
    }
    result.resize(nbytes);
    return result;
  }
  // Should not reach here
  return ErrnoToStatus(ERANGE, "fgetxattr: retry loop exhausted");
}

absl::StatusOr<std::vector<std::string>> flistxattr(int fd) {
  // Query size with a null buffer first; retry once if list grows (ERANGE on second call)
  for (int attempt = 0; attempt < 2; ++attempt) {
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
      int err = errno;
      if (err == ERANGE && attempt == 0) {
        // List grew between query and read; retry from the start
        continue;
      }
      return ErrnoToStatus(err, "flistxattr");
    }
    // Parse the NUL-separated list
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < static_cast<size_t>(nbytes)) {
      const char *str = buf.data() + pos;
      const void *nul = std::memchr(str, '\0', nbytes - pos);
      size_t len = nul == nullptr ? nbytes - pos
                                  : static_cast<const char *>(nul) - str;
      result.emplace_back(str, len);
      pos += len + 1;
    }
    return result;
  }
  // Should not reach here
  return ErrnoToStatus(ERANGE, "flistxattr: retry loop exhausted");
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

namespace {

std::string ProcFdPath(int fd) {
  return absl::StrFormat("/proc/self/fd/%d", fd);
}

// Splits a listxattr(2) result: NUL-terminated names, back to back.
std::vector<std::string> SplitXattrList(std::string_view buf) {
  std::vector<std::string> result;
  size_t pos = 0;
  while (pos < buf.size()) {
    size_t end = buf.find('\0', pos);
    if (end == std::string_view::npos) end = buf.size();
    result.emplace_back(buf.substr(pos, end - pos));
    pos = end + 1;
  }
  return result;
}

}  // namespace

absl::StatusOr<std::vector<std::string>> listxattr_opath(int fd) {
  const std::string path = ProcFdPath(fd);
  // The list can grow between sizing and reading it; ERANGE then means
  // "size again", which a few retries make overwhelmingly likely to settle.
  for (int attempt = 0; attempt < 4; ++attempt) {
    ssize_t size = ::listxattr(path.c_str(), nullptr, 0);
    if (size == -1) return ErrnoToStatus(errno, absl::StrCat("listxattr(", path, ")"));
    if (size == 0) return std::vector<std::string>();
    std::string buf(size, '\0');
    ssize_t nbytes = ::listxattr(path.c_str(), buf.data(), buf.size());
    if (nbytes == -1) {
      if (errno == ERANGE) continue;
      return ErrnoToStatus(errno, absl::StrCat("listxattr(", path, ")"));
    }
    buf.resize(nbytes);
    return SplitXattrList(buf);
  }
  return ErrnoToStatus(ERANGE, absl::StrCat("listxattr(", path, "): kept growing"));
}

absl::StatusOr<std::string> getxattr_opath(int fd, std::string_view name) {
  const std::string path = ProcFdPath(fd);
  const std::string name_str(name);
  for (int attempt = 0; attempt < 4; ++attempt) {
    ssize_t size = ::getxattr(path.c_str(), name_str.c_str(), nullptr, 0);
    if (size == -1) {
      return ErrnoToStatus(errno, absl::StrCat("getxattr(", path, ", ", EscapeBytes(name), ")"));
    }
    if (size == 0) return std::string();
    std::string value(size, '\0');
    ssize_t nbytes =
        ::getxattr(path.c_str(), name_str.c_str(), value.data(), value.size());
    if (nbytes == -1) {
      if (errno == ERANGE) continue;
      return ErrnoToStatus(errno, absl::StrCat("getxattr(", path, ", ", EscapeBytes(name), ")"));
    }
    value.resize(nbytes);
    return value;
  }
  return ErrnoToStatus(ERANGE, absl::StrCat("getxattr(", path, ", ", EscapeBytes(name),
                                            "): kept growing"));
}

absl::Status setxattr_opath(int fd, std::string_view name,
                            std::span<const uint8_t> value, int flags) {
  const std::string path = ProcFdPath(fd);
  const std::string name_str(name);
  if (::setxattr(path.c_str(), name_str.c_str(),
                 reinterpret_cast<const void *>(value.data()), value.size(),
                 flags) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("setxattr(", path, ", ", EscapeBytes(name), ")"));
  }
  return absl::OkStatus();
}

absl::Status removexattr_opath(int fd, std::string_view name) {
  const std::string path = ProcFdPath(fd);
  const std::string name_str(name);
  if (::removexattr(path.c_str(), name_str.c_str()) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("removexattr(", path, ", ", EscapeBytes(name), ")"));
  }
  return absl::OkStatus();
}

absl::Status fchmod_opath(int fd, mode_t mode) {
  const std::string path = ProcFdPath(fd);
  if (::fchmodat(AT_FDCWD, path.c_str(), mode, 0) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("fchmodat(", path, ")"));
  }
  return absl::OkStatus();
}

absl::Status futimens_opath(int fd, const struct timespec times[2]) {
  const std::string path = ProcFdPath(fd);
  if (::utimensat(AT_FDCWD, path.c_str(), times, 0) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("utimensat(", path, ")"));
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

uid_t fsuid() { return setfsuid(static_cast<uid_t>(-1)); }

gid_t fsgid() { return setfsgid(static_cast<gid_t>(-1)); }

mode_t umask(mode_t mask) { return ::umask(mask); }

absl::Status setgroups_thread(std::span<const gid_t> groups) {
  if (::syscall(SYS_setgroups, groups.size(), groups.data()) == -1) {
    return ErrnoToStatus(errno, "setgroups");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<gid_t>> getgroups() {
  while (true) {
    int n = ::getgroups(0, nullptr);
    if (n == -1) return ErrnoToStatus(errno, "getgroups");
    std::vector<gid_t> groups(n);
    int got = ::getgroups(n, groups.data());
    if (got == -1) {
      // The list grew in between (another thread cannot change ours, but
      // be exact anyway): ask again.
      if (errno == EINVAL) continue;
      return ErrnoToStatus(errno, "getgroups");
    }
    groups.resize(got);
    return groups;
  }
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
