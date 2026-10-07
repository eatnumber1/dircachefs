#ifndef DCFS_SYSCALLS_PROCESS_H_
#define DCFS_SYSCALLS_PROCESS_H_

#include <sys/types.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {
namespace syscalls {

// Process control, for test drivers (bench/): never linked into the daemon,
// which must not spawn or exec (tools/banned_symbols.txt). fork(2),
// execv(2) (returns only on failure), waitpid(2) (returns the pid it reaped,
// 0 with WNOHANG when nothing changed), kill(2), dup2(2) and _exit(2).
absl::StatusOr<pid_t> fork();
absl::Status execv(const char *path, char *const argv[]);
absl::StatusOr<pid_t> waitpid(pid_t pid, int *status, int options);
absl::Status kill(pid_t pid, int sig);
absl::Status dup2(int oldfd, int newfd);
[[noreturn]] void _exit(int status);

}  // namespace syscalls
}  // namespace dcfs

#endif  // DCFS_SYSCALLS_PROCESS_H_
