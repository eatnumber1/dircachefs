#ifndef DCFS_UMOUNT_HELPER_H_
#define DCFS_UMOUNT_HELPER_H_

// umount.fuse.dcfs and umount.fuse (plan step 15.6b; decision russ,
// 2026-10-09). A dcfs daemon outlives the unmount of its mount: the kernel
// aborts a plain FUSE connection and does not wait for the daemon (it sends
// FUSE_DESTROY and waits only for `fuseblk` and virtiofs mounts), which then
// syncs the backing filesystem and closes its cache database. umount(8)
// returns, systemd calls the unit stopped, and the next start finds the cache
// database in use. So the unmount of a dcfs mount is this helper, which
// unmounts and then waits, with no timeout, for the daemon: "unmounted" means
// "stopped".
//
// Which names umount(8) runs is in dcfs/mount_dcfs.h (kUmountFuseDcfsHelperName,
// kUmountFuseHelperName). The helper runs `umount -i` (no helper: no recursion)
// as a child with the options it was given, so the unmount, its messages and
// its status are umount(8)'s whatever the mount, and waits only for a dcfs
// daemon.
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
//
// Whether to wait at all: only when this unmount ended the mount's
// superblock. A bind mount of it (`mount --bind /data/sub /srv/x`), an rbind,
// a copy in another mount namespace (a container's volume, an `unshare -m`
// shell) or `umount -r` on a busy mount leave the superblock, and so the
// daemon, alive: waiting for it would wait for something this unmount did not
// do. The kernel says it in fusectl: /sys/fs/fuse/connections/<minor> exists
// while the connection does, and fuse_ctl_remove_conn removes it inside
// fuse_conn_destroy, which runs as the unmount's task work, before umount(2)
// returns to the helper. The helper opens that directory (O_PATH) before the
// unmount and waits only if fstat shows it gone (st_nlink 0). fusectl is one
// global view (it is not per namespace). Without fusectl mounted
// (`mount -t fusectl fusectl /sys/fs/fuse/connections`; systemd does it) the
// helper cannot tell and does not wait.

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

// The daemon's side. `mountpoint` as /proc/self/mountinfo will name it
// (symbolic links resolved), to be taken BEFORE the FUSE mount exists:
// afterwards the daemon, which is the one that would answer, cannot look at it.
[[nodiscard]] absl::StatusOr<std::string> CanonicalMountpoint(
    std::string_view mountpoint);

// After the FUSE mount of `mountpoint` exists (and before the loop serves it),
// takes the lock for its device, named by mountinfo's line for it, in `dir`
// (created, one level, if it is not there).
//
// A lock that is held is an earlier daemon of a mount with this device number
// that is still shutting down: anonymous device numbers are freed during the
// unmount and given out again lowest first, so `systemctl restart a.mount
// b.mount` can start a's new daemon on the device b's old daemon still serves
// the exit of. Waiting is right and needs no timer: a live mount holds its
// device number, so the holder is exiting. The wait is a blocking flock (logged
// at INFO when there is one), cancelled by a signal (Cancelled). The daemon
// before it removed the file as it shut down, so after the wait the file this
// lock is on may no longer be the one at the path (and the next daemon would
// lock another): compared with fstat and fstatat, and taken again until they
// are the same. The descriptor is never closed: the kernel releases the lock
// when the process exits, which is the event the helper waits for. Chosen over
// keying the lock by the 64-bit unique mount id (statx STATX_MNT_ID_UNIQUE,
// never reused): the daemon cannot statx its own mount before it serves it,
// and getting the id from mountinfo's old one needs listmount and statmount.
[[nodiscard]] absl::StatusOr<DaemonLock> HoldDaemonLock(
    std::string_view mountinfo, std::string_view mountpoint,
    std::string_view dir = kDaemonLockDir);

// What the daemon does with the lock file when it has shut down, while it
// still holds the lock (and after it let go of the cache database's own lock,
// so that a daemon that starts on the fresh file does not find that one held):
// removes it, so that a helper that opened it before the unmount waits for this
// daemon only, never for a later one that reuses the device number.
void RemoveDaemonLockFile(const DaemonLock &lock);

// The helper: runs `umount -i` for `args.target` with the options given and,
// if the target is a dcfs mount, the unmount was not lazy or in another mount
// namespace, and it ended the superblock (see above), waits for the daemon
// that served it to exit. A lazy unmount detaches the mount at once, as umount
// -l promises, and the daemon exits when the last user of the mount lets go:
// the helper does not wait for that (it may be never, for a busy mount).
//
// Returns umount(8)'s exit status in the status's payload when it failed
// (ExitStatusFor), with no message: umount printed it.
[[nodiscard]] absl::Status UmountAndWait(const UmountArgs &args);

}  // namespace dcfs

#endif  // DCFS_UMOUNT_HELPER_H_
