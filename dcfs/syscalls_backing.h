// The backing-reaching wrappers of dcfs::syscalls: every syscall that takes
// a backing fd, file handle or name (docs/style.md 1.8), plus mount and
// umount2 for the tests. Only backing.cc, its lower layers, startup and the
// tests may depend on //dcfs:syscalls_backing: //tools:syscalls_backing_users_test
// compares the dependents with a golden list. The process-local wrappers are
// in syscalls.h.
#ifndef DCFS_SYSCALLS_BACKING_H_
#define DCFS_SYSCALLS_BACKING_H_

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"

namespace dcfs {
namespace syscalls {

// O_CLOEXEC is unconditionally added.
absl::StatusOr<FileDescriptor> openat(
    int dirfd, std::string_view pathname, int flags = 0,
    mode_t mode = 0);

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

absl::StatusOr<int> ioctl(int fd, unsigned long request, auto &&... args);

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
// readlinkat(2): one call; returns how many bytes it wrote to `buf`. A
// result equal to `size` may be truncated (backing.cc grows the buffer).
absl::StatusOr<size_t> readlinkat(int dirfd, std::string_view path, char *buf,
                                  size_t size);
// fgetxattr(2)/flistxattr(2): one call each, returning the size. A null
// `value`/`list` with size 0 asks for the size needed. The size-then-read
// loop (the value can grow in between) is backing.cc's.
absl::StatusOr<size_t> fgetxattr(int fd, std::string_view name, void *value,
                                 size_t size);
absl::StatusOr<size_t> flistxattr(int fd, char *list, size_t size);
absl::Status fsetxattr(int fd, std::string_view name,
                       std::span<const uint8_t> value, int flags);
absl::Status fremovexattr(int fd, std::string_view name);
// The path-following xattr, chmod and utimes calls. backing.cc uses them on
// "/proc/self/fd/<fd>" to reach an O_PATH descriptor's object.
absl::StatusOr<size_t> getxattr(std::string_view path, std::string_view name,
                                void *value, size_t size);
absl::StatusOr<size_t> listxattr(std::string_view path, char *list,
                                 size_t size);
absl::Status setxattr(std::string_view path, std::string_view name,
                      std::span<const uint8_t> value, int flags);
absl::Status removexattr(std::string_view path, std::string_view name);
absl::Status fchmodat(int dirfd, std::string_view path, mode_t mode,
                      int flags);
absl::Status utimensat(int dirfd, std::string_view path,
                       const struct timespec times[2], int flags);

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
// copy_file_range(2) of up to `len` bytes from `fd_in` at `off_in` to
// `fd_out` at `off_out` (neither file's offset moves); returns how many
// bytes it copied (0 at the end of `fd_in`).
absl::StatusOr<size_t> copy_file_range(int fd_in, off_t off_in, int fd_out,
                                       off_t off_out, size_t len,
                                       unsigned int flags);
absl::StatusOr<size_t> pwrite(int fd, const void *buf, size_t count,
                              off_t offset);
absl::StatusOr<size_t> write(int fd, const void *buf, size_t count);

// mount(2) and umount2(2). Null `source`, `fstype` and `data` are allowed.
absl::Status mount(const char *source, std::string_view target,
                   const char *fstype, unsigned long flags, const void *data);
absl::Status umount2(std::string_view target, int flags);

}  // namespace syscalls

// Implementation below here

namespace syscalls {

absl::StatusOr<int> ioctl(int fd, unsigned long request, auto &&... args) {
  int rc = ::ioctl(fd, request, std::forward<decltype(args)>(args)...);
  if (rc == -1) return dcfs::ErrnoToStatus(errno, "ioctl");
  return rc;
}

}  // namespace syscalls

}  // namespace dcfs

#endif  // DCFS_SYSCALLS_BACKING_H_
