// SPDX-License-Identifier: MIT
#ifndef DCFS_SYSCALLS_PROCESS_H_
#define DCFS_SYSCALLS_PROCESS_H_

#include <sys/types.h>

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs::syscalls {

// Process control, for the benchmark driver (bench/) and the mount.dcfs
// wrapper, which forks the daemon (tools/banned_symbols.txt allows execv from
// this library only). fork(2), execv(2) (returns only on failure),
// waitpid(2) (returns the pid it
// reaped, 0 with WNOHANG when nothing changed), kill(2), dup2(2), pause(2)
// and _exit(2).
absl::StatusOr<pid_t> fork();
absl::Status execv(const char *path, char *const argv[]);
absl::StatusOr<pid_t> waitpid(pid_t pid, int *status, int options);
absl::Status kill(pid_t pid, int sig);
absl::Status dup2(int oldfd, int newfd);
absl::Status pause();  // returns (EINTR) once a signal handler ran
[[noreturn]] void _exit(int status);

// Runs a command and waits for it, as system(3) does without a shell:
// `argv[0]` is the program's path (execv: no PATH search), the rest its
// arguments; `argv` is not empty. OK if it exits 0; otherwise the Status names
// the command and how it ended (its exit status, 127 when the program could
// not be executed, or the signal). Its output goes to this process's.
absl::Status system(const std::vector<std::string> &argv);

}  // namespace dcfs::syscalls

#endif  // DCFS_SYSCALLS_PROCESS_H_
