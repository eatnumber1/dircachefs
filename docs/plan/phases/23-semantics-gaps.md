# Phase 23 — Semantics gaps (decided 2026-10-07)

Five user-visible gaps russ decided to close after trying dcfs by hand and
reading README's limitations. All test first; owner dcfs-protocol unless
noted. Order: right after S3, before Phase 22 (cancellation) and Phase 13.

## 23.1 Writable mmap after the last close: reconcile on FORGET

With passthrough the mapping holds the backing file, so RELEASE arrives at
`close()` and later stores through the mapping bypass FUSE; the only later
request is FORGET. dcfs tracks which inodes had a writable open during
this run; on FORGET (and FORGET_MULTI) of such an inode it re-reads the
backing attributes by handle and reconciles size, mtime, ctime and
blocks (through the usual present/unknown rule), then drops the inode
from the set. Cost: one statx per such inode, only when the kernel lets
go. Not covered: a mapping that keeps writing for longer than the kernel
keeps the inode; that needs the kernel change (future work: the mapping
should keep the FUSE file referenced so RELEASE follows munmap).
Tests: guest test with a shared writable mmap written after close, then
`echo 2 > drop_caches` (FORGET), then a cold stat through dcfs must show
the new size/mtime (fails today); unit test with a forged FORGET; the
model gains nothing (attributes of child objects are outside it) but
trace validation must still pass with the new call site mapped as a
refresh.

## 23.2 Mutations on removed-but-referenced objects

Objects with no row (unlinked files still open, removed working
directories, O_PATH descriptors) are served from an in-memory record;
mutations fail with ESTALE because the three-phase protocol writes rows.
Make them work: setattr (truncate, chmod, chown, utimens), xattr set and
remove, and fsync apply through the object's open fd (dcfs holds one until
the last FORGET) and update the in-memory record with the backing's
result; the record carries its own tri-state so a crash mid-mutation
cannot leave a stale value (it is in memory: a crash loses it, which is
sound). Reopening an unlinked file through /proc/<pid>/fd stays as the
backing allows (open by handle of an nlink-0 inode works on ext4/xfs/
btrfs; verify). Tests: unlink-then-ftruncate/fchmod/fsetxattr on an open
file (fails today with ESTALE), chmod of a removed cwd, pjdfstest shards
still green.

## 23.3 relatime semantics for atime (dcfs-implementer)

Reads are passthrough, so dcfs never sees them. On OPEN of a regular file
for reading, update the cached atime to now if it is older than mtime or
ctime, or more than a day old (the kernel's `relatime` rule), and on the
backing's own timing if `strictatime`/`noatime` can be detected from the
mount (document). Record it in the cache only (the backing updates its
own atime on read); no backing I/O. Tests: cached atime after an open
follows the relatime rule; a second open within a day does not change it;
zero backing reads.

## 23.4 O_TMPFILE, copy_file_range, FICLONE/FICLONERANGE, ioctls

- `copy_file_range`: FUSE_COPY_FILE_RANGE, passthrough to
  `copy_file_range(2)` on the two backing fds, with the destination's
  three-phase bookkeeping (attributes unknown before, refreshed after).
- Reflinks: FUSE_IOCTL for FICLONE/FICLONERANGE/FIDEDUPERANGE forwarded to
  the backing fds (btrfs and xfs support them; ext4 returns EOPNOTSUPP);
  same bookkeeping. Other ioctls: forward a documented allowlist
  (FS_IOC_GETFLAGS/SETFLAGS, FS_IOC_FSGETXATTR/FSSETXATTR,
  FS_IOC_GETVERSION); everything else ENOTTY, listed in README.
- `O_TMPFILE`: FUSE_TMPFILE (libfuse `tmpfile` op): create an unnamed
  file in the backing directory with O_TMPFILE, served from an in-memory
  record like 23.2's; `linkat(AT_EMPTY_PATH)` into a name becomes a
  create-like mutation that gives it a row.
- Tests: `cp --reflink=always` on btrfs and xfs (fails today), `cp` using
  copy_file_range (coreutils does by default), `chattr +i`, O_TMPFILE +
  linkat; errno parity with the backing filesystem for each; coverage of
  the error paths through the failure-sweep harness (Phase 8 when it
  lands, else unit fakes now). Model: copy and clone are mutations of the
  destination object's attributes (outside the one-directory model);
  tmpfile+linkat is a create (`CreateArrive` with no probe): add it.

## 23.5 Boundary stubs now (pulled forward from Phase 15.4)

russ (2026-10-07): submounts and btrfs subvolumes must appear in readdir,
and anything inside them returns ENOTSUP (today: hidden, and EXDEV).
Implement Phase 15's decision 3 now: the persisted "refused" dentry is
served as a stub directory (mode/owner/times from the boundary root's
statx; nodeid from the reserved range at or above 2^63, which Phase 14
keeps; dcfs refuses backing inode numbers in that range from now on),
listed by readdir/readdirplus, lookup succeeds, anything inside returns
ENOTSUP with a one-time ERROR log, rename/link across it EXDEV. Keep
Phase 15.4's remaining items (the bind form's recorded mount points)
there. Tests: the Phase 15.1 stub tests, written now, failing today
(`ls` shows the subvolume, `ls sub/` gives ENOTSUP not EXDEV).
