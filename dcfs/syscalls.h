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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
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
// copy_file_range(2) of up to `len` bytes from `fd_in` at `off_in` to
// `fd_out` at `off_out` (neither file's offset moves); returns how many
// bytes it copied (0 at the end of `fd_in`).
absl::StatusOr<size_t> copy_file_range(int fd_in, off_t off_in, int fd_out,
                                       off_t off_out, size_t len,
                                       unsigned int flags);
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
// (backing.cc reads the current ids that way).
uid_t setfsuid(uid_t uid);
gid_t setfsgid(gid_t gid);

// umask(2): sets the process's file mode creation mask and returns the
// previous one. Cannot fail. Process-wide (the fs_struct is shared by every
// thread not created with CLONE_FS unshared), unlike the credentials above.
mode_t umask(mode_t mask);

// setgroups(2) as the raw system call, which on Linux changes only the
// CALLING THREAD's supplementary groups. Not glibc's setgroups(), which
// broadcasts the change to every thread of the process (the POSIX
// per-process semantics, implemented with a signal to each thread).
absl::Status setgroups(std::span<const gid_t> groups);

// getgroups(2): the calling thread's supplementary groups; with size 0 only
// the count.
absl::StatusOr<int> getgroups(int size, gid_t *list);

// getrlimit(2), setrlimit(2), flock(2).
absl::StatusOr<struct rlimit> getrlimit(int resource);
absl::Status setrlimit(int resource, const struct rlimit &limit);
absl::Status flock(int fd, int operation);

}  // namespace syscalls

// Implementation below here

namespace syscalls {

absl::StatusOr<int> ioctl(int fd, int op, auto &&... args) {
  int rc = ::ioctl(fd, op, std::forward<decltype(args)>(args)...);
  if (rc == -1) return dcfs::ErrnoToStatus(errno, "ioctl");
  return rc;
}

}  // namespace syscalls

}  // namespace dcfs

#endif  // DCFS_SYSCALLS_H_
