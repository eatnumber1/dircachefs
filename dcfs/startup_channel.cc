#include "dcfs/startup_channel.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/fd.h"
#include "dcfs/fork_split.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"

namespace dcfs {
namespace {

// What waitpid's `wait_status` says, for the message of a daemon that died
// before reporting.
std::string DescribeWaitStatus(int wait_status) {
  if (WIFSIGNALED(wait_status)) {
    return absl::StrCat("killed by signal ", WTERMSIG(wait_status));
  }
  return absl::StrCat("exit status ", WEXITSTATUS(wait_status));
}

// Everything the reporting end sends, until it closes.
absl::StatusOr<std::string> ReadReport(int fd) {
  std::string bytes;
  char buf[4096];
  while (true) {
    ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::read(fd, buf, sizeof(buf)));
    if (n == 0) return bytes;
    bytes.append(buf, n);
  }
}

// The daemon's own half of the fork: its own session, no controlling
// terminal, nothing held on the directory the wrapper ran in, stdio on
// /dev/null (a daemon that kept the wrapper's pipes open would keep
// whatever waits for the wrapper's output waiting too).
absl::Status DetachDaemon() {
  ABSL_RETURN_IF_ERROR(syscalls::setsid().status());
  ABSL_RETURN_IF_ERROR(syscalls::chdir("/"));
  ABSL_ASSIGN_OR_RETURN(FileDescriptor null,
                        syscalls::openat(AT_FDCWD, "/dev/null", O_RDWR));
  for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    ABSL_RETURN_IF_ERROR(syscalls::dup2(*null, fd));
  }
  return absl::OkStatus();
}

}  // namespace

StartupReporter::StartupReporter(FileDescriptor channel)
    : channel_(std::move(channel)) {}

void StartupReporter::Ready() {
  if (!pending()) return;
  const std::string bytes = EncodeStartupReport({.ready = true});
  // MSG_NOSIGNAL: a wrapper that is gone is not worth dying for.
  syscalls::send(*channel_, bytes.data(), bytes.size(), MSG_NOSIGNAL)
      .status()
      .IgnoreError();
  channel_.Close().IgnoreError();
}

void StartupReporter::Fail(const absl::Status &status) {
  if (!pending()) return;
  const std::string bytes = EncodeStartupReport(
      {.ready = false,
       .exit_status = ExitStatusFor(status),
       .message = status.ToString()});
  syscalls::send(*channel_, bytes.data(), bytes.size(), MSG_NOSIGNAL)
      .status()
      .IgnoreError();
  channel_.Close().IgnoreError();
}

namespace {

// The wrapper's half of the fork: the daemon's report, then the wrapper's
// exit (its message, if any, on stderr). Exits here rather than return: see
// fork_split.h.
[[noreturn]] void AwaitReportAndExit(FileDescriptor parent_end, pid_t child) {
  absl::StatusOr<std::string> bytes = ReadReport(*parent_end);
  if (!bytes.ok()) bytes = std::string();
  StartupReport report = DecodeStartupReport(*bytes);
  if (report.ready) std::exit(0);
  if (bytes->empty()) {
    // Died without a word: say how.
    int wait_status = 0;
    if (syscalls::waitpid(child, &wait_status, 0).ok()) {
      report.message =
          absl::StrCat(report.message, " (", DescribeWaitStatus(wait_status),
                       ")");
    }
  }
  LOG(ERROR) << report.message;
  std::exit(report.exit_status);
}

// The daemon's half: it holds the reporting end.
StartupReporter BecomeDaemon(FileDescriptor parent_end,
                             FileDescriptor child_end) {
  parent_end.Close().IgnoreError();
  StartupReporter reporter(std::move(child_end));
  // A daemon that cannot detach says so through the channel, like any other
  // failure to start.
  if (absl::Status detached = DetachDaemon(); !detached.ok()) {
    reporter.Fail(detached);
    syscalls::_exit(ExitStatusFor(detached));
  }
  return reporter;
}

}  // namespace

absl::StatusOr<StartupReporter> ForkDaemon() {
  ABSL_ASSIGN_OR_RETURN(
      auto channel, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  auto &[parent_end, child_end] = channel;
  return ForkSplit(
      [&] {
        return BecomeDaemon(std::move(parent_end), std::move(child_end));
      },
      [&](pid_t child) {
        child_end.Close().IgnoreError();
        AwaitReportAndExit(std::move(parent_end), child);
      });
}

}  // namespace dcfs
