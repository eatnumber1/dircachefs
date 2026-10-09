#include "dcfs/umount_helper.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>

#include <cerrno>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/remount.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"

namespace dcfs {
namespace {

constexpr std::string_view kLockDir = "/run/dcfs";
// umount(8), run by this path: the helper runs as root or as the user who ran
// umount, with no PATH of its own; /bin is a symlink to /usr/bin on merged-usr
// systems.
constexpr char kUmountProgram[] = "/bin/umount";

// Makes /run/dcfs (and /run, on a system that has none yet) if it is not
// there.
absl::Status MakeLockDir() {
  for (std::string_view dir : {std::string_view("/run"), kLockDir}) {
    absl::Status made = syscalls::mkdirat(AT_FDCWD, dir, 0700);
    if (!made.ok() && StatusToErrno(made) != EEXIST) {
      return absl::StatusBuilder(made) << "creating " << dir;
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::string> CanonicalMountpoint(std::string_view mountpoint) {
  return syscalls::realpath(mountpoint);
}

absl::StatusOr<DaemonLock> HoldDaemonLock(std::string_view mountinfo,
                                          std::string_view mountpoint) {
  std::optional<std::string> device = DcfsMountDevice(mountinfo, mountpoint);
  if (!device.has_value()) {
    return InternalErrorBuilder()
           << "The FUSE mount of " << EscapeBytes(mountpoint)
           << " is not in /proc/self/mountinfo";
  }
  ABSL_RETURN_IF_ERROR(MakeLockDir());
  DaemonLock lock{.path = DaemonLockPath(*device)};
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor fd,
      syscalls::openat(AT_FDCWD, lock.path, O_RDWR | O_CREAT, 0600));
  if (absl::Status locked = syscalls::flock(*fd, LOCK_EX | LOCK_NB);
      !locked.ok()) {
    if (StatusToErrno(locked) == EWOULDBLOCK) {
      return FailedPreconditionErrorBuilder()
             << "The daemon of an earlier mount (device " << *device
             << ") is still shutting down (" << lock.path
             << " is locked); unmount with umount.fuse (dcfs's helper), which waits "
                "for it";
    }
    return absl::StatusBuilder(locked) << lock.path;
  }
  // Never closed: the kernel releases the lock when this process exits.
  (void)std::move(fd).Release();
  return lock;
}

void RemoveDaemonLockFile(const DaemonLock &lock) {
  if (absl::Status removed = syscalls::unlinkat(AT_FDCWD, lock.path, 0);
      !removed.ok() && StatusToErrno(removed) != ENOENT) {
    LOG(WARNING) << removed;
  }
}

absl::Status UmountAndWait(const UmountArgs &args) {
  // Which daemon, if any: the lock file is opened before the unmount, so that
  // what is waited for is the daemon of this mount, not one a later mount with
  // the same device number starts (the daemon removes the file as it shuts
  // down). No file, or not ours to open (a user's unmount): no daemon to wait
  // for.
  absl::StatusOr<FileDescriptor> lock = absl::NotFoundError("not a dcfs mount");
  if (absl::StatusOr<std::string> mountinfo = ReadMountinfo();
      mountinfo.ok()) {
    std::optional<std::string> device = DcfsMountDevice(*mountinfo, args.target);
    if (!device.has_value()) {
      // A dead mount cannot be resolved: the path as given first.
      if (absl::StatusOr<std::string> canonical =
              syscalls::realpath(args.target);
          canonical.ok()) {
        device = DcfsMountDevice(*mountinfo, *canonical);
      }
    }
    if (device.has_value()) {
      lock = syscalls::openat(AT_FDCWD, DaemonLockPath(*device), O_RDWR);
    }
  }

  std::vector<std::string> command = {"umount", "-i"};
  if (args.lazy) command.push_back("-l");
  if (args.force) command.push_back("-f");
  if (args.no_mtab) command.push_back("-n");
  if (args.read_only) command.push_back("-r");
  if (args.verbose) command.push_back("-v");
  command.push_back(args.target);
  ABSL_ASSIGN_OR_RETURN(pid_t child, syscalls::fork());
  if (child == 0) {
    std::vector<char *> argv;
    for (const std::string &arg : command) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    syscalls::execv(kUmountProgram, argv.data()).IgnoreError();
    syscalls::_exit(127);
  }
  int wait_status = 0;
  while (true) {
    absl::StatusOr<pid_t> reaped = syscalls::waitpid(child, &wait_status, 0);
    if (reaped.ok()) break;
    if (StatusToErrno(reaped.status()) != EINTR) {
      return absl::StatusBuilder(reaped.status())
             << "while waiting for " << kUmountProgram;
    }
  }
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    // umount said why on the user's terminal.
    return NativeMountError(
        WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 32, "");
  }
  if (args.lazy || !lock.ok()) return absl::OkStatus();
  // Blocks until the daemon exits (its exclusive lock goes with it). A signal
  // ends the wait: Ctrl-C, or systemd killing the helper; one that is not
  // fatal is retried.
  while (true) {
    absl::Status shared = syscalls::flock(**lock, LOCK_SH);
    if (shared.ok()) return absl::OkStatus();
    if (StatusToErrno(shared) != EINTR) return shared;
  }
}

}  // namespace dcfs
