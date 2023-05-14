#ifndef DFS_SYSCALLS_H_
#define DFS_SYSCALLS_H_

#include <string>
#include <unistd.h>
#include <sys/stat.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dfs/fd.h"
#include "dfs/mount.h"

namespace dfs {
namespace syscalls {

absl::Status close(dfs::FileDescriptor fd);

absl::StatusOr<dfs::FileDescriptor> open(
    const char *pathname, int flags, mode_t mode = 0);

absl::StatusOr<size_t> read(int fd, void *buf, size_t count);

absl::StatusOr<dfs::Mount> mount(
    const char *source, std::string target, const char *filesystemtype,
    unsigned long mountflags = 0, const void *data = nullptr);

absl::Status umount(dfs::Mount mount, int flags = 0);

absl::StatusOr<struct stat> stat(const char *pathname);

}  // namespace syscalls
}  // namespace dfs

#endif  // DFS_SYSCALLS_H_
