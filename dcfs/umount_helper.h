#ifndef DCFS_UMOUNT_HELPER_H_
#define DCFS_UMOUNT_HELPER_H_

// umount.fuse (plan step 15.6b; decision russ, 2026-10-09). A dcfs daemon
// outlives the unmount of its mount: the kernel aborts a plain FUSE
// connection and does not wait for the daemon (it sends FUSE_DESTROY and waits
// only for `fuseblk` and virtiofs mounts), which then syncs the backing
// filesystem and closes its cache database. umount(8) returns, systemd calls
// the unit stopped, and the next start finds the cache database in use. So
// the unmount of a dcfs mount is this helper, which unmounts and then waits,
// with no timeout, for the daemon: "unmounted" means "stopped".
//
// umount(8) runs `umount.<type>` with the type of the mountinfo line minus
// its subtype, so the helper is `umount.fuse`, for every FUSE mount. For a
// mount that is not dcfs's it does what umount(8) does without a helper
// (`umount -i`, run as a child, same flags; its messages and status are the
// user's) and nothing more.
//
// How the helper finds the daemon: the daemon holds an exclusive flock(2) on a
// lock file named by the device number of its FUSE mount
// (/run/dcfs/<major>_<minor>.lock, DaemonLockPath) from the moment it has
// mounted until it exits, and the helper takes a shared lock on the same
// file, which it gets when the daemon is gone. A lock, not a pid: the kernel
// releases it when the process dies however it dies (a crashed daemon: the
// helper returns at once), there is no pid to be reused, and the wait is a
// blocking system call that a signal ends (Ctrl-C, or systemd killing the
// helper) -- no timer, no polling.

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/mount_dcfs.h"

namespace dcfs {

// The daemon's lock: where it is, for RemoveDaemonLockFile.
struct DaemonLock {
  std::string path;
};

// The daemon's side. `mountpoint` as /proc/self/mountinfo will name it (symbolic
// links resolved), to be taken BEFORE the FUSE mount exists: afterwards the
// daemon, which is the one that would answer, cannot look at it.
[[nodiscard]] absl::StatusOr<std::string> CanonicalMountpoint(
    std::string_view mountpoint);

// After the FUSE mount of `canonical_mountpoint` exists
// (and before the loop serves it), takes the lock for its device, named by
// mountinfo's line for it. The descriptor is never closed: the kernel
// releases the lock when the process exits, which is the event the helper
// waits for. FailedPrecondition if the lock is held: an earlier daemon of a
// mount with this device number is still shutting down (something that does
// not wait for it unmounted it).
[[nodiscard]] absl::StatusOr<DaemonLock> HoldDaemonLock(
    std::string_view mountinfo, std::string_view canonical_mountpoint);

// What the daemon does with the lock file when it has shut down, while it
// still holds the lock: removes it, so that a helper that opened it before
// the unmount waits for this daemon only, never for a later one that reuses
// the device number.
void RemoveDaemonLockFile(const DaemonLock &lock);

// The helper: runs `umount -i` for `args.target` with the flags given (so the
// unmount, its messages and its status are umount(8)'s) and, if the target is
// a dcfs mount and the unmount was not lazy, waits for the daemon that served
// it to exit. A lazy unmount detaches the mount at once, as umount -l
// promises, and the daemon exits when the last user of the mount lets go: the
// helper does not wait for that (it may be never, for a busy mount).
//
// Returns umount(8)'s exit status in the status's payload when it failed
// (ExitStatusFor), with no message: umount printed it.
[[nodiscard]] absl::Status UmountAndWait(const UmountArgs &args);

}  // namespace dcfs

#endif  // DCFS_UMOUNT_HELPER_H_
