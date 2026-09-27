#ifndef DCFS_SYSCALLS_H_
#define DCFS_SYSCALLS_H_

#include <cerrno>
#include <array>
#include <fcntl.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>
#include <dirent.h>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_join.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"

namespace dcfs {
namespace syscalls {

absl::Status close(FileDescriptor fd);

// O_CLOEXEC is unconditionally added.
absl::StatusOr<FileDescriptor> open(
    std::string_view pathname, int flags = 0, mode_t mode = 0);

// O_CLOEXEC is unconditionally added.
absl::StatusOr<FileDescriptor> openat(
    int dirfd, std::string_view pathname, int flags = 0,
    mode_t mode = 0);


// O_CLOEXEC is unconditionally added to how.flags.
absl::StatusOr<FileDescriptor> openat2(
    int dirfd, std::string_view pathname, open_how how,
    size_t size);

absl::StatusOr<size_t> read(int fd, void *buf, size_t count);

// Use MountedFS::PerformMount instead.
absl::Status mount(
    std::string_view source, std::string_view target,
    std::string_view filesystemtype, unsigned long mountflags = 0,
    const void *data = nullptr);

absl::Status umount(std::string_view target, int flags = 0);

absl::StatusOr<struct stat> stat(const char *pathname);

absl::Status sigaction(
    int signum,
    const struct sigaction *act = nullptr,
    struct sigaction *oldact = nullptr);

// SFD_CLOEXEC is unconditionally added to flags
absl::StatusOr<FileDescriptor> signalfd(
    const sigset_t &mask, int flags = 0);

// SFD_CLOEXEC is unconditionally added to flags
absl::Status signalfd(int fd, const sigset_t &mask, int flags = 0);

absl::Status sigprocmask(
    int how, const sigset_t *set, sigset_t *oldset = nullptr);

absl::Status pthread_sigmask(
    int how, const sigset_t *set, sigset_t *oldset = nullptr);

absl::Status pthread_setschedparam(
    pthread_t thread, int policy, const sched_param &param);

absl::StatusOr<struct statvfs> fstatvfs(int fd);
absl::StatusOr<struct stat> fstat(int fd);
absl::StatusOr<struct stat> fstatat(
    int dirfd, std::string_view pathname, int flags = 0);

absl::Status name_to_handle_at(
    int dirfd, std::string_view pathname, file_handle &handle,
    int &mount_id, int flags = 0);

// O_CLOEXEC is unconditionally added.
absl::StatusOr<FileDescriptor> open_by_handle_at(
    int mount_fd, const file_handle &handle, int flags = 0);

absl::StatusOr<int> ioctl(int fd, int op, auto &&... args);

absl::StatusOr<std::reference_wrapper<DIR>> fdopendir(FileDescriptor fd);
absl::StatusOr<std::reference_wrapper<DIR>> opendir(std::string_view dirname);
absl::Status closedir(DIR &dir);
// Returns nullptr at EOF.
absl::StatusOr<dirent *absl_nullable> readdir(DIR &dir);
absl::StatusOr<long> telldir(DIR &dir);
absl::StatusOr<int> dirfd(DIR &dir);

// Definition from https://man7.org/linux/man-pages/man2/getdents.2.html
struct linux_dirent64 {
  ino64_t d_ino;  // 64-bit inode number
  off64_t d_off;  // Not an offset; see getdents()
  unsigned short d_reclen;  // Size of this dirent
  unsigned char d_type;  // File type
  char d_name[];  // Filename (null-terminated)
};

absl::StatusOr<ssize_t> getdents64(int fd, void *dirp, size_t count);

absl::StatusOr<off_t> lseek(int fd, off_t offset, int whence);

}  // namespace syscalls

// Mask a set of signals (ala pthread_sigmask) and unmask them at destruction.
class ScopedSignalMask {
 public:
  ScopedSignalMask() = default;
  static absl::StatusOr<ScopedSignalMask> Create(int how, const sigset_t &set);

  ScopedSignalMask(ScopedSignalMask &&);
  ScopedSignalMask(const ScopedSignalMask &) = delete;
  ScopedSignalMask &operator=(ScopedSignalMask &&);
  ScopedSignalMask &operator=(const ScopedSignalMask &) = delete;

  ~ScopedSignalMask();

 private:
  ScopedSignalMask(sigset_t oldset);

  bool valid_ = false;
  sigset_t oldset_;
};

absl::StatusOr<uint32_t> GetInodeGeneration(int fd);

struct LogOpenFlags {
 public:
  explicit LogOpenFlags(int flags);

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const LogOpenFlags &l);

 private:
  int flags_ = 0;
};

struct ClosedirAndLog {
  void operator()(DIR *d);
};

using DIR_unique_ptr = std::unique_ptr<DIR, ClosedirAndLog>;

absl::StatusOr<DIR_unique_ptr> WrapDirfd(FileDescriptor dirfd);

// Implementation below here

namespace syscalls {

absl::StatusOr<int> ioctl(int fd, int op, auto &&... args) {
  int rc = ::ioctl(fd, op, std::forward<decltype(args)>(args)...);
  if (rc == -1) return absl::ErrnoToStatus(errno, "ioctl");
  return rc;
}

}  // namespace syscalls

template <typename Sink>
void AbslStringify(Sink &sink, const LogOpenFlags &l) {
  static const absl::flat_hash_map<int, const std::string> kFlagsToNames {
#define F(n) {n, #n}
#ifdef O_ACCMODE
      F(O_ACCMODE),
#endif  // O_ACCMODE
#ifdef O_RDONLY
      F(O_RDONLY),
#endif  // O_RDONLY
#ifdef O_WRONLY
      F(O_WRONLY),
#endif  // O_WRONLY
#ifdef O_RDWR
      F(O_RDWR),
#endif  // O_RDWR
#ifdef O_CREAT
      F(O_CREAT),
#endif  // O_CREAT
#ifdef O_EXCL
      F(O_EXCL),
#endif  // O_EXCL
#ifdef O_NOCTTY
      F(O_NOCTTY),
#endif  // O_NOCTTY
#ifdef O_TRUNC
      F(O_TRUNC),
#endif  // O_TRUNC
#ifdef O_APPEND
      F(O_APPEND),
#endif  // O_APPEND
#ifdef O_NONBLOCK
      F(O_NONBLOCK),
#endif  // O_NONBLOCK
#ifdef O_DSYNC
      F(O_DSYNC),
#endif  // O_DSYNC
#ifdef FASYNC
      F(FASYNC),
#endif  // FASYNC
#ifdef O_DIRECT
      F(O_DIRECT),
#endif  // O_DIRECT
#ifdef O_LARGEFILE
      F(O_LARGEFILE),
#endif  // O_LARGEFILE
#ifdef O_DIRECTORY
      F(O_DIRECTORY),
#endif  // O_DIRECTORY
#ifdef O_NOFOLLOW
      F(O_NOFOLLOW),
#endif  // O_NOFOLLOW
#ifdef O_NOATIME
      F(O_NOATIME),
#endif  // O_NOATIME
#ifdef O_CLOEXEC
      F(O_CLOEXEC),
#endif  // O_CLOEXEC
#undef F
  };

  std::vector<std::string_view> flag_names;
  int flags = l.flags_;
  for (const auto &[flag, name] : kFlagsToNames) {
    if ((flags & flag) == 0) continue;
    flag_names.emplace_back(name);
  }
  absl::Format(&sink, "%s", absl::StrJoin(flag_names, " | "));
}

}  // namespace dcfs

#endif  // DCFS_SYSCALLS_H_
