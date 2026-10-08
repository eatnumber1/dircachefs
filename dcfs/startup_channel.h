#ifndef DCFS_STARTUP_CHANNEL_H_
#define DCFS_STARTUP_CHANNEL_H_

// How mount.dcfs daemonizes and learns that the daemon is serving (phase 15,
// decision 13; docs/design.md "Daemonization"). ForkDaemon forks before any
// thread, the database or the mount exists. The child becomes the daemon
// (new session, working directory "/", stdio on /dev/null) and holds the
// reporting end of a socket pair; the parent waits on the other end for the
// daemon's StartupReport and returns the exit status the wrapper exits with.
// So the wrapper returns only once dcfs answered FUSE_INIT ("ready") or a
// failure, with its message, is known.

#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"

namespace dcfs {

// The daemon's end. Default-constructed it is inert (a foreground run has no
// one to tell). Reports at most once, then closes its end, so the waiting
// wrapper sees the end of the report.
class StartupReporter {
 public:
  StartupReporter() = default;
  explicit StartupReporter(FileDescriptor channel);

  // Nobody has been told yet.
  bool pending() const { return channel_.valid(); }
  // "Ready": dcfs answered FUSE_INIT. No-op unless pending.
  void Ready();
  // The start failed with `status`: its message and ExitStatusFor. No-op
  // unless pending (a failure after "ready" is the daemon's own to log).
  void Fail(const absl::Status &status);

 private:
  FileDescriptor channel_;
};

struct DaemonFork {
  // Set in the parent only: the wrapper's exit status, after the daemon's
  // report (a failure's message is already on the wrapper's stderr).
  std::optional<int> parent_exit_status;
  // The daemon's (child's) reporter.
  StartupReporter reporter;
};

// Forks; see the top of this file.
absl::StatusOr<DaemonFork> ForkDaemon();

}  // namespace dcfs

#endif  // DCFS_STARTUP_CHANNEL_H_
