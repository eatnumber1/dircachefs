#ifndef DCFS_SYSCALLS_H_
#define DCFS_SYSCALLS_H_

#include <cerrno>
#include <array>
#include <cstddef>
#include <fcntl.h>
#include <linux/openat2.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

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
absl::StatusOr<FileDescriptor> openat(
    int dirfd, std::string_view pathname, int flags = 0,
    mode_t mode = 0);

// O_CLOEXEC is unconditionally added to how.flags.
absl::StatusOr<FileDescriptor> openat2(
    int dirfd, std::string_view pathname, open_how how,
    size_t size);

absl::StatusOr<size_t> read(int fd, void *buf, size_t count);

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

// New fd-based wrappers for file operations
absl::StatusOr<struct statx> statx(int dirfd, std::string_view path, int flags,
                                    unsigned int mask);
absl::StatusOr<struct statfs> fstatfs(int fd);
absl::StatusOr<std::string> readlinkat(int dirfd, std::string_view path);
absl::StatusOr<std::string> fgetxattr(int fd, std::string_view name);
absl::StatusOr<std::vector<std::string>> flistxattr(int fd);
absl::Status fsetxattr(int fd, std::string_view name,
                       std::span<const uint8_t> value, int flags);
absl::Status fremovexattr(int fd, std::string_view name);
absl::StatusOr<FileDescriptor> ReopenPathFd(int fd, int flags);

// listxattr(2)/getxattr(2) on "/proc/self/fd/<fd>" -- the path-following
// variants. The magic link resolves to exactly the object `fd` refers to,
// even when that object is itself a symlink (it is not followed further),
// so these work on an O_PATH fd for any file type: unlike flistxattr and
// fgetxattr they need neither a non-O_PATH fd nor an open() of the object,
// which for a FIFO or device could block or have side effects.
absl::StatusOr<std::vector<std::string>> listxattr_opath(int fd);
absl::StatusOr<std::string> getxattr_opath(int fd, std::string_view name);

// setxattr(2)/removexattr(2) on "/proc/self/fd/<fd>" -- the same
// path-following trick as listxattr_opath/getxattr_opath, for the write
// side. Used for symlinks and other special files, which fsetxattr(2)/
// fremovexattr(2) cannot reach directly (they need a non-O_PATH fd) and
// which reopening for one could block on or have a side effect on.
absl::Status setxattr_opath(int fd, std::string_view name,
                            std::span<const uint8_t> value, int flags);
absl::Status removexattr_opath(int fd, std::string_view name);

// fchmodat(AT_FDCWD, "/proc/self/fd/<fd>", mode, 0) -- as listxattr_opath,
// for fchmod(2), which (like flistxattr/fgetxattr) rejects O_PATH fds.
// Intended for FIFOs, sockets and devices, which reopening for a real fd
// (to use plain fchmod) could block on or have side effects on. Not
// called for a symlink in practice: Linux has no way to chmod a
// symlink's own mode (there is no lchmod syscall) and this path
// correctly surfaces that as EOPNOTSUPP (verified experimentally) rather
// than silently chmoding the target, but callers reject that case
// explicitly before ever reaching this function.
absl::Status fchmod_opath(int fd, mode_t mode);

// utimensat(AT_FDCWD, "/proc/self/fd/<fd>", times, 0) -- as fchmod_opath,
// for futimens(2) on the same set of file types, plus symlinks: verified
// experimentally that this sets a symlink's own timestamp (not its
// target's), matching the magic link's usual "resolves to exactly the
// object the fd refers to, without following further" behavior.
absl::Status futimens_opath(int fd, const struct timespec times[2]);

// fcntl(fd, F_DUPFD_CLOEXEC, 0).
absl::StatusOr<FileDescriptor> dup(int fd);
absl::Status linkat(int olddirfd, std::string_view oldpath, int newdirfd,
                    std::string_view newpath, int flags);
absl::Status unlinkat(int dirfd, std::string_view path, int flags);
absl::Status renameat2(int olddirfd, std::string_view oldpath, int newdirfd,
                       std::string_view newpath, unsigned int flags);
absl::Status mkdirat(int dirfd, std::string_view path, mode_t mode);
absl::Status mknodat(int dirfd, std::string_view path, mode_t mode, dev_t dev);
absl::Status symlinkat(std::string_view target, int newdirfd,
                       std::string_view linkpath);
absl::Status fchmod(int fd, mode_t mode);
absl::Status fchownat(int dirfd, std::string_view path, uid_t owner,
                      gid_t group, int flags);
absl::Status futimens(int fd, const struct timespec times[2]);
absl::Status ftruncate(int fd, off_t length);
absl::Status fsync(int fd);
absl::Status fdatasync(int fd);
// syncfs(2): writes back and flushes the whole filesystem `fd` is on.
absl::Status syncfs(int fd);
absl::Status fallocate(int fd, int mode, off_t offset, off_t len);
absl::StatusOr<size_t> pread(int fd, void *buf, size_t count, off_t offset);
absl::StatusOr<size_t> pwrite(int fd, const void *buf, size_t count,
                              off_t offset);
absl::StatusOr<size_t> write(int fd, const void *buf, size_t count);

// --- Per-thread filesystem credentials (see backing.cc's AsCaller) ---------
//
// setfsuid(2)/setfsgid(2): set the calling thread's filesystem uid/gid and
// return the PREVIOUS value. They report no error: an unacceptable id is
// silently ignored (the previous value is returned either way), so a caller
// must read the value back to know whether a switch took effect -- passing
// an invalid id such as -1 changes nothing and returns the current value
// (that is what fsuid()/fsgid() below do).
uid_t setfsuid(uid_t uid);
gid_t setfsgid(gid_t gid);
uid_t fsuid();
gid_t fsgid();

// umask(2): sets the process's file mode creation mask and returns the
// previous one. Cannot fail. Process-wide (the fs_struct is shared by every
// thread not created with CLONE_FS unshared), unlike the credentials above.
mode_t umask(mode_t mask);

// setgroups(2) as the raw system call, which on Linux changes only the
// CALLING THREAD's supplementary groups. Not glibc's setgroups(), which
// broadcasts the change to every thread of the process (the POSIX
// per-process semantics, implemented with a signal to each thread).
absl::Status setgroups_thread(std::span<const gid_t> groups);

// getgroups(2): the calling thread's supplementary groups.
absl::StatusOr<std::vector<gid_t>> getgroups();

}  // namespace syscalls

absl::StatusOr<uint32_t> GetInodeGeneration(int fd);

struct LogOpenFlags {
 public:
  explicit LogOpenFlags(int flags);

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const LogOpenFlags &l);

 private:
  int flags_ = 0;
};

// Implementation below here

namespace syscalls {

absl::StatusOr<int> ioctl(int fd, int op, auto &&... args) {
  int rc = ::ioctl(fd, op, std::forward<decltype(args)>(args)...);
  if (rc == -1) return dcfs::ErrnoToStatus(errno, "ioctl");
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
