// SPDX-License-Identifier: MIT
#include "dcfs/syscalls_process.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "absl/container/fixed_array.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/status.h"

namespace dcfs::syscalls {

absl::StatusOr<pid_t> fork() {
  pid_t pid = ::fork();
  if (pid == -1) return ErrnoToStatus(errno, "fork");
  return pid;
}

absl::Status execv(std::string_view path, std::span<const std::string> argv) {
  const std::string path_str(path);
  // execv(2) takes a mutable array and does not change what it points to.
  absl::FixedArray<char *> args(argv.size() + 1);
  for (size_t i = 0; i < argv.size(); ++i) {
    args[i] = const_cast<char *>(argv[i].c_str());
  }
  args[argv.size()] = nullptr;
  ::execv(path_str.c_str(), args.data());
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

}  // namespace dcfs::syscalls
