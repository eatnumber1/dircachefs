#include "dcfs/syscalls.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
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

}  // namespace syscalls

}  // namespace dcfs
