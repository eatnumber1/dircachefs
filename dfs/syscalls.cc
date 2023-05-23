#include "dfs/syscalls.h"

#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>

#include "dfs/status.h"
#include "absl/log/log.h"

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

absl::Status sigaction(
    int signum, const struct sigaction *act, struct sigaction *oldact) {
  if (int rc = ::sigaction(signum, act, oldact); rc != 0) {
    return absl::ErrnoToStatus(errno, "sigaction");
  }
  return absl::OkStatus();
}

absl::StatusOr<dfs::FileDescriptor> signalfd(const sigset_t &mask, int flags) {
  int fd = ::signalfd(/*fd=*/-1, &mask, flags);
  if (fd == -1) return absl::ErrnoToStatus(errno, "signalfd");
  return dfs::FileDescriptor(fd);
}

absl::Status signalfd(int fd, const sigset_t &mask, int flags) {
  errno = 0;
  ::signalfd(fd, &mask, flags);
  return absl::ErrnoToStatus(errno, "signalfd");
}

absl::Status sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  errno = 0;
  ::sigprocmask(how, set, oldset);
  return absl::ErrnoToStatus(errno, "sigprocmask");
}

absl::Status pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  return absl::ErrnoToStatus(
      ::pthread_sigmask(how, set, oldset), "pthread_sigmask");
}

absl::Status pthread_setschedparam(
    pthread_t thread, int policy, const struct sched_param &param) {
  return absl::ErrnoToStatus(
      ::pthread_setschedparam(thread, policy, &param), "pthread_setschedparam");
}

}  // namespace syscalls

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

}  // namespace dfs
