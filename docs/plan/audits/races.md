# dcfs race audit (main @ 8d545ef)

Read-only audit. Nothing was modified or run. Kernel references are to
~/Sources/linux @ d530980dfbb7. libfuse references are to the Bazel-fetched
libfuse+ under `$(bazel info output_base)/external/`.

Severity scale used:
- **wrong data**: dcfs (or the kernel, fed by dcfs) serves a value as current
  that is wrong, and nothing in the normal flow corrects it.
- **stale, self-healing**: wrong for a bounded time (entry/attr timeout) or
  until an access that dcfs already makes anyway.
- **cosmetic**: a value nobody should rely on, or the behavior differs from a
  local fs without serving wrong file data or names.

---

## Findings

### F1. Writable shared mmap outlives the last writable RELEASE (passthrough). Severity: wrong data (metadata; to NFS clients, file data too)

**Kernel behavior (confirmed in source).**
- `fuse_file_mmap()` (fs/fuse/file.c:2389-2406) sends a passthrough file to
  `fuse_passthrough_mmap()` (fs/fuse/passthrough.c), which calls
  `backing_file_mmap()` (fs/backing-file.c:329-355).
- `backing_file_mmap()` calls `vma_set_file(vma, file)`, and `vma_set_file()`
  (mm/util.c:328-334) does `get_file(backing); swap(vma->vm_file, ...);
  fput(fuse_file)`. After mmap() the VMA holds only the backing file. The FUSE
  file is no longer pinned by the mapping.
- So `close(fd)` after `mmap(MAP_SHARED, PROT_WRITE)` drops the last reference
  to the FUSE file, and the kernel sends RELEASE while the mapping stays
  writable against the backing inode.
- Page-fault writes then go straight to the backing fs (`page_mkwrite` ->
  `file_update_time` on the backing inode). Nothing reaches FUSE. The only
  callback `backing_file_mmap` makes is `ctx->accessed` once, at mmap time,
  which is `fuse_invalidate_atime`.

**Interleaving.**
1. Thread A: `fd = open(f, O_RDWR)`. dcfs Open: `BeginWriting`, and `f` joins
   `open_for_write_`.
2. A: `p = mmap(fd, MAP_SHARED|PROT_WRITE)`, then `close(fd)`. The kernel sends
   FLUSH and RELEASE.
3. dcfs Release (dir_cache_fs.cc:845-852): `writable_refs` becomes 0,
   `open_for_write_.erase`, and `RecordWrittenAttrs` stores the statx as
   **valid**.
4. A: `memcpy(p, ...)` minutes later. The backing mtime and ctime advance
   (and blocks, after writeback). dcfs has no signal of this.
5. Any GETATTR, LOOKUP or READDIRPLUS is served from the cache with the old
   mtime and ctime, marked valid, with a 1 h attr timeout
   (dir_cache_fs.cc:117-131). Nothing corrects it until an `OpenNode` of that
   inode happens to run `ReconcileAttrs` (backing.cc:372-440). That logs a
   **false** "out-of-band change (unsupported)" WARNING and drops the xattr
   cache.

**Consequences.**
- `make` and rsync-style tools see an unchanged mtime.
- nfsd derives the NFS change attribute from ctime for FUSE, because FUSE
  reports no `STATX_CHANGE_COOKIE` (`nfsd4_change_attribute`,
  fs/nfsd/nfsfh.c:957-972). NFS clients therefore never invalidate their
  cached data for that file. That is stale file **data** on clients.
- Local readers read correct data, because passthrough reads use the backing
  page cache.

**F1b, the same mechanism while the file is still open.** During a writable
open, dcfs keeps the attrs unknown, but every reply still carries
`attr_timeout = 1h`. A passthrough `write()` invalidates the kernel's attrs
through `fuse_passthrough_end_write` -> `fuse_write_update_attr`
(fs/fuse/file.c:1199-1216, `FUSE_STATX_MODSIZE`). An mmap store does not. So
after one GETATTR the kernel serves the pre-store mtime and ctime for up to
1 h, even though dcfs holds them as unknown. Severity: stale, self-healing
(1 h).

**Fix options.**
- **F1b.** Reply `attr_timeout = 0` (entry timeout unchanged) for any inode in
  `open_for_write_`, in `EntryFor` and hence Getattr, Lookup, Readdirplus,
  Create, Setattr and Link. Cost: one GETATTR round trip plus one `statx` on the
  already-open fd per `stat()` of a file that is currently open for writing.
  No disk I/O and no new syscall on any other path.
- **F1, dcfs-only (conservative).** At the last writable Release of an inode
  that had passthrough, move it to an in-memory "possibly mmap-written" set.
  - Store its attrs unknown in the DB, as today while open.
  - Reply with `attr_timeout = 0` and refresh on each GETATTR by reopening by
    handle and running `statx`.
  - In-memory is enough: a dcfs restart implies a remount, so no mapping can
    survive it.
  - Cost: `open_by_handle_at` + `statx` + one SQLite write per stat of a
    recently-written file, for the rest of the session. It usually needs no
    disk I/O while the backing inode is in the icache, but not always.
  - Bound the set with an LRU and accept imprecision at eviction.
  - dcfs cannot detect mmap at all, so this has to apply to every file ever
    written with passthrough.
- **F1, kernel-side.** The owner already carries a patch. Make passthrough
  shared writable mappings keep the FUSE file (or its `fuse_file`) referenced
  until munmap, so RELEASE really means "no more writers". **Unverified
  feasibility**: after `vfs_mmap` the `vm_ops` belong to the backing fs.
- **Otherwise:** document "writes through a MAP_SHARED mapping after close()
  are not reflected in metadata" as unsupported, and remove the false WARNING
  for this case if possible.

### F2. chmod on a file with a POSIX access ACL leaves `system.posix_acl_access` stale in the xattr cache. Severity: wrong data

**Kernel behavior (confirmed in source).**
- dcfs Setattr applies the mode with `fchmod` on the backing inode
  (backing.cc:902-914, 968-980).
- ext4_setattr calls `posix_acl_chmod()` (fs/ext4/inode.c:6243), which
  rewrites the ACL's mask (or group) entry in the backing
  `system.posix_acl_access` xattr. xfs and btrfs do the same.
- dcfs Setattr (dir_cache_fs.cc:203-236) marks only the attrs unknown and
  refreshes only the attrs. `xattrs_complete` stays 1 and the old ACL value
  stays cached.
- FUSE without `FUSE_POSIX_ACL` still sends GETXATTR for ACL names on an
  init-userns mount: `__fuse_get_acl` / `fuse_get_acl` (fs/fuse/acl.c) ->
  `fuse_getxattr`.
- `fuse_get_inode_acl` returns NULL when `!fc->posix_acl`. So the **kernel's
  permission checks do not use the stale ACL**. There is no access-control
  bypass; this is wrong metadata.

**Interleaving.**
1. `setfacl -m u:bob:rwx f` (mask rwx) goes through dcfs Setxattr.
2. `chmod 640 f` -> SETATTR(mode) -> backing chmod; the backing mask becomes
   r--.
3. `getfacl f`, `rsync -A`, `tar --acls`, or an NFS ACL query -> GETXATTR ->
   dcfs returns the cached mask rwx. This never heals: phase 3 records the new
   ctime, so `ReconcileAttrs` never sees a difference.

**Fix.** In Setattr phase 1, when `FUSE_SET_ATTR_MODE` or `KILL_SUID`/`KILL_SGID`
is set (all of them `fchmod` the backing file), forget the cached
`system.posix_acl_access`. Either use `ForgetXattr`, which also clears
`xattrs_complete`, or delete the row only if it exists, which keeps "complete"
when there is no ACL. Cost: none on the chmod path. The next getxattr or
listxattr of that file rereads its xattrs (one `listxattr` plus `getxattr` per
name), and only for files that had an ACL if the conditional variant is used.

### F3. Setxattr caches the value the client sent, not what the backing fs stored. Severity: wrong data

**Kernel behavior (confirmed in source).**
- ext4, xfs and btrfs `set_acl` call `posix_acl_update_mode()`
  (fs/posix_acl.c:712-729; ext4 acl.c:252, xfs_acl.c:262, btrfs acl.c:115).
  When the ACL is equivalent to the mode bits, this sets `acl = NULL`, so the
  backing xattr is **removed**, not stored.
- dcfs Setxattr phase 3 then calls `cache::SetXattr(id, name, value)`
  (dir_cache_fs.cc:1116), which caches the value as present.

**Interleaving.**
1. `setfacl --set u::rw-,g::r--,o::r-- f` (or any ACL-copying tool) ->
   SETXATTR(`system.posix_acl_access`) -> the backing fs stores nothing and
   updates the mode.
2. dcfs caches the xattr.
3. Results:
   - `getxattr(f, system.posix_acl_access)` returns a value that does not
     exist on the backing fs. It is served directly from the per-name row even
     though `xattrs_complete = 0`.
   - `listxattr` refreshes (complete = 0) and does **not** list the name. That
     is internally inconsistent.
   - After a later `ReplaceXattrs` the stale row disappears. So it heals only
     when something rereads the whole set.

**Also, unverified:** LSMs can canonicalize `security.*` values (for example,
SELinux context translation), which would give the same "cached value !=
stored value" result.

**Fix.** For `system.posix_acl_*` (and `security.*`), after a successful
backing setxattr, do not cache the sent value. Either `ForgetXattr` (already
done in phase 1, so just skip the phase-3 `cache::SetXattr` for these names),
or read the value back with `fgetxattr` on the fd already open for the
operation. Cost: zero, or one `getxattr` syscall on the setxattr path, which
is not a hot path.

### F4. libfuse can turn dcfs multi-threaded behind its back (FUSE-over-io_uring). Severity: wrong data / memory corruption if triggered (configuration race, not a request race)

**libfuse behavior (confirmed in source).**
- `do_init` sets `LL_SET_DEFAULT(1, FUSE_CAP_OVER_IO_URING)`
  (fuse_lowlevel.c:2813).
- It enables io_uring when `se->uring.enable && want_ext &
  FUSE_CAP_OVER_IO_URING` (2923-2925). `se->uring.enable` comes from
  **`FUSE_URING_ENABLE` in the daemon's environment** (4179-4181; default 0 per
  `SESSION_DEF_URING_ENABLE`, fuse_uring_i.h:22) or from `-o io_uring`, which
  dcfs passes through from `--fuse_opt` (main.cc:165-174).
- `fuse_uring_start` then creates `get_nprocs_conf()` queue threads
  (fuse_uring.c:511, 864-867), each calling the op handlers concurrently.

**What breaks.** Every assumption in this audit's "correctly handled" list, and
the unsynchronized `open_files_`, `backing_files_`, `open_for_write_` and
`sqlite3::Connection`, would race.

**Fix.** In `DirCacheFS::Init`, `fuse_unset_feature_flag(&conn,
FUSE_CAP_OVER_IO_URING)`. It must run in `init`, which libfuse calls before the
2923 check (2826). Optionally also reject `io_uring` in `--fuse_opt`. Cost:
none.

### F5. Phase ordering is not durable across power loss / kernel crash (WAL + `synchronous=NORMAL`). Severity: wrong data after an unclean system shutdown

**Code.** sqlite.cc:408-410 sets `journal_mode=WAL` and `synchronous=NORMAL`.
Per SQLite's documentation (not source-verified here), a WAL commit under
NORMAL survives a process crash but can roll back on power loss or an OS
crash. The three-phase argument in the README holds only for **daemon**
crashes.

**Interleaving (Unlink).**
1. Phase 1 commits `(P, x)` as unknown. It is in the WAL, not yet synced.
2. `unlinkat` runs; the backing journal commits within a few seconds.
3. Phase 3 commits `(P, x)` as negative, with the listing complete.
4. Power is lost.
5. On reboot either direction can be wrong:
   - **WAL lost, backing kept:** the cache has `x` positive and valid while
     the backing file is gone. Lookups return a positive entry; OPEN gets
     ESTALE; this heals on access.
   - **Backing journal lost, WAL kept:** `x` exists on the backing fs, but the
     cache says it is negative in a complete listing. `x` is invisible forever.
     That is not self-healing.
   - The same applies to Create (a phantom or invisible name), Rename (an
     entry under the wrong name), and BeginWriting (phase 1 lost while
     passthrough data and size reached disk, so the pre-write size and mtime
     are served as valid).

**Fix options.**
- **Cheapest robust option.**
  - Keep a "clean shutdown" flag in `meta`: set it after the final checkpoint
    in main.cc, and clear it (synchronously) at startup.
  - On startup without the flag: mark all attrs unknown, delete all dentries
    (positive and negative) and clear `children_complete`, and mark all xattrs
    incomplete. Keep inode rows, so handles and fuse_gen survive.
  - Cost: a cold namespace after an unclean shutdown only; zero on the hot
    path.
- **Alternative.** `synchronous=FULL`, or switch to FULL just for phase-1
  transactions. Cost: one fsync of the WAL per mutating op, including every
  writable open and create. Measurable on untar-like workloads, and it still
  does not fix the "backing lost, phase 3 kept" direction.

### F6. `CreateChild` does not mark the parent's attrs unknown in phase 1. Severity: stale, self-healing; daemon-crash only

**Code.** dir_cache_fs.cc:161-166. Phase 1 only runs
`MarkUnknown(parent, {name})`. RemoveChild (377), Rename (478-479) and Link
(604) all mark the parent's attrs unknown. Mkdir, Mknod, Symlink and Create do
not.

**Interleaving.** mkdir `P/d`; `mkdirat` succeeds; the daemon crashes before
`RefreshAttrsFromFd(parent)` at 193. After restart:
- P's cached mtime, ctime and nlink are the pre-mkdir values, marked valid.
- `stat P` serves them.
- An NFS client uses P's change attribute (ctime) to decide whether its
  cached readdir of P is still good, so it keeps a listing without `d`.
- It heals only when P is repopulated (its `children_complete` was cleared, so
  the next readdir or lookup-miss in P does that), and then logs a false
  out-of-band WARNING.

**Fix.** Add `cache::MarkAttrsUnknown(ctx_, parent)` to CreateChild's phase-1
transaction. Phase 3 already refreshes from the open fd. Cost: none.

### F7. Phase-3 bookkeeping failures after a successful phase 2 are replied as op failures. Severity: stale, self-healing (up to entry/attr timeout, 1 h)

**Code.**
- Rename: dir_cache_fs.cc:501-540. Phase 3 and the `RefreshAttrs`,
  `SettleUnlinkedFile` and `ForgetRemoved` calls all run after `renameat2`
  succeeded, and any error returns through `ReplyFailureAndLogIfNotOk`.
- The same applies to RemoveChild (393-410), CreateChild (183-193), Link
  (622-631), Setattr (233), Setxattr and Removexattr (1116-1117, 1202-1203).

**Interleaving.**
1. `rename(a, b)`: `renameat2` succeeds. Then, for example, `RefreshAttrs(src)`
   fails with EIO or ENOMEM, or a SQLite error.
2. The kernel receives an error. VFS does not `d_move`, so the kernel keeps
   positive `a` and negative `b` (cached for 1 h).
3. dcfs and the backing fs say `b` exists and `a` does not.
4. Applications see the old namespace until the dentries expire or an OPEN
   gets ESTALE and triggers a revalidate.
5. Also, the caller is told the rename failed when it happened.

**Fix.** Once phase 2 succeeds, treat any later cache or refresh failure as
"leave unknown and log", and reply success, as Release and Flush already do.
- For Create, Mkdir, Mknod, Symlink and Link, which must return an entry: if
  `RecordNewChild` fails, reply from the `statx` already in hand, or mark the
  row unknown and retry `EntryFor`.
- Do **not** call `fuse_lowlevel_notify_inval_entry` from inside the handler:
  the kernel holds the parent's `i_rwsem` for the in-flight op, and the reverse
  invalidation takes it, which is an unverified but likely deadlock.

Cost: none.

### F8. Readdir can return an unchanged name twice (and, in theory, skip one) across concurrent mutations. Severity: cosmetic / POSIX-edge

**Code.**
- `MarkUnknown` **deletes** the dentry row (metadata_cache.cc:675-686). Any
  later re-insert gets a new rowid, and the readdir cookie is the rowid
  (dir_cache_fs.cc:943-945, 1006; metadata_cache.cc:242-274).
- That defeats the intent stated at `PutDentry` (metadata_cache.cc:568-571:
  "REPLACE ... would make an in-progress ListDir see a renamed-over name
  twice"). Rename phase 1 deletes `newname`, so phase 3's `LinkDentry` inserts
  it fresh anyway.

**Interleavings.**
- **Rename-over.** Process A is part way through reading directory P; its
  cookie is past `b`. Process B runs `rename(P/a, P/b)` where `b` existed.
  `b` gets a new, maximal rowid, and A's next getdents returns `b` again.
- **Failed removal.** B runs `rmdir(P/d)` and gets ENOTEMPTY (common). Phase 1
  deleted `d`'s row; `ReresolveAfterFailure` repopulates P and reinserts `d`
  at a new rowid. A sees `d` twice although `d` never changed.
- The same happens for a failed unlink, rename or mkdir.
- **Theoretical skip.** dentries has no AUTOINCREMENT, so a new rowid can
  reuse a deleted maximum and land at or below a live cursor. Omission is
  possible in principle. Unverified by test.

**Fix options.**
- Remember the rowid in phase 1 and reinsert with the same rowid when the
  name is re-established. Cost: one SELECT in phase 1.
- Or add an explicit "unknown" state column instead of deleting, so
  `MarkUnknown` becomes an UPDATE, rowids are stable, and `Lookup` and
  `ListDir` treat the state as absent. Cost: a schema change; no syscalls.

### F9. atime (and `st_blocks`) are served as valid while passthrough I/O and writeback change them. Severity: cosmetic

**Kernel behavior (confirmed in source).** A passthrough read calls
`fuse_file_accessed` -> `fuse_invalidate_atime` (fs/fuse/passthrough.c,
fs/fuse/dir.c:335-339). The next `stat` sends GETATTR, and dcfs answers from
the cache (valid), so it returns the atime from before the read. `relatime`
updates on the backing fs are never picked up. The README says `ReconcileAttrs`
deliberately ignores atime and blocks, but the cache still presents them as
current.

**Effects.**
- mbox "new mail" detection (atime < mtime) misfires.
- `st_blocks` recorded at Release can drift after delayed allocation and
  writeback. The size of the drift is unverified per filesystem (ext4 counts
  delalloc blocks in `getattr`; btrfs compression is unverified).

**Fix (optional).** At the Release of the last reference of any mode,
`statx` the still-open shared fd and write only if something changed. Cost:
one `statx` on an open fd per last close (no disk I/O) and a SQLite write only
when the value differs. Or accept it and document that atime is not
maintained.

### F10. Rows are deleted while the kernel still legitimately references the nodeid. Severity: cosmetic (ESTALE where a local fs succeeds)

**Code.** `Forget` is a no-op (dir_cache_fs.cc:270-280). Rows are deleted by
RemoveChild (407), `SettleUnlinkedFile` (581) and Release (888-894) with no
regard for the kernel's lookup count.

**Interleavings.**
- **Removed working directory.** A shell's cwd is `D`; `rmdir D` (from
  elsewhere) deletes the row. Then `ls` in the shell -> OPENDIR(D) ->
  `RequireAttr` -> ESTALE, where a local fs gives an empty listing, and
  `stat .` gives "Stale file handle".
- **O_PATH fd.** A process holds an O_PATH fd on `f` (no FUSE OPEN is sent);
  `f` is unlinked; `fstat` -> GETATTR -> ESTALE, where a local fs returns
  nlink 0.
- **/proc reopen racing the last close.** `open("/proc/pid/fd/N")` of an
  unlinked open file races the owner's close. RELEASE is async and OPEN is
  sync, so either order reaches dcfs. If RELEASE is processed first, the row
  is deleted and the OPEN gets ESTALE; the LOOKUP_REVAL retry then fails. A
  local fs would succeed if the path walk won. This one is unverified as a
  timing window.

**Fix (only if wanted).** Track nlookup per nodeid in memory (Lookup, Create,
Mkdir, Readdirplus and so on increment it; Forget decrements it), and for
removed objects keep a "dead" row, serving nlink 0 and an empty listing, until
Forget drops the count to 0. Cost: an in-memory map and more code. No
syscalls.

---

## Checked and correctly handled

1. **The owner's case: a passthrough writer while others stat, read or open.**
   Every attr-recording path goes through `RecordAttrs` (backing.cc:109-117),
   which re-marks the attrs unknown for ids in `open_for_write`. The paths
   covered are `RefreshAttrs`, `RefreshAttrsFromFd`, `RecordNewLink` and
   `ReconcileAttrs`, plus `PopulateDirectory` (backing.cc:689-693) and
   `RecordNewChild` (766-768). `ReconcileAttrs` skips invalid rows, so it does
   not produce false out-of-band warnings for files being written.
2. **ftruncate on a passthrough fd.** The kernel sends FUSE_SETATTR:
   `do_truncate` -> `fuse_setattr`, and there is no passthrough branch in
   fs/fuse/dir.c. Setattr is write-through, and the phase-3 refresh stays
   unknown while the file is open for writing. O_TRUNC opens (with
   `ATOMIC_O_TRUNC` off) send OPEN **then** SETATTR (fs/namei.c `do_open`:
   `vfs_open` then `handle_truncate`), not SETATTR first as the Init comment
   at dir_cache_fs.cc:67-77 says. This is harmless, since `fuse_send_open`
   strips O_TRUNC (fs/fuse/file.c:34), so the RET_CHECK in Open holds. The
   comment is inaccurate.
3. **fallocate.** `fuse_file_fallocate` always sends FUSE_FALLOCATE (no
   passthrough) and calls `file_modified()` first. dcfs Fallocate is
   write-through, and its refresh respects `open_for_write`.
4. **copy_file_range / splice / sendfile / O_APPEND / AIO and io_uring
   writes.**
   - FUSE has `->copy_file_range`. dcfs has no handler, so it gets ENOSYS and
     the kernel returns EOPNOTSUPP without a splice fallback
     (fs/read_write.c:1594-1611). Userspace then falls back to read/write on
     the same fds, which go through passthrough inside a writable open.
   - splice goes through `fuse_passthrough_splice_write`, which also runs the
     `end_write` invalidation.
   - The per-FUSE-file backing file is opened with the user's own `f_flags`
     (`backing_file_open(file, file->f_flags, ...)` in `fuse_passthrough_open`),
     so O_APPEND is honored by the backing fs.
   - Async writes hold a file reference until completion, so RELEASE cannot
     overtake them.
5. **suid and `security.capability` removal on passthrough writes, truncate
   and chown.** `backing_file_write_iter` and `backing_file_splice_write` call
   `file_remove_privs()` on the **FUSE** file first (fs/backing-file.c), and
   FUSE `fallocate` calls `file_modified`. On the FUSE inode that leads to:
   - `cap_inode_need_killpriv` -> GETXATTR;
   - `fuse_setattr` -> GETATTR, then SETATTR(mode) for suid (no
     `HANDLE_KILLPRIV` requested; libfuse does not default it);
   - `setattr_prepare` -> `security_inode_killpriv` (fs/attr.c:224-232, called
     from `fuse_do_setattr`, dir.c:2172) -> REMOVEXATTR.

   All of these reach dcfs and update its cache before the backing write runs,
   so the cached mode and xattrs stay consistent. Also, libfuse mounts with
   `MS_NOSUID|MS_NODEV` by default (mount.c:653), so exec would ignore suid and
   file caps unless `--fuse_opt=suid` is passed.
6. **Kernel GETATTR racing passthrough writes.** A stale reply is dropped by
   the kernel's `attr_version` check (`fuse_write_update_attr` bumps
   `fi->attr_version`). dcfs's own copy stays unknown.
7. **Last writable RELEASE racing a new writable OPEN, in either order,
   including OPEN overtaking a background-queued RELEASE.** `writable_refs`
   and `open_for_write_` are updated in dcfs's single thread before any reply.
   No write can go through a FUSE file whose RELEASE has been sent, except
   through mmap (F1).
8. **Reply versus commit ordering.** Every op replies after its phase-3
   transaction. Open and Create commit `BeginWriting` / `MarkAttrsUnknown`
   before `ReplyOpen` / `ReplyCreate`. An OPEN reply cannot be lost to an
   interrupt: once a request is sent, `request_wait_answer`
   (fs/fuse/dev.c:702-750) always waits for the answer, so there is no leaked
   `open_for_write_` entry.
9. **Kernel dentry and attr caches after dcfs's own mutations.** The kernel
   invalidates what dcfs changes: `fuse_dir_changed`, `fuse_update_ctime` on
   rename, link and setxattr, and `fuse_entry_unlinked`. dcfs is right not to
   call `notify_inval_*`.
10. **Negative dentries and complete listings.** They can only be invalidated
    by dcfs-mediated ops or out-of-band changes, which are unsupported. Kernel
    lookup (shared `i_rwsem`) and create, unlink and rename (exclusive) are
    serialized per directory. Revalidate LOOKUPs are consistent snapshots
    because dcfs is single-threaded.
11. **LOOKUP "." and ".." during renames.** dcfs processes RENAME atomically
    with respect to other requests, so ".." returns either the old or the new
    parent. `exportfs` reconnect re-verifies and retries. "." on a deleted row
    gives ESTALE through `RequireAttr`.
12. **Read-only opens cannot write through passthrough mmap.** `VM_MAYWRITE`
    is decided from the FUSE file before `backing_file_mmap` swaps
    `vm_file`, even though dcfs's shared fd is O_RDWR.
13. **Unlink or rename-over of a file open through dcfs.**
    `SettleUnlinkedFile` refreshes from the open fd; Release deletes the row
    at nlink 0 after its own final refresh. Hard links share a row, and
    `open_for_write_` is keyed by row.
14. **Generation and nodeids.** Ids are never reused and fuse_gen comes from
    the row, so a deleted row cannot be re-served under an old
    `(nodeid, gen)` pair.
15. **Symlink targets.** They are immutable per backing inode, so caching them
    is always safe. `FUSE_CAP_CACHE_SYMLINKS` is safe for the same reason.

---

## Other observations (not races, noticed while reading)

- **Double umask.** `fc->dont_mask` is 0 (fs/fuse/inode.c:1776-1778 sets
  `SB_POSIXACL` without setting `dont_mask`), so the kernel already applies the
  caller's umask (fs/fuse/dir.c:852-853 and elsewhere). dcfs then calls
  `mkdirat`, `openat(O_CREAT)` and `mknodat` with its **own** process umask
  still in effect; nothing in dcfs calls `umask()`. The result is that a user
  with umask 002 gets group-write stripped if dcfs runs with 022. For parents
  with a default ACL the backing fs ignores umask, but the kernel has already
  masked the mode. Not verified at runtime; the syscall semantics are certain.
  Fix: `umask(0)` at startup, or honor the `umask` field with FUSE_DONT_MASK.
- **Inconsistent generation-0 handling.** `ReadGeneration` returns 0 on
  EACCES/EPERM/ENOTTY (backing.cc:348-370). `VerifyBackingIdentity` treats 0
  as "same object" (459), but `UpsertInode` treats a 0 against a stored
  nonzero generation as a **different** object and invalidates the existing
  row (metadata_cache.cc:472-502), even if that row is open or in
  `open_for_write_`. The object is then split into two rows, and the old
  nodeid gets ESTALE. This only happens if the generation read fails
  transiently for one object (an LSM denial, for example). Unverified whether
  that occurs in practice.
- **Setattr and xattr ops ignore the open fd.** Setattr ignores `fi` and
  `OpenFdOf` and always reopens by handle. The comment at
  dir_cache_fs.cc:208-212 ("our open fds are always read-only") is outdated,
  since the shared fd is O_RDWR. Using the open fd when there is one saves an
  `open_by_handle_at` and avoids depending on handle decode of
  unlinked-but-open inodes. ext4 decodes those fine (fs/ext4/inode.c:5380 only
  rejects `i_mode == 0`); other filesystems are unverified. `ApplyXattrOp`
  likewise always runs `OpenNode` first.
- **Failed ops re-list the whole parent.** A failed Unlink, Rmdir, Rename or
  create re-populates the whole parent (`ReresolveAfterFailure` ->
  `PopulateDirectory`, because phase 1 cleared `children_complete`). That
  probes every child: `openat`, `statx`, `name_to_handle`, generation and
  xattrs. rmdir with ENOTEMPTY is common, and this can spin up disks. A failed
  syscall changed nothing on the backing fs, so re-probing only the named
  entries and restoring `parent_was_complete` would be enough.

---

## Prioritized list

1. **F4.** Unset `FUSE_CAP_OVER_IO_URING` in Init. Trivial, and it prevents
   arbitrary corruption from one environment variable or option.
2. **F1 / F1b.** Writable mmap outliving RELEASE, and 1 h kernel attr timeouts
   for open-for-write inodes. Do F1b now (`attr_timeout = 0` while in
   `open_for_write_`, which is cheap). Decide between the dcfs-side "suspect
   after release" set, the kernel change, or documenting it as unsupported.
3. **F2.** chmod leaves a stale `system.posix_acl_access`. Forget it in
   Setattr phase 1 when the mode changes. No syscall cost.
4. **F3.** Setxattr caches the sent value for `system.posix_acl_*` and
   `security.*`. Do not cache it, or read it back.
5. **F5.** Durability ordering across power loss. Add the clean-shutdown flag
   and invalidate the namespace and attrs on an unclean start.
6. **F7.** After phase 2 succeeds, never reply with an error; log and leave
   state unknown instead.
7. **F6.** Add `MarkAttrsUnknown(parent)` to CreateChild's phase 1.
8. **F8.** Keep rowids stable across `MarkUnknown` / re-insert.
9. **F9.** Refresh atime at the last close, or document it as unmaintained.
10. **F10.** nlookup-aware "dead" rows, only if the ESTALE behavior bothers
    anyone.
11. **Non-race items.** Fix the umask (probably a real user-visible bug) and
    the generation-0 inconsistency.
