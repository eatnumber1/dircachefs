// SPDX-License-Identifier: MIT
#include "dcfs/subprocess.h"

#include <sys/socket.h>
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
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_process.h"

namespace dcfs {

absl::StatusOr<std::string> ReadAll(int fd) {
  std::string text;
  char buffer[4096];
  while (true) {
    ASSIGN_OR_RETURN(size_t n, syscalls::read_own(fd, buffer, sizeof(buffer)));
    if (n == 0) return text;
    text.append(buffer, n);
  }
}

namespace {

// In the child: `capture`'s streams become `fd`.
absl::Status Redirect(Capture capture, int fd) {
  if (capture == Capture::kNothing) return absl::OkStatus();
  RETURN_IF_ERROR(syscalls::dup2(fd, STDOUT_FILENO));
  if (capture == Capture::kOutput) {
    RETURN_IF_ERROR(syscalls::dup2(fd, STDERR_FILENO));
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<ProgramRun> RunProgram(std::string_view path,
                                      std::span<const std::string> argv,
                                      Capture capture) {
  std::pair<FileDescriptor, FileDescriptor> pipe;
  if (capture != Capture::kNothing) {
    ASSIGN_OR_RETURN(pipe, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  }
  ASSIGN_OR_RETURN(pid_t child, syscalls::fork());
  if (child == 0) {
    // dup2 can fail even for open descriptors (EBUSY, racing an open of the
    // new one, on Linux): the program does not run, as the doc says.
    if (!Redirect(capture, *pipe.second).ok()) syscalls::_exit(126);
    syscalls::execv(path, argv).IgnoreError();
    syscalls::_exit(127);
  }
  ProgramRun run;
  absl::Status read = absl::OkStatus();
  if (capture != Capture::kNothing) {
    pipe.second.Close().IgnoreError();
    absl::StatusOr<std::string> output = ReadAll(*pipe.first);
    if (output.ok()) {
      run.output = *std::move(output);
    } else {
      read = output.status();
      // A child with output pending is blocked writing it; it ends only when
      // nobody can read it any more (EPIPE).
      pipe.first.Close().IgnoreError();
    }
  }
  while (true) {
    absl::StatusOr<pid_t> reaped =
        syscalls::waitpid(child, &run.wait_status, 0);
    if (reaped.ok()) break;
    if (StatusToErrno(reaped.status()) != EINTR) {
      return absl::StatusBuilder(reaped.status())
             << "while waiting for " << path;
    }
  }
  RETURN_IF_ERROR(read);
  return run;
}

}  // namespace dcfs
