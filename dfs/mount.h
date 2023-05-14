#ifndef DFS_MOUNT_H_
#define DFS_MOUNT_H_

#include <string>

#include "absl/status/statusor.h"
#include "absl/status/status.h"

namespace dfs {

// Forward declarations for syscalls.h
class Mount;
namespace syscalls {
absl::StatusOr<Mount> mount(
  const char *source, std::string target, const char *filesystemtype,
  unsigned long mountflags, const void *data);
}  // namespace syscalls

class Mount {
 public:
  Mount() = default;
  ~Mount();

  const std::string &GetTarget() const;
  std::string Release() &&;

  Mount(Mount &&);
  Mount(const Mount &) = delete;
  Mount &operator=(Mount &&);
  Mount &operator=(const Mount &) = delete;

 private:
  friend absl::StatusOr<Mount> syscalls::mount(
    const char *source, std::string target, const char *filesystemtype,
    unsigned long mountflags, const void *data);

  Mount(std::string target);

  // Empty string when moved-from.
  std::string target_;
};

}  // namespace dfs

#endif  // DFS_MOUNT_H_
