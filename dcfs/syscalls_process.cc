// SPDX-License-Identifier: MIT
#include "dcfs/syscalls_process.h"

#include <signal.h>
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers): WIFEXITED lives here
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"

namespace dcfs::syscalls {

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

absl::Status pause() {
  ::pause();
  return ErrnoToStatus(errno, "pause");
}

void _exit(int status) { ::_exit(status); }

absl::Status system(const std::vector<std::string> &argv) {
  RET_CHECK(!argv.empty()) << "system of no command";
  std::vector<char *> args;
  args.reserve(argv.size() + 1);
  for (const std::string &arg : argv) {
    args.push_back(const_cast<char *>(arg.c_str()));
  }
  args.push_back(nullptr);
  ASSIGN_OR_RETURN(pid_t pid, fork());
  if (pid == 0) {
    // Only returns on failure; the status of that is the exit status, 127.
    execv(args[0], args.data()).IgnoreError();
    _exit(127);
  }
  int wait_status = 0;
  RETURN_IF_ERROR(waitpid(pid, &wait_status, 0).status());
  if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
    return absl::OkStatus();
  }
  std::string how =
      WIFEXITED(wait_status)
          ? absl::StrCat("exited with status ", WEXITSTATUS(wait_status))
          : absl::StrCat("was killed by signal ", WTERMSIG(wait_status));
  return InternalErrorBuilder()
         << "`" << absl::StrJoin(argv, " ") << "` " << how;
}

}  // namespace dcfs::syscalls
