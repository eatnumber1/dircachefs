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

**Done (2026-10-07, step-23 branch).** `DirCacheFS::ReconcileWritten` at
the last FORGET/FORGET_MULTI and at DESTROY (the kernel sends no FORGETs at
unmount): one `backing::StatWritten` (statx by handle, identity checked, not
an out-of-band change); unchanged -> nothing; changed or unknown -> phase 1
(`BeginAttrChange`, durably dirty) then a refresh as a fill. Finding: the
plan's "not covered: a mapping that keeps writing longer than the kernel
keeps the inode" cannot happen: the mapping's backing file holds the FUSE
file's path (`backing_file_open` takes `user_path`), so the last FORGET
comes after munmap. What is left: stale attributes between munmap and the
last FORGET (and after a crash in that window), and the statx at FORGET can
spin up a disk whose cache dropped the file. Tests: write.sh
`mmap-store-after-close-reconciled-on-forget` (3 fs), harness LastForget*,
DestroyReconcilesWrittenFiles (traced; validates). lib.sh's
`drop_caches_quiesced` now waits for the daemon to go quiet, so that a
reconciliation's statx lands before a zero-reads baseline.

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

**Done (2026-10-07, step-23 branch).** SETATTR, SETXATTR, REMOVEXATTR,
FSYNCDIR and OPEN of a removed object go through its record's descriptor
(`backing::SetAttrFd`/`SetXattrFd`/`RemoveXattrFd`/`FsyncDirFd`/`ReopenFd`);
writes, FSYNC and RELEASE of a reopened one skip the cache bookkeeping. The
record caches nothing that changes (every read already went to the
descriptor), so its "tri-state" is degenerate (always unknown, read
through), nothing can be stale after a change or a crash, and the model is
unchanged. Found while testing: unlink-then-ftruncate/fchmod/fsetxattr on a
file dcfs has *open* already worked (its row lives until the last release,
and open_by_handle_at reaches an unlinked inode that is still held, on
ext4, xfs and btrfs); the failing cases were the O_PATH magic link and the
removed cwd. LINK of a removed object still ESTALE.

## 23.3 relatime semantics for atime (dcfs-implementer)

Reads are passthrough, so dcfs never sees them. On OPEN of a regular file
for reading, update the cached atime to now if it is older than mtime or
ctime, or more than a day old (the kernel's `relatime` rule), and on the
backing's own timing if `strictatime`/`noatime` can be detected from the
mount (document). Record it in the cache only (the backing updates its
own atime on read); no backing I/O. Tests: cached atime after an open
follows the relatime rule; a second open within a day does not change it;
zero backing reads.

**Done (2026-10-07, step-23 branch).** `cache::TouchAtime` at a read OPEN
(not O_NOATIME, not writable: a writable open's attributes are unknown and
re-read at its last release), one transaction, no backing I/O; the rule is
the kernel's `relatime_need_update` (atime not after mtime *or ctime*, or a
day old), strictatime always, noatime never, from the source mount's
`statvfs` flags at `InitRoot` (`Context::atime`). Tests: `atime_test`
(3 fs: matches the backing atime within 2 s, zero backing reads for the
stat, unchanged on a second read), `RelatimeTest.*` (incl. a remount to
strictatime and noatime), `TouchAtimeFollowsTheMountsRule`. Known
differences: dcfs stamps the open, the backing filesystem the read; an
open that reads nothing still moves dcfs's atime; directories' atimes are
not predicted.

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

**Done (2026-10-07, step-23 branch), with one item impossible.**
- copy_file_range: `DirCacheFS::CopyFileRange` on the shared backing fds
  (phase 1 / copy / refreshes as a fallback write). On btrfs and xfs the
  copy shares extents as natively (`copy_test`: `shared=1` through dcfs,
  `shared=0` before).
- **FICLONE/FICLONERANGE/FIDEDUPERANGE cannot be done**: the VFS handles
  them (`do_vfs_ioctl` -> `vfs_clone_file_range`), FUSE has no
  `remap_file_range`, so they never reach a FUSE server and fail
  EOPNOTSUPP on every backing filesystem (ext4's own answer too; xfs and
  btrfs differ). It needs a kernel FUSE op; the ioctl path could not carry
  it anyway (the source fd is a number in the caller's process).
  `cp --reflink=auto` gets copy_file_range's sharing.
- ioctls: allowlist FS_IOC_GETFLAGS/SETFLAGS, FSGETXATTR/FSSETXATTR (the
  VFS fileattr calls: FUSE sends them as FUSE_IOCTL on a private
  OPEN/RELEASE) and FS_IOC_GETVERSION; a set is a phase 1 + refresh;
  anything else ENOTTY; FUSE_CAP_IOCTL_DIR requested. Found: the shared
  backing fd may predate `chattr +i` (the private open's RELEASE is
  asynchronous), so a writable OPEN now re-checks writability with a reopen
  of that fd with its access mode.
- O_TMPFILE: a row without a dentry (not an in-memory record: the nodeid
  needs a row's id, and the row-lifetime rule already retires it), linked
  by an ordinary LINK; the model has `linkcreate` (a create whose phase 3
  needs no probe); the recorder maps a LINK of an unnamed tmpfile
  (`Op::kLinkTmpfile`) to it; `dir_cache_fs_trace_test` validates
  TmpfileLinkedIntoANameIsACreate (EEXIST and success). States: small
  773,371, large 7,238,097, nolock 6,036,816; every known bug still found.
- Error paths: harness unit tests (CopyFileRange EBADF, ioctl ENOTTY,
  COMPAT, ESTALE, a refused set; tmpfile in a stub and a stale parent;
  removed destinations). Not covered: the tmpfile's undo after a failed
  BeginWriting (no fault point short of Phase 8's sweep).

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

**Done (2026-10-07, step-23 branch).** Schema v4 `stubs` table (nodeid
from 2^63 up, random generation, the boundary root's attributes; triggers
keep it exactly as long as its dentry is refused), `ListDir` merges stubs
in rowid order, `DirCacheFS::RefuseStub` (ENOTSUP, EXDEV across, logged
once per stub), `RefuseReservedIno` for backing inode numbers >= 2^63.
Tests: `boundary_test` (ext4/xfs/btrfs, mount and subvolume, restart),
`dir_cache_fs_test` Boundary*/BackingInodeNumbers*, cache and migration
tests; readonly/create/handles/rename/nfs updated. Deviation from 15.1's
text: a link or rename *into* a stub fails ENOTSUP, not EXDEV, because the
kernel looks the target name up in the stub first (EXDEV is what renaming
the stub itself, and a forged RENAME/LINK into it, get).

## 23.6 Held fd instead of a statx at FORGET (russ, 2026-10-07)

Replace 23.1's statx-by-handle at the last FORGET with an fd dcfs keeps
open for every file written during this run until that file's last
FORGET; the reconciliation then reads attributes through the held fd
(`fstatx`), which the kernel answers from the pinned inode without a
disk read, so a sleeping backing disk stays asleep. Cost: one fd per
written file until the kernel forgets it (bounded by the kernel's inode
cache, as the lookup map is); raise RLIMIT_NOFILE at startup and
document it.
**Document carefully, in three places that point at each other** (the
reason this exists and when to remove it):
- `docs/design.md`: passthrough mmap keeps only the backing file, so the
  kernel releases the dcfs file at `close()`, before `munmap`; stores
  through a mapping after the last close reach the backing file without
  a FUSE request; the last FORGET is the only later event, hence the
  reconciliation there, and the held fd keeps that reconciliation off the
  disk. The fix belongs in the kernel: a passthrough mapping should keep
  the FUSE file referenced so RELEASE follows `munmap`; once that lands
  (and the minimum kernel requires it), delete the written-file set, the
  held fds and the FORGET hook, and let the existing last-writable-
  release path cover mmap.
- the code: one marker comment (`// See design.md "mmap after close"
  (held-fd workaround)`) at the written-file set, the held fd and the
  FORGET hook, so a grep finds all three.
- `docs/plan/README.md` future work: the kernel change, with the pointer
  back to the workaround.
Tests: the 23.1 tests unchanged; plus the idle test extended with a
written-then-forgotten file (drop_caches) showing zero backing reads at
the FORGET (fails with 23.1's statx when the inode is cold: prove it with
a cold backing, e.g. after dropping the backing fs's caches via a second
drop or dm-delay timing).

## 23.7 Review fixes (audits/review-2026-10-07-phase23.md)

- M1: refuse SETFLAGS/FSSETXATTR that change FS_CASEFOLD_FL (EOPNOTSUPP;
  compare with a GETFLAGS first); test with an ext4 made `-O casefold`.
- M2: with 23.6's held fds the DESTROY reconciliation is fstatx per held
  fd (no disk); measure it with 100k written files and bound it; document
  the live-mapping-after-unmount case (libfuse aborts the connection and
  unmounts lazily; stores after that reach the backing file; the next run
  cannot know) in README and design.md, next to the held-fd rationale.
- L1: when the writability check passes and the shared fd is not
  writable, keep the reopened (or an O_RDWR) fd as the shared fd, so
  copy_file_range and fallocate work; avoid the extra open/close side
  effects where possible.
- L2: stub errnos: RMDIR/UNLINK of a stub EBUSY (as a mount point), LINK of
  a stub EXDEV as design.md says, RefuseStub's log names the errno it
  returns; tests for every refused op on a stub.
- L3: RefuseStub returns ESTALE when the stub row is gone.
- L4/L6/L8 docs: stub attributes "as of the last probe"; atime exempt from
  the mirror rule (+A ignored, private opens move it, not recovered after
  power loss); 32-bit programs without large-file support get EOVERFLOW
  listing a directory with a stub.
- L5: a startup sweep deletes tmpfile rows left by a crash; the undo path
  removes its RecordTmpfile row.
- L7: StatWritten returns its first statx; RefreshAttrs reuses the fd;
  TouchAtime reads before it writes.
- Tests: fix the two over-claiming comments; `handle-boundary-stub-decodes`
  must actually evict the dentry; `rename-stub-exdev` gets a check_cold;
  the recorder maps a read-only GETFLAGS on D to no cut.

## Done (2026-10-07, a64e666)

23.1-23.5 merged 1554bee..e5c27cc; 23.6 + 23.7 merged a64e666 after two
review rounds (audits/phase23-review.md findings M1, M2, L1-L8 all
closed). Deviations from the text above, recorded per the reviewer:
- L1: instead of keeping the reopened fd as the shared fd, a separate
  `write_fd` (first writer's; yields only to a non-O_APPEND one; dropped
  with the last writer), and every writable open that reuses a shared fd
  re-checks the flags with one FS_IOC_GETFLAGS on it (in-memory inode, no
  disk I/O; EPERM on immutable or append-only without O_APPEND), so an
  out-of-band chattr is still refused (russ's default kept).
- L7's `StatWritten` and the by-handle refresh went away with 23.6 (the
  held fd replaces both).
- M2 measures (destroy_test: 100k files, 0 sectors read after SIGTERM,
  about 5 s to exit) rather than marking attributes unknown.
- L5's sweep runs only after an unclean shutdown; rows of files unlinked
  while open at a clean, lazily unmounted shutdown are not swept.
- 23.6 extras: held fds capped (reserve = half the soft limit, within
  16Ki..64Ki; a cap of 0 is logged once at startup), FORGET_MULTI/DESTROY
  batch their phase-1 transaction, the guests' busybox had no `usleep`
  (quiesce never waited; `lib.sh` now checks the commands it needs),
  ASan unit guests measured (dir_cache_fs_test asan_mem=448), destroy_test
  mem=832.
- Kernel gap found: no attribute invalidation after a successful
  fileattr_set (notes/kernel-facts.md); the check is
  `DISABLED_immutable-ctime`, README Limitations has the sentence.
- Open: in destroy_test at 832 MiB about 1,000 of 100,010 held inodes
  still got FORGETs with no memory pressure (test tolerates >= 90%);
  undiagnosed, for an investigator.

## 23.8 Access times from the held fd; directory atimes in the cache (russ, 2026-10-08)

Decision and alternatives: `notes/atime-alternatives-2026-10-08.md`.
- Regular files: remove atime prediction. While dcfs holds a backing fd
  for an inode (passthrough open, written file's O_PATH fd), its attributes
  come from `fstat` on that fd: at FLUSH, at RELEASE, and for a GETATTR
  that arrives while it is held (no disk I/O: an open file pins its inode).
  The row recorded from `fstat` is marked dirty with its own reason, so the
  next sync point's `syncfs` makes the backing's lazy atime write-back
  durable; recovery treats it like any dirty row. This is a new fill
  source and a new dirty-set reason: the TLA+ model gets it in the same
  change (a "held" fill that is always current, its guard, its dirty row).
- Directories and symlinks: dcfs stamps the relatime-predicted atime in
  the database on readdir/readlink (from the mount's atime flags, as
  today) and never writes it to the backing; it survives restarts and is
  lost on a cache wipe. `formal/limitations/` states that these two
  attributes are cache-only; the checker and the comparisons in the fault
  tests exclude directory and symlink atime from "served equals backing".
- README "Limitations": the predicted-atime paragraph is replaced by the
  new rule and the directory/symlink limitation; design.md gets the
  mechanism and the kernel facts.
- Tests first: an open that reads nothing leaves atime unchanged (fails
  today); a read then stat shows the backing's atime exactly (compare with
  a stat on the backing); `chattr +A` and `O_NOATIME` respected; mmap read
  then close then stat; a long-held reader's GETATTR; `lsattr`'s private
  open; power loss after a read: recovery leaves the backing's value;
  directory atime advances on readdir under relatime rules and survives a
  restart; the fault tests' comparisons updated. Budgets: one `fstat` per
  FLUSH/RELEASE, no new syscall on the GETATTR-from-cache path (the strace
  goldens and syscall budgets say so).
Owner: dcfs-protocol.

## 23.9 LINK of a removed object answers what the backing answers (russ, 2026-10-08)

The README's "Removed objects that are still referenced cannot be linked
back" overstates the limitation: Linux itself refuses to link an inode
with no links (`vfs_link`: ENOENT unless `I_LINKABLE`, which only
`O_TMPFILE` sets, and that case works through dcfs), and a directory can
never be hard-linked (EPERM). The only difference is the errno: dcfs
replies ESTALE for a LINK whose source row is a removed object. Make the
behaviour identical: forward the backing's answer (`linkat` by the held
descriptor with `AT_EMPTY_PATH`, which yields ENOENT for an unlinked
regular file, EPERM for a directory, and succeeds for an O_TMPFILE file
as today), test each against the same operation on the backing
(`removed_test`), and rewrite the README and design.md paragraphs. After
23.8, same agent. Owner: dcfs-protocol.

## 23.10 A directory-level dirty set (russ, 2026-10-08: "I do care about simplification")

Replace per-row dirty marks with dirty **directories**: a mutation marks
its parent directory dirty (both parents for rename and link; a write-open
and a read-open mark the parent, the latter with the atime-only reason
from 23.8), durably once per sync interval instead of once per operation
(the fsync-per-create cost goes away); file rows are never dirty.
Recovery re-lists every dirty directory against the backing and re-stats
its children (names present/absent, attributes, written sizes, atimes),
one procedure for every mutation class, instead of per-row probes by
handle. Sync points syncfs then clear the directories as today;
atime-only directories do not drive sync points. Cost: after a crash, a
listing plus a statx per child for each directory touched since the last
sync point. Protocol change: dcfs.tla's dirty set becomes a set over
directories (it already reasons mostly about D), `RecoveryIdempotent`,
`CrashSafe` and 12.7b's `ReplyObservable` must hold, known_bugs variants
for "child not re-listed" and "sync point clears a directory with a
mutation in flight"; schema v7; the checker's dirty rules; recovery
tests (12.6b's crash-during-recovery binary, 11.x's power cuts and ACE
sequences, 23.8's atime power cut) are the regression suite. Sequence:
after 12.8 (crash sequences) and 23.8 (held-fd dirty reason) merge, since
all three touch the dirty set. Owner: dcfs-protocol.

From the 12.8 review (2026-10-09), the condition the model must be able
to judge before 23.10 is called correct: the no-gap argument needs only
(i) D durably dirty before its first unsynced change in an interval,
(ii) the mark cleared only after a completed syncfs with no mutation of D
since the sync began, (iii) recovery forgetting all of a dirty D; 23.10
keeps all three for directories under every regime. What it changes is
that file rows are never dirty, so under every regime a power loss can
revert a file's size or mtime from unsynced writes, and 23.10 is correct
only if every written file has a dirty parent that recovery re-lists and
re-stats, **including hard links named in other directories, files opened
by NFS handle with no parent dentry, and an unlinked O_TMPFILE**. Judging
that needs a file inode (attributes, data, link count) and a second
directory in dcfs.tla: 23.10's model work starts there. Trap: replacing
syncfs by per-directory fsyncs breaks under ext4
(`known_bugs/sync_by_file_fsync`).

