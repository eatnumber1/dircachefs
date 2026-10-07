#include "dcfs/syscalls_process.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/status.h"

namespace dcfs {
namespace syscalls {

absl::StatusOr<pid_t> fork() {
  pid_t pid = ::fork();
  if (pid == -1) return ErrnoToStatus(errno, "fork");
  return pid;
}

absl::Status execv(const char *path, char *const argv[]) {
  ::execv(path, argv);
  return ErrnoToStatus(errno, absl::StrCat("execv(", path, ")"));
}

absl::StatusOr<pid_t> waitpid(pid_t pid, int *status, int options) {
  pid_t rc = ::waitpid(pid, status, options);
  if (rc == -1) return ErrnoToStatus(errno, absl::StrCat("waitpid(", pid, ")"));
  return rc;
}

absl::Status kill(pid_t pid, int sig) {
  if (::kill(pid, sig) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("kill(", pid, ", ", sig, ")"));
  }
  return absl::OkStatus();
}

absl::Status dup2(int oldfd, int newfd) {
  if (::dup2(oldfd, newfd) == -1) {
    return ErrnoToStatus(errno, absl::StrCat("dup2(", oldfd, ", ", newfd, ")"));
  }
  return absl::OkStatus();
}

void _exit(int status) { ::_exit(status); }

}  // namespace syscalls
}  // namespace dcfs
