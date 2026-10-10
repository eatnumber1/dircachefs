#include "dcfs/umount_helper.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
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
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
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

// umount(8), run by this path: the helper runs as root or as the user who ran
// umount, with no PATH of its own; /bin is a symlink to /usr/bin on merged-usr
// systems.
constexpr char kUmountProgram[] = "/bin/umount";
// fusectl, where the kernel keeps a directory per FUSE connection, named by the
// connection's device number (the minor, for an anonymous device).
constexpr std::string_view kFuseConnections = "/sys/fs/fuse/connections";

// Makes `dir` (the one level: /run is the system's) if it is not there.
absl::Status MakeLockDir(std::string_view dir) {
  absl::Status made = syscalls::mkdirat(AT_FDCWD, dir, 0700);
  if (!made.ok() && StatusToErrno(made) != EEXIST) {
    return absl::StatusBuilder(made) << "creating " << dir;
  }
  return absl::OkStatus();
}

bool SameFile(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Takes `fd`'s lock, `operation` (LOCK_EX or LOCK_SH), blocking until it is
// granted. `retry_signals`: the helper retries a signal that was not fatal; the
// daemon's own handlers are those of a shutdown request, which ends the wait.
absl::Status BlockingLock(int fd, int operation, bool retry_signals,
                          std::string_view what) {
  while (true) {
    absl::Status locked = syscalls::flock(fd, operation);
    if (locked.ok()) return locked;
    if (StatusToErrno(locked) == EINTR && retry_signals) continue;
    return absl::StatusBuilder(locked) << what;
  }
}

}  // namespace

absl::StatusOr<std::string> CanonicalMountpoint(std::string_view mountpoint) {
  return syscalls::realpath(mountpoint);
}

absl::StatusOr<DaemonLock> HoldDaemonLock(std::string_view mountinfo,
                                          std::string_view mountpoint,
                                          std::string_view dir) {
  std::optional<std::string> device =
      MountDeviceOfFuseDcfs(mountinfo, mountpoint);
  if (!device.has_value()) {
    return InternalErrorBuilder()
           << "The FUSE mount of " << EscapeBytes(mountpoint)
           << " is not in /proc/self/mountinfo";
  }
  ABSL_RETURN_IF_ERROR(MakeLockDir(dir));
  DaemonLock lock{.path = DaemonLockPath(*device, dir)};
  bool announced = false;
  while (true) {
    ABSL_ASSIGN_OR_RETURN(
        FileDescriptor fd,
        syscalls::openat(AT_FDCWD, lock.path, O_RDWR | O_CREAT, 0600));
    if (absl::Status locked = syscalls::flock(*fd, LOCK_EX | LOCK_NB);
        !locked.ok()) {
      if (StatusToErrno(locked) != EWOULDBLOCK) {
        return absl::StatusBuilder(locked) << "locking " << lock.path;
      }
      if (!announced) {
        LOG(INFO) << "The daemon of an earlier mount (device " << *device
                  << ") is still shutting down (" << lock.path
                  << " is locked); waiting for it to exit";
        announced = true;
      }
      ABSL_RETURN_IF_ERROR(BlockingLock(*fd, LOCK_EX, /*retry_signals=*/false,
                                        "waiting for the lock " + lock.path));
    }
    // The holder removes the file before it exits: the file this lock is on
    // may no longer be the one at the path.
    ABSL_ASSIGN_OR_RETURN(struct stat held, syscalls::fstat(*fd));
    absl::StatusOr<struct stat> named = syscalls::fstatat(AT_FDCWD, lock.path);
    if (named.ok() && SameFile(held, *named)) {
      // Never closed: the kernel releases the lock when this process exits.
      (void)std::move(fd).Release();
      return lock;
    }
    if (!named.ok() && StatusToErrno(named.status()) != ENOENT) {
      return absl::StatusBuilder(named.status()) << lock.path;
    }
  }
}

void RemoveDaemonLockFile(const DaemonLock &lock) {
  if (absl::Status removed = syscalls::unlinkat(AT_FDCWD, lock.path, 0);
      !removed.ok() && StatusToErrno(removed) != ENOENT) {
    LOG(WARNING) << removed;
  }
}

absl::Status UmountAndWait(const UmountArgs &args) {
  // Whether, and for whom, to wait: a dcfs mount, not lazily unmounted, in
  // this namespace. The lock file and the fusectl directory are opened before
  // the unmount (the daemon removes the one, the kernel the other, as the
  // mount ends), and the lock file is the daemon of this mount, never one a
  // later mount with the same device number starts.
  std::optional<FileDescriptor> lock;
  std::optional<FileDescriptor> connection;
  if (!args.lazy && !args.other_namespace) {
    if (absl::StatusOr<std::string> mountinfo = ReadMountinfo();
        mountinfo.ok()) {
      std::optional<MountEntry> entry =
          TopmostMountEntry(*mountinfo, args.target);
      if (!entry.has_value()) {
        // Not as given (a symbolic link, a relative path): its canonical form.
        // Only when nothing is mounted at the path as it stands: resolving the
        // path of another filesystem's mount point can ask a server that is
        // not answering.
        if (absl::StatusOr<std::string> canonical =
                syscalls::realpath(args.target);
            canonical.ok()) {
          entry = TopmostMountEntry(*mountinfo, *canonical);
        }
      }
      if (entry.has_value() && entry->fstype == "fuse.dcfs") {
        if (absl::StatusOr<FileDescriptor> opened = syscalls::openat(
                AT_FDCWD, DaemonLockPath(entry->device), O_RDWR);
            opened.ok()) {
          lock = *std::move(opened);
        }
        size_t colon = entry->device.find(':');
        if (absl::StatusOr<FileDescriptor> opened = syscalls::openat(
                AT_FDCWD,
                absl::StrCat(kFuseConnections, "/",
                             entry->device.substr(colon + 1)),
                O_PATH | O_DIRECTORY);
            opened.ok()) {
          connection = *std::move(opened);
        }
      }
    }
  }

  std::vector<std::string> command = {"umount", "-i"};
  command.insert(command.end(), args.forwarded.begin(), args.forwarded.end());
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
  if (!lock.has_value() || !connection.has_value()) return absl::OkStatus();
  // Did this unmount end the superblock? Its fusectl directory is gone (no
  // links) if so: a bind mount, an rbind, a copy in another namespace or `-r`
  // on a busy mount leave it, and the daemon serving it.
  ABSL_ASSIGN_OR_RETURN(struct stat dir, syscalls::fstat(**connection));
  if (dir.st_nlink != 0) return absl::OkStatus();
  // Blocks until the daemon exits (its exclusive lock goes with it). A signal
  // ends the wait: Ctrl-C, or systemd killing the helper; one that is not
  // fatal is retried.
  return BlockingLock(**lock, LOCK_SH, /*retry_signals=*/true,
                      "waiting for the daemon");
}

}  // namespace dcfs
