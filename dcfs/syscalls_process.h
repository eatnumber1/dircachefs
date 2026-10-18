// SPDX-License-Identifier: MIT
#ifndef DCFS_SYSCALLS_PROCESS_H_
#define DCFS_SYSCALLS_PROCESS_H_

#include <sys/types.h>

#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs::syscalls {

// Process control, for the benchmark driver (bench/) and the mount.dcfs
// wrapper, which forks the daemon (tools/banned_symbols.txt allows execv from
// this library only). fork(2), execv(2) (returns only on failure; `argv[0]`
// is the program's name for itself, not searched for), waitpid(2) (returns
// the pid it reaped, 0 with WNOHANG when nothing changed), kill(2), dup2(2),
// pause(2) and _exit(2).
absl::StatusOr<pid_t> fork();
absl::Status execv(std::string_view path, std::span<const std::string> argv);
absl::StatusOr<pid_t> waitpid(pid_t pid, int *status, int options);
absl::Status kill(pid_t pid, int sig);
absl::Status dup2(int oldfd, int newfd);
absl::Status pause();  // returns (EINTR) once a signal handler ran
[[noreturn]] void _exit(int status);

}  // namespace dcfs::syscalls

#endif  // DCFS_SYSCALLS_PROCESS_H_
