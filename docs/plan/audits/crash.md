# dcfs crash-robustness audit (main @ 8d545ef)

This was a read-only audit: no files were modified and nothing was built or run.
Sources read: README.md, dcfs/schema.sql, dcfs/migrate.{h,cc}, dcfs/sqlite.{h,cc},
dcfs/metadata_cache.{h,cc}, dcfs/backing.{h,cc}, dcfs/dir_cache_fs.{h,cc}, dcfs/main.cc,
plus the SQLite 3.53.4 amalgamation and libfuse under bazel external/, and fs/{btrfs,ext4,xfs}
in ~/Sources/linux.

## Framing: the only DB states a crash can leave

* **Process crash (daemon killed, OOM, abort).** Every COMMIT has already reached the WAL
  through write(2), so the OS page cache keeps it. After restart the DB is exactly the
  state after the last committed transaction. Backing syscalls that returned are also kept
  (same kernel). So a process crash is the "clean" case, and each three-phase op only has
  to be safe at each commit boundary.
* **Power loss or kernel crash.** With WAL and `synchronous=NORMAL` (sqlite.cc:410), a commit
  is not fsynced. The WAL is only fsynced when a checkpoint runs (auto-checkpoint at 1000
  pages, `SQLITE_DEFAULT_WAL_AUTOCHECKPOINT`, sqlite3.c:14859) and at shutdown
  (main.cc:223). The WAL's chained frame checksums mean recovery always gives a *prefix* of
  the committed transactions (consistent, possibly not durable). The backing filesystems
  also come back as a prefix of their own journal commits, each filesystem independently.
  Nothing orders one against the other, so after power loss the DB can be **behind** the
  backing or **ahead** of it.

Because of the prefix property, if phase 3 of an op survived, its phase 1 did too. The only
DB states power loss can leave for any op are:

| DB prefix ends | Backing kept op | Backing lost op |
|---|---|---|
| before phase 1 (old state, *valid*) | **WRONG** (cache behind) | ok |
| between phase 1 and phase 3 (*unknown*) | ok | ok |
| after phase 3 (new state, *valid*) | ok | **WRONG** (cache ahead) |

---

## Findings

### F1. Power loss leaves valid-but-wrong cache state, in both directions. Severity: WRONG DATA served after a machine crash
**Where:** sqlite.cc:410 (`synchronous=NORMAL`). This affects every mutation and every
cache-populating path. The README's "Crash robustness" and "Write-through cache and nothing
more" sections promise the opposite.

**Cache behind the backing** (phase 1 lost, syscall kept). ext4 data=ordered commits its
journal, with FLUSH/FUA, about every 5 s. dcfs's WAL pages reach the device only through
normal page writeback (dirty_expire 30 s plus up to 5 s writeback interval, still with no
FLUSH) or at the next auto-checkpoint. So for about 5 to 35 s after **every** mutation, a
power cut is likely to keep the backing change and drop phase 1. Concrete examples:

* **unlink/rmdir.** The dentry still points at row X with valid attrs, and the parent is
  still `children_complete=1`. Lookup and getattr serve the dead file from cache. Only an
  open-by-handle notices (ESTALE), and readdir keeps listing it.
* **create/mkdir/symlink/link.** The cached *negative* dentry and the complete listing stay
  in place. A file that exists on the authority is invisible (readdir never relists, and
  lookup says ENOENT) until some unrelated mutation touches that directory. To the user
  this looks like data loss.
* **write-then-rename-then-fsync(dir)** (the standard atomic-replace pattern). The backing
  has the new file, but dcfs still has `file`→old row and `tmp`→new row. stat(file) serves
  the old size and mtime. stat(tmp) succeeds even though tmp no longer exists. The
  application fsynced, and dcfs's Fsync/Fsyncdir (dir_cache_fs.cc:898-916, 1062-1071) pass
  the sync to the backing but never make the DB durable.
* **Writable open plus passthrough writes.** BeginWriting's MarkAttrsUnknown
  (dir_cache_fs.cc:143-146) is lost while the data and size survive, so the pre-write
  size and mtime are served as current. This is exactly what the README says cannot happen.
* **Out-of-band recovery does not help here.** ReconcileAttrs only fires when an OpenNode
  or PopulateDirectory happens anyway, and cache hits detect nothing.

**Cache ahead of the backing** (phase 3 or a populate kept, backing op lost). This needs the
WAL tail to be on media while the backing journal commit is not. It is less likely on ext4
(5 s commit) and plausible on btrfs (30 s commit), xfs, or any fs with a long commit
interval, or whenever an auto-checkpoint fsync happens right after the op. Examples: a
created file's row, dentry and complete listing survive while the backing lost the file
(stat and readdir serve a nonexistent file until an open gets ESTALE), or an unlink's
negative dentry survives while the backing kept the file (the file is invisible with no
self-healing).

**Is `synchronous=FULL` (or an fsync after phase 1) required? Necessary for one direction,
not sufficient.**

* FULL on phase-1 commits fixes "cache behind". FULL on everything still does **not** fix
  "cache ahead". That would need the *backing* change to be durable before phase 3 can
  become durable, meaning fsync of the backing parent directory or syncfs per mutation.
  On spinning disks that is roughly 10 to 30 ms per op, and it is not viable.
* SQLite refuses to change the safety level inside a transaction (sqlite3.c:145934), so
  FULL just for phase 1 means `PRAGMA synchronous=FULL` before and `=NORMAL` after the
  phase-1 `Transaction()`. Both pragmas do no I/O.

**Rough costs.** One WAL fdatasync plus device FLUSH costs about 20-60 µs on an enterprise
SSD with power-loss protection, about 0.2-2 ms on a consumer NVMe without it, and about
1-5 ms on a SATA SSD. Commit counts per op in the current code:

| Op | Commits | Of which phase 1 |
|---|---|---|
| Create | about 5 (MarkUnknown, RecordNewChild, MarkDirComplete, parent RefreshAttrsFromFd, EntryFor refresh) | 1 |
| Setattr | 2 | 1 |
| Unlink | about 4 | 1 |
| Rename | about 4-6 | 1 |
| Open for write | 1 | 1 |
| Release | 1-2 | 0 |
| Populate / negative lookup | 1 | 0 |

Take an untar of about 80k files, as root, where tar also does chmod/chown/utimens. That is
2-4 phase-1 commits per file, so 160k-320k flushes:

* FULL on phase 1 only: +8-16 s with power-loss protection, +80-160 s on consumer NVMe,
  +8-25 min on SATA SSD. Baseline is minutes of HDD work.
* FULL everywhere: about 2.5x that, and the *read* path (populate, negative lookups) also
  pays one flush per populate.

**Recommended fix (A): detect an unclean machine shutdown and invalidate at startup.** This
costs nothing in steady state and covers both directions, which is exactly what the README
promises ("costs a repopulation").

1. At startup, before InitRoot: read `meta.running_boot_id`.
   * If it is present and differs from `/proc/sys/kernel/random/boot_id`, the machine went
     down while dcfs was running. In one transaction, drop everything the cache claims to
     know but keep every row and id, so NFS handles still resolve and are verified on open:
     `DELETE FROM dentries; UPDATE directories SET children_complete=0; UPDATE inodes SET
     attrs_valid=0, xattrs_complete=0; DELETE FROM xattrs; DELETE FROM symlinks`.
     `DELETE FROM xattrs` matters because `GetXattr` serves a present row even when
     `xattrs_complete=0` (metadata_cache.cc:337-348).
   * Then write the current boot_id with FULL. Put this in the same transaction, so a
     crash during recovery just reruns it.
   * If the stored value equals the current boot_id, it was only a process crash and
     nothing is needed.
2. On clean shutdown, after the loop: `syncfs()` each backing mount fd, so the backing
   is durable too. Without this, a power cut right after a clean unmount is still "cache
   ahead". Then `Checkpoint()`, then clear `running_boot_id` durably.
3. Cost: one FULL commit per start, one syncfs per backing filesystem and a checkpoint per
   clean stop. After a machine crash, a fully cold namespace cache: an O(rows) update plus
   relisting directories on first access. For a DB with 10M rows the invalidation
   transaction is a few seconds and a large WAL on the SSD; it could be chunked.

If (A) is not done, the minimum partial fix is FULL for phase-1 commits plus making the WAL
durable in Fsync/Fsyncdir (a FULL no-op commit or a PASSIVE checkpoint). That still leaves
"cache ahead".

### F2. A (nodeid, generation) pair can be reissued for a different file after power loss. Severity: WRONG FILE through a surviving NFS handle
**Where:** migrate.cc:180-195 (MintFuseGeneration), metadata_cache.cc:504-519 (the
UpsertInode insert), schema.sql:50 (`AUTOINCREMENT`).

The row insert, the `sqlite_sequence` bump and the `gen_counter` bump are all in one
transaction. That is atomic, so a *process* crash cannot split them. But under NORMAL the
whole transaction can be rolled back by power loss **after** the reply carrying (N, G) went
to the kernel and on to an NFS client.

Because every insert except the root mints exactly one generation, `id - fuse_gen` is
constant for a DB's lifetime. The next insert after reboot therefore gets exactly the same
N (from `sqlite_sequence`) and the same G (from `gen_counter`), for whatever file is
upserted next. NFS clients survive a server reboot by design, so the old handle (N, G) now
passes the patched kernel's generation check and resolves to the new, unrelated file. This
breaks the README's core NFS promise.

The window is the full non-durable WAL tail (up to about 1000 pages or about 30 s of
writeback).

**Fix: hi/lo generation reservation.**

* Store `meta.gen_reserved` (a ceiling). Mint in memory from `gen_counter`. When the
  counter reaches the ceiling, commit `gen_reserved += 65536` with `synchronous=FULL`
  (toggled outside the transaction) *before* handing out any value in the new block.
* At startup, set `gen_counter = gen_reserved` and bump it durably again.
* Only G has to be unique: reusing N with a fresh G is harmless because of the kernel's
  generation check.
* Cost: one flush per 65536 mints plus one per start. It burns up to 64K generations per
  restart (2^32 / 2^16 = 65536 restarts before a wrap concern, the same scale as today).
* Alternative on top of F1(A): on an unclean-boot recovery, jump `gen_counter` by a large
  delta. This is cheaper to write but not rigorous, because the lost tail is not strictly
  bounded.

### F3. CreateChild's phase 1 does not mark the parent's attributes unknown. Severity: STALE ATTRS served after a process crash (self-heals only on the next OpenNode of the parent)
**Where:** dir_cache_fs.cc:161-166 (phase 1 is only `MarkUnknown(parent, {name})`) and
dir_cache_fs.cc:193 (the parent refresh comes last, in its own transaction). This covers
Mknod, Mkdir, Symlink and Create.

**Crash window:** after `mkdirat`/`openat(O_CREAT)`/... (phase 2) and before the
`RefreshAttrsFromFd(parent)` commit. That includes the RecordNewChild and MarkDirComplete
commits and the statx. After restart the parent still has `attrs_valid=1` with the
pre-create mtime, ctime and size. For mkdir, nlink is also stale, one short of the real
value. Getattr and readdirplus serve these as current until something OpenNode's the
parent, at which point ReconcileAttrs logs a bogus "out-of-band change" and fixes it.

The same stale state persists without any crash if line 193 fails (for example
SQLITE_BUSY); in that case the op also returns an error for a create that happened.

RemoveChild, Rename and Link all mark the parents' attributes unknown in phase 1; only
CreateChild does not.

**Second-order bug (g):** because the parent's attributes are still valid,
`OpenNode(parent)` at :176 runs VerifyBackingIdentity → ReconcileAttrs. If that detects an
out-of-band change, it calls `ForgetNegativeDentries` + `MarkIncomplete`. Then line 186
**restores `children_complete=1`** from the `parent_was_complete` it captured before the
syscall. That undoes the detection, and the next lookup of an out-of-band-added name
inside that directory is cached *negative* (backing.cc:717-724).

**Fix:** add `MarkAttrsUnknown(parent)` to the phase-1 transaction, the way RemoveChild
does. That fixes the crash window and also stops the ReconcileAttrs/restore-complete race,
since reconcile is then skipped for this path, as for the other ops. Cost: one UPDATE in an
existing transaction.

### F4. Startup checks the source filesystem but not the source *directory*. Severity: WRONG DATA served after restart (misconfiguration, or root replaced while down)
**Where:** main.cc:143-151, backing.cc:327-346 (InitRoot), metadata_cache.cc:525-564
(UpsertRoot).

The foreign-DB check compares only `source_device_id` (fs UUID plus subvol). If `--source`
points at a different directory on the same filesystem, or the source directory was
`mv`'d aside and recreated, UpsertRoot silently rewrites row 1's `backing_ino`/`backing_gen`
and handle. It keeps all of root's dentries and its `children_complete`
(EnsureDirectory is `DO NOTHING`). The old tree's rows are then served under the new root.
Their handles still decode (same fs, objects still exist), so nothing ever gets ESTALE.

**Fix:** in UpsertRoot, if the stored `(backing_ino, backing_gen)` differ from the probe,
refuse to start ("cache DB was built for a different source directory; delete it"), the
same as for the device mismatch. The stored identity is always set, by CreateSchema from
ProbeRoot. Cost: none.

### F5. Identity for generation-0 objects is only (dev, ino), and UpsertInode overwrites the stale row's handle. Severity: WRONG FILE through a stale handle, reachable via a crash window
**Where:** backing.cc:348-349 (ReadGeneration returns 0 for everything except
regular files and directories, and for filesystems without FS_IOC_GETVERSION, e.g. ZFS),
metadata_cache.cc:472-495 (`backing_gen == backing_gen` matches 0 == 0 and then
**UPDATEs `handle`** to the new object's handle), backing.cc:456-459
(VerifyBackingIdentity treats gen 0 as "same").

Scenario (symlink, FIFO or device node on ext4/xfs, where inode numbers are reused readily):

* RemoveChild or Rename-over removes it, and the process crashes between the unlink
  syscall and SettleUnlinkedFile's forget (dir_cache_fs.cc:400-410). The same happens if
  power loss rolls back that forget.
* The row survives. The next object created with the recycled inode number (new symlink,
  mknod, or a populate discovering it) matches the old row. It inherits the old id and
  fuse_gen, and the stored handle is replaced.
* An old NFS handle (N, G) for the deleted symlink now resolves to the new one. It is also
  reachable with no crash for any row left behind by an out-of-band delete.

The backing filesystem's own handle bytes *do* encode the generation (ext4 encodes ino plus
i_generation for symlinks too; ZFS encodes z_gen), but dcfs discards that signal by
overwriting.

**Fix:**

* In UpsertInode, treat an existing row as the same object only if its stored handle
  bytes equal the new handle (when both are present). Otherwise invalidate it and mint a
  new row.
* In VerifyBackingIdentity, also compare `stx_btime` against the stored `btime` when both
  are nonzero. The statx already requests STATX_BTIME, so this adds no syscall.
* Cost: none.

### F6. btrfs reissues identical (ino, generation, handle) after *its own* power loss. Severity: THEORETICAL (inherited from the backing fs; knfsd over raw btrfs shares it)
**Where:** btrfs uses `i_generation = trans->transid` (fs/btrfs/inode.c:6709-6710), and
free objectids are recomputed as highest+1 at mount (fs/btrfs/disk-io.c:5044ff). Both
roll back with a lost btrfs transaction.

If dcfs recorded a file that btrfs then lost (DB ahead, as in F1), the next file btrfs
creates after reboot can have a byte-identical handle. dcfs's row, and any NFS handle
(N, G) to it, then resolves to the new file. F1(A) does not help, because rows are kept on
purpose.

**Fix:** the btime comparison from F5. The new object's btime is after the reboot. Cost:
none.

### F7. Nothing stops two daemons from using one DB. Severity: WRONG DATA (restart while an old instance is stuck, e.g. in D state on a spun-down disk)
**Where:** sqlite.cc:405-414 and main.cc:133. There is no `locking_mode=EXCLUSIVE` and no
flock. The two processes' three-phase sequences and `open_for_write_` sets interleave
freely. SQLite serializes their transactions but not the protocol.

**Fix:** take `flock(LOCK_EX|LOCK_NB)` on the DB file (or a sidecar file) at startup and
fail if it is held, or use `PRAGMA locking_mode=EXCLUSIVE`. The kernel drops the lock when
the process dies. Cost: none.

### F8. Unlinked-file rows can survive a crash with valid attrs and `nlink=0`. Severity: STALE-BUT-HARMLESS
**Where:** dir_cache_fs.cc:863-894 (Release: attrs recorded valid, then fds closed, then
the row deleted in a separate transaction) and dir_cache_fs.cc:571-573
(SettleUnlinkedFile with an open fd records `nlink=0` as valid).

A crash between those steps leaves a row whose getattr is served from cache (`nlink 0`)
instead of ESTALE. It self-heals as soon as anything opens it by handle.

**Fix:** decide on deletion inside the same transaction that records the `nlink=0` statx:
delete the row instead of recording attrs when nlink is 0 and no dcfs open remains.

### F9. `journal_mode=WAL` failure is ignored. Severity: THEORETICAL
**Where:** sqlite.cc:408. If WAL cannot be enabled (for example a DB on a filesystem
without shared-memory support), dcfs silently runs rollback-journal plus
`synchronous=NORMAL`. SQLite documents that combination as having a small chance of
*corruption* on power loss, not just rollback.

**Fix:** for a file DB, check that the pragma returns `wal`, and fail startup otherwise.

### F10. ParentOf has no backing fallback, so `..` fails after crash windows. Severity: AVAILABILITY, not wrong data
**Where:** metadata_cache.cc:383-403, dir_cache_fs.cc:248-254.

A crash between phase 1 and phase 3 of a directory rename or rmdir, or the F1(A)
invalidation, leaves a directory with no dentry. `LOOKUP ".."` returns ENOENT until an
ancestor is repopulated. Exportfs reconnect of an NFS directory handle (FUSE get_parent
does LOOKUP "..") can then fail, which undercuts "handles survive restarts".

**Fix:** on NotFound, open the directory by handle, open `..` with O_PATH,
name_to_handle, and UpsertInode it to get the parent id.

---

## Per-area verdicts for (a)-(h)

**(a) Durability.** See F1 and F2. Under a process crash every op's commit boundaries were
checked (table below) and are safe except F3.

**(b) Population.** PopulateDirectory (backing.cc:635-708) does all I/O first, then *one*
transaction: upserts, LinkDentry, symlinks, xattrs, PruneDentriesNotIn and
MarkDirComplete. The nested Transaction() calls are savepoints inside it.

* A crash in phase A writes nothing. A crash in phase B rolls the whole transaction back.
* dcfs is single-threaded with one connection, so there is no concurrent population, and
  none can span a restart.
* Children open for write stay unknown in the same transaction (backing.cc:691-693).
* Nothing is marked complete from data older than the listing.
* The only exposure is power loss (F1). A durable listing can include a create the
  backing lost.

**(c) Generation counter.** A process crash cannot split the row from the counter (one
transaction), and a wiped DB gets a fresh random seed. Since `id - gen` is constant per DB
lifetime, a wipe collides only if the seeds line up, about 2^-32 for the whole population,
not per handle. Power loss reissues (N, G): F2. btrfs identity reuse: F6. Gen-0 row reuse:
F5.

**(d) Row deletion.**

* Rmdir: the ForgetRemoved crash window leaves a row with attrs unknown, and GETATTR/OPEN
  get ESTALE via the handle. Readdir of that nodeid is preceded by OPENDIR, which also
  gets ESTALE.
* Unlink: the file's attrs are unknown until settled, which is fine except F8. For
  symlinks and special files see F5.
* Rename-over: the same as unlink/rmdir for dst.
* Deleted ids are never reused by AUTOINCREMENT except through the power-loss rollback in
  F2.

**(e) Startup.**

* Migrate: CreateSchema, including the random seed, the root row and the directories
  row, runs in one BEGIN IMMEDIATE transaction (migrate.cc:132-137), so the DB cannot be
  left half-migrated. A crash just re-creates it.
* A foreign or garbage DB fails on the missing `schema_version` or the version mismatch.
* The device check is duplicated in main.cc and InitRoot.
* InitRoot runs UpsertRoot and EnsureDirectory in one transaction, so a crash reruns it.
* StartupPurge purges each filesystem in its own transaction and re-reads the list, so it
  is idempotent after a crash.
* Gap: the root identity is never compared (F4).

**(f) Shutdown checkpoint.** A TRUNCATE checkpoint is crash-safe by SQLite's design. A
crash mid-copy leaves the WAL intact to replay. A crash after the DB fsync and before the
truncate replays idempotently. A checkpoint failure is only logged, which is harmless. The
missing piece is making the *backing* durable too (F1(A) step 2).

**(g) Valid/complete from data read before a syscall.**

* `parent_was_complete` restore: correct in RemoveChild, Rename and Link, because phase 1
  marks the parents' attrs unknown so no reconcile runs in phase 2, and the op changes
  only the named dentries. Wrong in CreateChild (F3).
* RecordNewLink stats *after* linkat. Release refreshes *after* `open_for_write_.erase`.
* Every refresh of an inode that is open for write goes through RecordAttrs, which
  re-marks it unknown in the same transaction (backing.cc:109-117). That covers the
  UpsertInode-in-populate path (:691) and RecordNewChild (:766). The only direct
  UpdateAttr caller is RecordAttrs.

**(h) SQLite usage.**

* Every Prepared() reuse resets and clears bindings. ForEachRow and ExecuteOnce reset via
  Cleanup on every path. ReadOne, ListDir, UpsertInode and PruneDentriesNotIn materialize
  rows before writing or calling back, so no cursor is open across writes or transaction
  ends.
* Transaction() correctly unwinds with ROLLBACK TO plus RELEASE for savepoints and ROLLBACK
  for the outer transaction, including after a failed COMMIT.
* No Transaction body swallows a nested error and continues. All `IgnoreError()` calls
  are outside transactions (dir_cache_fs.cc:228, 554, 616-617, 1109-1110, 1196-1197,
  1287). This matters because an automatic full rollback on IOERR/FULL followed by
  swallowing would drop later statements into autocommit.
* MintFuseGeneration RET_CHECKs that it is inside a transaction.
* The only weaknesses: F9 (the WAL pragma result is unchecked) and the fact that
  `synchronous` cannot be toggled inside a transaction (verified at sqlite3.c:145934),
  which matters for how F1/F2 fixes must be written.

## Per-op crash table (process crash; power loss is F1 for all)

| Op | Phase 1 content | Crash 1→2 | Crash 2→3 | Crash inside or between phase-3 transactions | After phase 3 or reply |
|---|---|---|---|---|---|
| Mknod/Mkdir/Symlink/Create | dentry deleted, parent incomplete | unknown, ok | unknown, ok | **parent attrs stale (F3)** | ok |
| Unlink/Rmdir | dentry deleted, parent incomplete, parent+child attrs unknown | ok | ok | parent/child unknown until refreshed, ok. Rmdir leftover row gets ESTALE. Gen-0 child: F5 | ok (F8 for open files) |
| Rename (+NOREPLACE/EXCHANGE) | both dentries deleted, both parents incomplete, parents+src+dst attrs unknown | ok | ok | ok (moved directory loses `..` until repopulated: F10) | ok |
| Link | dentry deleted, newparent incomplete, newparent+src attrs unknown | ok | ok | ok | ok |
| Setattr | attrs unknown | ok | ok | n/a (one transaction) | ok |
| Setxattr/Removexattr | that xattr deleted, xattrs_complete=0, attrs unknown | ok | ok | ok | ok |
| Open(write)/Create(write) | attrs unknown before reply | n/a | writes happen while unknown, ok | Release refresh: ok | ok |
| Write (fallback), Fallocate | attrs unknown | ok | ok | stays unknown while open for write, ok | ok |
| Populate / LookupOrPopulate / Readlink / RefreshXattrs | none (read paths) | n/a | n/a | atomic single transaction, ok | ok |

## Noted in passing (outside the crash scope, unverified or not fully traced)

* **Errors reported for mutations that happened.** Ops return an error *after* a
  successful backing syscall if a phase-3 or refresh commit fails: RemoveChild
  :393-410, Rename :501-540, CreateChild :183-193. The client sees failure for a
  completed unlink/rename/create. The cache is left conservative, so this is a reply
  problem, not a data problem.
* **Stale xattrs from syscalls with xattr side effects.** Setattr (chmod/chown) and
  passthrough writes (killpriv) can change xattrs: the POSIX-ACL mask and
  `security.capability` removal. Phase 1 marks only the attrs unknown, and GetXattr then
  serves the old cached row. *Unverified*: I did not trace whether the kernel routes
  `system.posix_acl_*` to dcfs without FUSE_POSIX_ACL on this kernel.
* **One stale child fails readdirplus.** Readdirplus (dir_cache_fs.cc:1047) fails the whole
  reply if one child's EntryFor returns ESTALE. InvalidateInode marks the directory
  incomplete, so a retry self-heals.
* **Libfuse defaults.** writeback_cache is not enabled by default in the vendored libfuse
  (checked `LL_SET_DEFAULT` list), so Release-time attrs are not undercut by delayed
  writeback. FUSE_CAP_OVER_IO_URING *is* defaulted on. I assumed dcfs still processes
  requests serially; not verified against fuse_ops.cc.

## Verified correct

* Three-phase ordering exists for every mutating op, and every phase-1 transaction commits
  before its syscall (except F3's missing parent attrs).
* All replies are sent after the outer commit that minted any nodeid or generation they
  carry.
* WAL prefix semantics mean phase 3 is never durable without its phase 1.
* Process-crash safety holds for Migrate, InitRoot, StartupPurge, PopulateDirectory,
  RecordNewChild, RecordNewLink, UpsertInode (including recycled-ino invalidation) and
  InvalidateInode (dentries deleted before the row, so ON DELETE SET NULL cannot fabricate
  negatives). PurgeFilesystem removes the set-null boundary negative and marks the parent
  incomplete.
* `open_for_write_` handling: every attrs write for an open-for-write inode re-marks it
  unknown in the same transaction.
* Statement lifetime, savepoint nesting and rollback, and no swallowed errors inside
  transactions.
* The shutdown checkpoint is crash-safe.

## Prioritized fixes

1. **F1(A)**: boot-id unclean-shutdown invalidation, plus syncfs of the backing
   filesystems before a durable "clean" marker at shutdown. Zero steady-state cost, and
   it restores the README promise for power loss. If rejected, at least FULL phase-1
   commits plus a WAL sync in Fsync/Fsyncdir (costs above; fixes one direction only).
2. **F2**: durable hi/lo reservation of `gen_counter`, one FULL commit per 64K mints and
   per start. Without it, NFS handles can hit the wrong file after power loss.
3. **F3**: add `MarkAttrsUnknown(parent)` to CreateChild's phase 1. Trivial, and it fixes
   both the crash window and the undone out-of-band detection.
4. **F5 + F6**: identity checks in UpsertInode and VerifyBackingIdentity using stored
   handle bytes and btime. No syscalls added.
5. **F4**: refuse to start when the root's (ino, gen) differs from the DB.
6. **F7**: single-instance flock on the DB.
7. **F8, F9, F10**: small hardening items.
