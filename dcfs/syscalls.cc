#include "dcfs/syscalls.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <signal.h>
#include <string_view>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>

#include "absl/log/log.h"
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

absl::StatusOr<FileDescriptor> open(
    std::string_view pathname, int flags, mode_t mode) {
  int fd = ::open(std::string(pathname).c_str(), flags | O_CLOEXEC, mode);
  if (fd == -1) {
    return ErrnoToStatus(errno, absl::StrCat("open(", pathname, ")"));
  }
  return FileDescriptor(fd);
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

absl::Status mount(
    std::string_view source, std::string_view target,
    std::string_view filesystemtype, unsigned long mountflags,
    const void *data) {
  if (::mount(
        std::string(source).c_str(), std::string(target).c_str(),
        std::string(filesystemtype).c_str(), mountflags, data) == -1) {
    return ErrnoToStatus(errno, "mount");
  }
  return absl::OkStatus();
}

absl::StatusOr<struct stat> stat(const char *pathname) {
  struct stat statbuf;
  if (::stat(pathname, &statbuf) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("stat(", pathname, ")"));
  }
  return statbuf;
}

absl::Status umount(std::string_view target, int flags) {
  if (::umount2(std::string(target).c_str(), flags) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("umount2(", target, ")"));
  }
  return absl::OkStatus();
}

absl::Status sigaction(
    int signum, const struct sigaction *act, struct sigaction *oldact) {
  if (int rc = ::sigaction(signum, act, oldact); rc != 0) {
    return ErrnoToStatus(errno, "sigaction");
  }
  return absl::OkStatus();
}

absl::StatusOr<FileDescriptor> signalfd(const sigset_t &mask, int flags) {
  int fd = ::signalfd(/*fd=*/-1, &mask, flags | SFD_CLOEXEC);
  if (fd == -1) return ErrnoToStatus(errno, "signalfd");
  return FileDescriptor(fd);
}

absl::Status signalfd(int fd, const sigset_t &mask, int flags) {
  errno = 0;
  ::signalfd(fd, &mask, flags | SFD_CLOEXEC);
  return ErrnoToStatus(errno, "signalfd");
}

absl::Status sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  errno = 0;
  ::sigprocmask(how, set, oldset);
  return ErrnoToStatus(errno, "sigprocmask");
}

absl::Status pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  return ErrnoToStatus(
      ::pthread_sigmask(how, set, oldset), "pthread_sigmask");
}

absl::Status pthread_setschedparam(
    pthread_t thread, int policy, const sched_param &param) {
  return ErrnoToStatus(
      ::pthread_setschedparam(thread, policy, &param), "pthread_setschedparam");
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

absl::StatusOr<std::reference_wrapper<DIR>> fdopendir(FileDescriptor fd) {
  DIR *dir = ::fdopendir(*fd);
  if (dir == nullptr) return ErrnoToStatus(errno, "fdopendir");
  std::move(fd).Release();
  return *dir;
}

absl::StatusOr<std::reference_wrapper<DIR>> opendir(std::string_view dirname) {
  DIR *dir = ::opendir(std::string(dirname).c_str());
  if (dir == nullptr) return ErrnoToStatus(errno, "opendir");
  return *dir;
}

absl::Status closedir(DIR &dir) {
  int rc = ::closedir(&dir);
  if (rc != 0) return ErrnoToStatus(errno, "closedir");
  return absl::OkStatus();
}

absl::StatusOr<dirent *absl_nullable> readdir(DIR &dir) {
  errno = 0;
  dirent *dent = ::readdir(&dir);
  if (dent == nullptr && errno != 0) return ErrnoToStatus(errno, "readdir");
  return dent;
}

absl::StatusOr<long> telldir(DIR &dir) {
  long rc = ::telldir(&dir);
  if (rc == -1) return ErrnoToStatus(errno, "telldir");
  return rc;
}

absl::StatusOr<int> dirfd(DIR &dir) {
  int fd = ::dirfd(&dir);
  if (fd == -1) return ErrnoToStatus(errno, "dirfd");
}

absl::StatusOr<ssize_t> getdents64(int fd, void *dirp, size_t count) {
  ssize_t nb = ::getdents64(fd, dirp, count);
  if (nb < 0) return ErrnoToStatus(errno, "getdents64");
  return nb;
}

absl::StatusOr<off_t> lseek(int fd, off_t offset, int whence) {
  off_t rc = ::lseek(fd, offset, whence);
  if (rc == static_cast<off_t>(-1)) return ErrnoToStatus(errno, "lseek");
  return rc;
}

}  // namespace syscalls

absl::StatusOr<uint32_t> GetInodeGeneration(int fd) {
  uint32_t generation = 0;
  RETURN_IF_ERROR(syscalls::ioctl(fd, FS_IOC_GETVERSION, &generation).status());
  return generation;
}

absl::StatusOr<ScopedSignalMask> ScopedSignalMask::Create(
    int how, const sigset_t &set) {
  sigset_t oldset;
  RETURN_IF_ERROR(syscalls::pthread_sigmask(how, &set, &oldset));
  return ScopedSignalMask(std::move(oldset));
}

ScopedSignalMask::ScopedSignalMask(ScopedSignalMask &&o)
    : ScopedSignalMask() {
  *this = std::move(o);
}

ScopedSignalMask &ScopedSignalMask::operator=(ScopedSignalMask &&o) {
  using std::swap;
  swap(valid_, o.valid_);
  swap(oldset_, o.oldset_);
  return *this;
}

ScopedSignalMask::ScopedSignalMask(sigset_t oldset)
    : valid_(true), oldset_(std::move(oldset)) {}

ScopedSignalMask::~ScopedSignalMask() {
  if (!valid_) return;
  absl::Status st = syscalls::pthread_sigmask(SIG_SETMASK, &oldset_);
  LOG_IF(WARNING, !st.ok()) << "Failed to restore signal disposition " << st;
}

LogOpenFlags::LogOpenFlags(int flags) : flags_(flags) {}

void ClosedirAndLog::operator()(DIR *d) {
  if (d == nullptr) return;
  LOG_IF_ERROR(WARNING, syscalls::closedir(*d));
}

absl::StatusOr<DIR_unique_ptr> WrapDirfd(FileDescriptor dirfd) {
  ASSIGN_OR_RETURN(DIR &d, syscalls::fdopendir(*dirfd));
  std::move(dirfd).Release();
  return DIR_unique_ptr(&d);
}

}  // namespace dcfs
