# Phase 21 — Real-hardware checks (manual, russ)

QEMU cannot show that a real disk stays spun down. These checks are run by
russ on real hardware, from a written procedure, and recorded in `log.md`.

**21.1 Trial on a spare disk (after Phase 11).** On a disk holding nothing
precious:
1. Mount dcfs over it (command-line form current at that point) with the
   cache on an always-on disk; warm the cache (`find` over the tree).
2. Spin the disk down (`hdparm -y`) and confirm standby (`hdparm -C`, or
   `smartctl -n standby -i`).
3. For an hour, run a metadata workload: `ls -R`, `find`, `stat`,
   `updatedb` restricted to the mount, `df`, browsing over NFS from
   another machine. Check standby every few minutes; the disk must stay
   in standby throughout.
4. Read a file: the disk spins up (expected); let it spin down again and
   repeat step 3 briefly.
5. Unmount and remount; repeat step 3 briefly (warm restart).
Any spin-up during metadata-only activity is a bug: reproduce it in QEMU
as a failing test (Phase 10's idle test or a new one) before fixing.

**21.2 Final check before deployment (after every other phase).** The
same procedure on the target setup, through the `mount.dcfs` wrapper and
fstab, including submounts or btrfs subvolumes as used there, and an
unclean shutdown (power off during idle) followed by a restart.

**Target server** (russ, 2026-10-05): x86_64 (the architecture all tests
use). Its kernel is upgraded to at least 6.9 before 21.2 (dcfs needs FUSE
passthrough and `FS_IOC_GETFSUUID`); the same applies to the machine used
for 21.1.

**Deployment rule (russ):** dcfs goes on the server with precious data
only after all phases are done, all tests pass (including the slow tier
and a soak run), and 21.2 passes.

## 21.1 notes for russ's setup (2026-10-10)

- **Kernel floor.** The dev host runs 6.8.0-146-generic; dcfs needs 6.9
  (FS_IOC_GETFSUUID, FUSE passthrough). Boot an HWE kernel (6.11 or 6.14
  on 24.04) before the trial; 6.13+ only matters if the connectable-handle
  path (13.5) ever returns.
- **snapraid-btrfs** (snapper snapshots of each data disk, snapraid run
  against the snapshot paths). Through dcfs a snapshot is a subvolume and
  so a boundary stub (contents ENOTSUP), so snapraid keeps running against
  the backing btrfs directly, outside dcfs. It coexists if: (1) nothing
  writes to the served tree behind dcfs: snapper writes only inside
  `.snapshots`, a subvolume dcfs does not cache, fine; snapraid's `content`
  file must not be cached by dcfs if snapraid writes it natively; russ's
  layout puts it on its own subvolume
  (`/mnt/<disk>-data/snapraid-ignore/snapraid.content`), which is a
  boundary stub to dcfs, so the native writes are invisible to the cache
  and the rule holds as is (the stub cannot be entered through the mount,
  which nothing needs); (2) native reads are fine except atime
  (dcfs caches file atimes from held fds and directory atimes cache-only,
  so a native read's relatime update is not seen): `noatime` on the backing
  or indifference; (3) no native restores or `fix` into the served tree
  while mounted. dcfs does not make snapraid's scans cheaper (they read the
  snapshot on the backing); its benefit is for the live tree's readers
  while the disks sleep. A dcfs instance per snapshot
  (`dcfs.fstype=btrfs,subvol=`) is cold every time (new identity).
  To verify in the trial: `.snapshots` appears as a stub, the rest of the
  tree is unaffected (15.4 finishes the stub behaviour), a snapraid sync
  during the trial changes nothing dcfs serves.
- **First configuration.** The `none` form over a directory of the spare
  disk, `dcfs.cache_db` on the fast disk, a tree without submounts, the
  README's operations table at hand; collect the daemon's CPU seconds and
  the sync-point cadence against the disk's spin-down (what the design
  promises and no test measures on real hardware); expect a slow first
  cold listing of a big tree (17.3 measures why).
- **Worth having first:** 15.5 (`fsck.dcfs`, for the sixth field) and 15.8
  (`allow_other` on by default), both in flight; 17.2's setgid fix is
  minor for a trial.
