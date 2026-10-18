// SPDX-License-Identifier: MIT
#include "dcfs/syscalls.h"

#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "dcfs/capability_set.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"

// glibc exports capget(2) and capset(2) but declares them in no header: the
// manpage calls them through syscall(2), and libcap's <sys/capability.h>,
// which declares them like this, is not in the sysroot.
extern "C" int capget(cap_user_header_t header, cap_user_data_t data);
extern "C" int capset(cap_user_header_t header, cap_user_data_t data);

namespace dcfs::syscalls {

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

namespace {

// The header naming the calling thread, in the 64-bit layout.
__user_cap_header_struct ThisThread() {
  return {.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
}

using CapabilityData =
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3>;

}  // namespace

absl::StatusOr<CapabilitySets> capget() {
  __user_cap_header_struct header = ThisThread();
  CapabilityData data{};
  if (::capget(&header, data.data()) == -1) {
    return ErrnoToStatus(errno, "capget");
  }
  auto join = [](uint32_t low, uint32_t high) {
    return CapabilitySet::FromKernel(uint64_t{high} << 32 | low);
  };
  return CapabilitySets{
      .effective = join(data[0].effective, data[1].effective),
      .permitted = join(data[0].permitted, data[1].permitted),
      .inheritable = join(data[0].inheritable, data[1].inheritable)};
}

absl::Status capset(const CapabilitySets &sets) {
  __user_cap_header_struct header = ThisThread();
  CapabilityData data{};
  for (size_t i = 0; i < data.size(); ++i) {
    size_t shift = 32 * i;
    auto word = [shift](CapabilitySet set) {
      return static_cast<uint32_t>(set.ToKernel() >> shift);
    };
    data[i] = {.effective = word(sets.effective),
               .permitted = word(sets.permitted),
               .inheritable = word(sets.inheritable)};
  }
  if (::capset(&header, data.data()) == -1) {
    return ErrnoToStatus(errno, "capset");
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

void sync() { ::sync(); }

absl::Status nanosleep(const struct timespec &duration) {
  if (::nanosleep(&duration, nullptr) == -1) {
    return ErrnoToStatus(errno, "nanosleep");
  }
  return absl::OkStatus();
}

pid_t getpid() { return ::getpid(); }

pid_t gettid() { return ::gettid(); }

absl::StatusOr<int> fcntl(int fd, int cmd, int arg) {
  int rc = ::fcntl(fd, cmd, arg);
  if (rc == -1) return ErrnoToStatus(errno, absl::StrCat("fcntl(", fd, ")"));
  return rc;
}

absl::StatusOr<std::string> realpath(std::string_view path) {
  const std::string path_str(path);
  char *resolved = ::realpath(path_str.c_str(), nullptr);
  if (resolved == nullptr) {
    return ErrnoToStatus(errno,
                         absl::StrCat("realpath(", EscapeBytes(path), ")"));
  }
  std::string result(resolved);
  ::free(resolved);
  return result;
}

absl::StatusOr<struct timespec> clock_gettime(clockid_t clock) {
  struct timespec now{};
  if (::clock_gettime(clock, &now) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("clock_gettime(", clock, ")"));
  }
  return now;
}

absl::StatusOr<struct rlimit> getrlimit(int resource) {
  struct rlimit limit{};
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

absl::StatusOr<int> poll(std::span<struct pollfd> fds, absl::Duration timeout) {
  // poll(2) takes milliseconds in an int, and a negative one waits for ever.
  RET_CHECK_GE(timeout, absl::ZeroDuration());
  RET_CHECK(timeout == absl::InfiniteDuration() ||
            timeout <= absl::Milliseconds(INT_MAX))
      << "poll timeout " << timeout;
  const int timeout_ms =
      timeout == absl::InfiniteDuration()
          ? -1
          : static_cast<int>(absl::ToInt64Milliseconds(timeout));
  int n = ::poll(fds.data(), fds.size(), timeout_ms);
  if (n == -1) {
    return ErrnoToStatus(errno, absl::StrCat("poll of ", fds.size(), " fds"));
  }
  return n;
}

absl::StatusOr<FileDescriptor> pidfd_open(pid_t pid) {
  int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
  if (fd == -1) {
    return ErrnoToStatus(errno, absl::StrCat("pidfd_open(", pid, ")"));
  }
  return FileDescriptor(fd);
}

absl::Status setresuid(uid_t real, uid_t effective, uid_t saved) {
  if (::setresuid(real, effective, saved) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("setresuid(", real, ", ",
                                             effective, ", ", saved, ")"));
  }
  return absl::OkStatus();
}

absl::Status setresgid(gid_t real, gid_t effective, gid_t saved) {
  if (::setresgid(real, effective, saved) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("setresgid(", real, ", ",
                                             effective, ", ", saved, ")"));
  }
  return absl::OkStatus();
}

uid_t getuid() { return ::getuid(); }
uid_t geteuid() { return ::geteuid(); }
gid_t getgid() { return ::getgid(); }
gid_t getegid() { return ::getegid(); }

absl::StatusOr<pid_t> setsid() {
  const pid_t group = ::setsid();
  if (group == -1) return ErrnoToStatus(errno, "setsid");
  return group;
}

absl::Status chdir(std::string_view path) {
  const std::string path_str(path);
  if (::chdir(path_str.c_str()) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("chdir(", EscapeBytes(path), ")"));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> getcwd() {
  char *cwd = ::getcwd(nullptr, 0);
  if (cwd == nullptr) return ErrnoToStatus(errno, "getcwd");
  std::string result(cwd);
  ::free(cwd);
  return result;
}

absl::StatusOr<std::pair<FileDescriptor, FileDescriptor>> socketpair(
    int domain, int type, int protocol) {
  int fds[2];
  if (::socketpair(domain, type | SOCK_CLOEXEC, protocol, fds) == -1) {
    return ErrnoToStatus(errno, "socketpair");
  }
  return std::make_pair(FileDescriptor(fds[0]), FileDescriptor(fds[1]));
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

absl::StatusOr<FileDescriptor> open(std::string_view path, int flags,
                                    mode_t mode) {
  std::string path_str(path);
  int fd = ::open(path_str.c_str(), flags | O_CLOEXEC, mode);
  if (fd == -1) {
    return ErrnoToStatus(errno, absl::StrCat("open(", EscapeBytes(path), ")"));
  }
  return FileDescriptor(fd);
}

absl::Status mkdir(std::string_view path, mode_t mode) {
  std::string path_str(path);
  if (::mkdir(path_str.c_str(), mode) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("mkdir(", EscapeBytes(path), ")"));
  }
  return absl::OkStatus();
}

absl::Status unlink(std::string_view path) {
  std::string path_str(path);
  if (::unlink(path_str.c_str()) == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("unlink(", EscapeBytes(path), ")"));
  }
  return absl::OkStatus();
}

absl::StatusOr<struct stat> stat(std::string_view path) {
  std::string path_str(path);
  struct stat buf{};
  if (::stat(path_str.c_str(), &buf) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("stat(", EscapeBytes(path), ")"));
  }
  return buf;
}

absl::Status chmod(std::string_view path, mode_t mode) {
  std::string path_str(path);
  if (::chmod(path_str.c_str(), mode) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("chmod(", EscapeBytes(path), ")"));
  }
  return absl::OkStatus();
}

absl::Status rename(std::string_view from, std::string_view to) {
  std::string from_str(from);
  std::string to_str(to);
  if (::rename(from_str.c_str(), to_str.c_str()) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("rename(", EscapeBytes(from), ", ",
                                             EscapeBytes(to), ")"));
  }
  return absl::OkStatus();
}

absl::Status rmdir(std::string_view path) {
  std::string path_str(path);
  if (::rmdir(path_str.c_str()) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("rmdir(", EscapeBytes(path), ")"));
  }
  return absl::OkStatus();
}

absl::StatusOr<struct statfs> statfs(std::string_view path) {
  std::string path_str(path);
  struct statfs buf{};
  if (::statfs(path_str.c_str(), &buf) == -1) {
    return ErrnoToStatus(errno,
                         absl::StrCat("statfs(", EscapeBytes(path), ")"));
  }
  return buf;
}

absl::StatusOr<void *> mmap(void *addr, size_t length, int prot, int flags,
                            int fd, off_t offset) {
  void *mapped = ::mmap(addr, length, prot, flags, fd, offset);
  if (mapped == MAP_FAILED) {
    return ErrnoToStatus(errno, absl::StrCat("mmap(", fd, ")"));
  }
  return mapped;
}

absl::Status munmap(void *addr, size_t length) {
  if (::munmap(addr, length) == -1) return ErrnoToStatus(errno, "munmap");
  return absl::OkStatus();
}

absl::Status mincore(void *addr, size_t length, unsigned char *vec) {
  if (::mincore(addr, length, vec) == -1) {
    return ErrnoToStatus(errno, "mincore");
  }
  return absl::OkStatus();
}

absl::StatusOr<FileDescriptor> inotify_init1(int flags) {
  int fd = ::inotify_init1(flags | IN_CLOEXEC);
  if (fd == -1) return ErrnoToStatus(errno, "inotify_init1");
  return FileDescriptor(fd);
}

absl::StatusOr<int> inotify_add_watch(int fd, std::string_view path,
                                      uint32_t mask) {
  std::string path_str(path);
  int wd = ::inotify_add_watch(fd, path_str.c_str(), mask);
  if (wd == -1) {
    return ErrnoToStatus(
        errno, absl::StrCat("inotify_add_watch(", EscapeBytes(path), ")"));
  }
  return wd;
}

}  // namespace dcfs::syscalls
