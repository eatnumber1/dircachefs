// SPDX-License-Identifier: MIT
#include "tools/process_runner.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "tools/test_first.h"

namespace dcfs::test_first {
namespace {

// The path of the program `name`: itself when it has a '/', else the first
// executable regular file of that name in a directory of `path`.
absl::StatusOr<std::string> Resolve(std::string_view name,
                                    std::string_view path) {
  if (absl::StrContains(name, '/')) return std::string(name);
  for (std::string_view directory : absl::StrSplit(path, ':')) {
    std::string candidate =
        absl::StrCat(directory.empty() ? "." : directory, "/", name);
    absl::StatusOr<struct stat> found = syscalls::stat(candidate);
    if (found.ok() && S_ISREG(found->st_mode) && (found->st_mode & 0111) != 0) {
      return candidate;
    }
  }
  return NotFoundErrorBuilder()
         << name << " is not in any directory of PATH (" << path << ")";
}

// A wait status as a shell reports it. The low 7 bits are the signal that
// killed the child, 0 when it exited, and the next 8 bits its exit code
// (waitpid(2); the macros WIFEXITED and WEXITSTATUS are these shifts).
int ExitCodeOf(int wait_status) {
  const int signal = wait_status & 0x7f;
  return signal == 0 ? (wait_status >> 8) & 0xff : 128 + signal;
}

std::string ReadAll(int fd) {
  std::string text;
  char buffer[4096];
  while (true) {
    absl::StatusOr<size_t> n = syscalls::read(fd, buffer, sizeof(buffer));
    if (!n.ok() || *n == 0) break;
    text.append(buffer, *n);
  }
  return text;
}

}  // namespace

absl::StatusOr<CommandResult> ProcessRunner::Run(
    std::span<const std::string> argv) {
  if (argv.empty()) return InvalidArgumentErrorBuilder() << "no program";
  ASSIGN_OR_RETURN(std::string program, Resolve(argv.front(), path_));
  ASSIGN_OR_RETURN(auto pipe, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  ASSIGN_OR_RETURN(pid_t child, syscalls::fork());
  if (child == 0) {
    if (!syscalls::dup2(*pipe.second, STDOUT_FILENO).ok() ||
        !syscalls::dup2(*pipe.second, STDERR_FILENO).ok()) {
      syscalls::_exit(126);
    }
    syscalls::execv(program, argv).IgnoreError();
    syscalls::_exit(127);
  }
  pipe.second.Close().IgnoreError();
  CommandResult result = {.output = ReadAll(*pipe.first)};
  int wait_status = 0;
  while (true) {
    absl::StatusOr<pid_t> reaped = syscalls::waitpid(child, &wait_status, 0);
    if (reaped.ok()) break;
    if (StatusToErrno(reaped.status()) != EINTR) {
      return absl::StatusBuilder(reaped.status())
             << "while waiting for " << program;
    }
  }
  result.exit_code = ExitCodeOf(wait_status);
  return result;
}

}  // namespace dcfs::test_first
