#include "dcfs/syscalls.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/escape.h"
#include "dcfs/status.h"

namespace dcfs {
namespace syscalls {

absl::Status close(FileDescriptor fd) {
  // Failure of close is not recoverable... we must leak the fd.
  int fd_i = std::move(fd).Release();
  errno = 0;
  ::close(fd_i);
  return ErrnoToStatus(errno, absl::StrCat("close(", fd_i, ")"));
}

absl::StatusOr<FileDescriptor> dup(int fd) {
  int new_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (new_fd == -1) return ErrnoToStatus(errno, absl::StrCat("dup(", fd, ")"));
  return FileDescriptor(new_fd);
}

uid_t fsuid() { return static_cast<uid_t>(::setfsuid(static_cast<uid_t>(-1))); }

gid_t fsgid() { return static_cast<gid_t>(::setfsgid(static_cast<gid_t>(-1))); }

absl::Status setfsuid(uid_t uid) {
  if (uid == static_cast<uid_t>(-1)) {
    return ProducedErrnoToStatus(EINVAL, "setfsuid(-1) is not an id");
  }
  ::setfsuid(uid);
  if (fsuid() != uid) {
    return ProducedErrnoToStatus(
        EPERM, absl::StrCat("setfsuid(", uid, ") did not take"));
  }
  return absl::OkStatus();
}

absl::Status setfsgid(gid_t gid) {
  if (gid == static_cast<gid_t>(-1)) {
    return ProducedErrnoToStatus(EINVAL, "setfsgid(-1) is not an id");
  }
  ::setfsgid(gid);
  if (fsgid() != gid) {
    return ProducedErrnoToStatus(
        EPERM, absl::StrCat("setfsgid(", gid, ") did not take"));
  }
  return absl::OkStatus();
}

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

void sync() { ::sync(); }

absl::Status nanosleep(const struct timespec &duration) {
  if (::nanosleep(&duration, nullptr) == -1) {
    return ErrnoToStatus(errno, "nanosleep");
  }
  return absl::OkStatus();
}

pid_t getpid() { return ::getpid(); }

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

absl::StatusOr<short> poll(int fd, short events, int timeout_ms) {
  struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
  const int n = ::poll(&pfd, 1, timeout_ms);
  if (n == -1) return ErrnoToStatus(errno, absl::StrCat("poll(", fd, ")"));
  return n == 0 ? static_cast<short>(0) : pfd.revents;
}

uid_t getuid() { return ::getuid(); }

absl::StatusOr<pid_t> setsid() {
  const pid_t group = ::setsid();
  if (group == -1) return ErrnoToStatus(errno, "setsid");
  return group;
}

absl::Status chdir(std::string_view path) {
  const std::string path_str(path);
  if (::chdir(path_str.c_str()) == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("chdir(", EscapeBytes(path), ")"));
  }
  return absl::OkStatus();
}

absl::Status unshare(int flags) {
  if (::unshare(flags) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("unshare(", flags, ")"));
  }
  return absl::OkStatus();
}

absl::Status close_range(unsigned int first, unsigned int last, int flags) {
  if (::close_range(first, last, static_cast<unsigned int>(flags)) == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("close_range(", first, ", ", last, ")"));
  }
  return absl::OkStatus();
}

absl::StatusOr<FileDescriptor> open_tree(int dirfd, std::string_view path,
                                         unsigned int flags) {
  const std::string path_str(path);
  const int fd = ::open_tree(dirfd, path_str.c_str(), flags);
  if (fd == -1) {
    return ErrnoToStatus(
        errno, absl::StrCat("open_tree(", dirfd, ", ", EscapeBytes(path), ")"));
  }
  return FileDescriptor(fd);
}

absl::StatusOr<std::pair<FileDescriptor, FileDescriptor>> socketpair(
    int domain, int type, int protocol) {
  int fds[2];
  if (::socketpair(domain, type | SOCK_CLOEXEC, protocol, fds) == -1) {
    return ErrnoToStatus(errno, "socketpair");
  }
  return std::make_pair(FileDescriptor(fds[0]), FileDescriptor(fds[1]));
}

absl::StatusOr<size_t> sendmsg(int fd, const struct msghdr &message,
                               int flags) {
  const ssize_t n = ::sendmsg(fd, &message, flags);
  if (n == -1) return ErrnoToStatus(errno, absl::StrCat("sendmsg(", fd, ")"));
  return static_cast<size_t>(n);
}

absl::StatusOr<size_t> recvmsg(int fd, struct msghdr &message, int flags) {
  const ssize_t n = ::recvmsg(fd, &message, flags | MSG_CMSG_CLOEXEC);
  if (n == -1) return ErrnoToStatus(errno, absl::StrCat("recvmsg(", fd, ")"));
  return static_cast<size_t>(n);
}

absl::StatusOr<size_t> send(int fd, const void *buf, size_t count, int flags) {
  const ssize_t n = ::send(fd, buf, count, flags);
  if (n == -1) return ErrnoToStatus(errno, absl::StrCat("send(", fd, ")"));
  return static_cast<size_t>(n);
}

void openlog(const char *ident, int option, int facility) {
  ::openlog(ident, option, facility);
}

void syslog(int priority, std::string_view message) {
  const std::string message_str(message);
  ::syslog(priority, "%s", message_str.c_str());
}

}  // namespace syscalls

}  // namespace dcfs
