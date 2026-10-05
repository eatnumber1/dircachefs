# Tri-state audit of dcfs cached records (main @ 03aa81b)

Read-only audit. Files: dcfs/schema.sql, dcfs/metadata_cache.{h,cc}, dcfs/backing.{h,cc},
dcfs/dir_cache_fs.{h,cc}, dcfs/sqlite.cc, README.md. Kernel claims checked against
~/Sources/linux where marked "verified"; libfuse against ~/Sources/libfuse.

## The owner's rule

> "in general we need all [or] most of the records we have in the database which represent
> something on the backing file to have three states: present, absent, unknown, and they need to
> get set at appropriate times (e.g. xattr writes set unknown, then write to backing, then set
> present)."

What the rule needs, stated precisely. For each record, at every commit point:

- **R1 (crash):** if a backing syscall may have happened and phase 3 has not committed, the record
  reads as *unknown*.
- **R2 (reader):** every reader can tell present, absent and unknown apart. Unknown is never served
  as present or as absent.
- **R3 (population):** a population (a read from backing followed by a cache write) never commits
  present/absent over a record that a mutation changed, or is still changing, after the population's
  I/O began.

A concurrent reader can linearize before or after an in-flight mutation. So serving the pre-mutation
value during phase 2 is not wrong in itself. What is wrong:

- stale data that outlives the mutation's reply;
- a crash leaving stale data marked current;
- a population *writing* present from a snapshot that raced a mutation.

Today's design gets R1 mostly right, R2 mostly right, and R3 not at all. Nothing guards R3.

### Kernel serialization (why some races can't happen today via the kernel, and why that isn't enough)

Verified or strongly believed:

- The kernel holds the parent's `i_rwsem` exclusively for create/mkdir/mknod/symlink/link/unlink/rmdir/rename.
- It holds the parent shared for lookup and readdir (`fs/fuse/dir.c:2450 .iterate_shared`, verified).
- dcfs does not request `FUSE_CAP_PARALLEL_DIROPS`. libfuse does not default it on (verified,
  `lib/fuse_lowlevel.c:2961-2977`). So FUSE also serializes lookup and readdir per directory
  (`fs/fuse/inode.c:595`, verified).
- setattr and setxattr hold the target inode's lock.
- **getattr, getxattr, listxattr and readlink take no inode lock** (believed, not re-verified here).
- **NFS `get_parent` (LOOKUP "..") holds no lock on the child directory** (believed).

So under coroutines, namespace ops in directory D cannot interleave with lookup, readdir or
population *of D* when they arrive from the kernel. These still can interleave:

- (a) unlocked readers (getattr, getxattr, listxattr, readlink) vs. any mutation of the same inode;
- (b) PopulateDirectory(D) vs. mutations of a child C's *own* records: setattr, setxattr, a writable
  open, or link/unlink of C through another directory;
- (c) hard links across directories;
- (d) dcfs-internal cache writes that no kernel lock covers: `InvalidateInode` from an ESTALE in
  `OpenNode`, `ReconcileAttrs`, `UpsertInode`'s recycled-number invalidation, `ReresolveAfterFailure`;
- (e) NFS reconnect.

The README (line ~160) also plans SQLite I/O through an io_uring VFS. Then *every DB call* becomes
a suspension point, and even "read, then write in a second transaction" sequences race. dcfs should
not rely on kernel locking for cache invariants.

---

## 1. Inventory: how each record type encodes its states today

| Record | present | absent | unknown | Explicit? |
|---|---|---|---|---|
| **Per-inode attributes** (`inodes.attrs_valid` + 16 cols, schema.sql:57-73) | row exists, `attrs_valid=1` | the object doesn't exist: no row (can't be told from "forgotten") | `attrs_valid=0` (values kept as a stale hint) | Yes (2 states + row). "Absent" doesn't make sense per object; removal is expressed through dentries. |
| **Dentry** (`dentries`, schema.sql:82-89) | row with `inode NOT NULL` | row with `inode IS NULL`, **or** no row while `directories.children_complete=1` | no row while the parent is incomplete (or has no `directories` row) | **Implicit:** unknown = row missing AND parent incomplete |
| **Directory listing completeness** (`directories.children_complete`, :91-94) | `1` = every name without a row is absent | — | `0`, or no `directories` row | Boolean. One flag for all names of the dir. |
| **Symlink target** (`symlinks`, :96-99) | row | the object isn't a symlink (read from `mode`) | no row | 2 states; the target is immutable per identity |
| **Xattr value** (`xattrs`, :101-106) | row | no row while `xattrs_complete=1` | no row while `xattrs_complete=0` | **Implicit:** unknown = row missing AND set incomplete |
| **Xattr name set** (`inodes.xattrs_complete`, :74) | `1` | — | `0` | Boolean. One flag for all names. |
| **File handle** (`inodes.handle_type/handle`) | non-NULL (every non-root row gets one from UpsertInode) | — | NULL (root before InitRoot only) | Immutable per identity |
| **Backing identity** (`device_id, backing_ino, backing_gen`) | exact values | — | `backing_gen=0` means "unknown generation" (backing.cc:349-371) | Implicit sentinel 0. Its readers disagree about it (F9). |
| **filesystems rows** | row | no row, and the child isn't a boundary | no unknown state; runtime mounts are unsupported and StartupPurge revalidates | Dormant after 4.8 |
| **open-for-write** | in-memory `open_for_write_` set (dir_cache_fs.h:303) | not in the set | — | In memory. Its durable effect is `attrs_valid=0` for as long as the file is open. |

## 2. Readers: what each returns in each state

| Reader | present | absent | unknown | Notes |
|---|---|---|---|---|
| `cache::Lookup` (metadata_cache.cc:169) | kFound | kNegative (only when a negative row exists) | kUnknown for a missing row, **including a missing row in a complete dir** | The caller resolves it |
| `backing::LookupOrPopulate` (backing.cc:711) | kFound | kNegative; converts "missing and complete" into a negative row (:740) | populates, then re-looks-up; if still missing, SetNegative **without re-checking completeness** | Correct only while invariant **I1** holds: *complete ⇒ every missing name is absent*. F4 breaks I1. |
| `cache::ListDir` (:242) | yields the row | skips it | cannot represent it; callers must check `IsDirComplete` first (Readdir does, dir_cache_fs.cc:975, 1017) | OK |
| `cache::IsDirComplete` (:276) | — | — | false when there is no row | OK |
| `cache::ParentOf` (:383) | parent id | — | **NotFound → ENOENT** (status.cc:44) | **Treats unknown as an error (F6)** |
| `cache::GetAttr` (:190) | `valid=true` | NotFound (no row) | `valid=false`, stale values | Callers that ignore `valid`: Readdir (dir_cache_fs.cc:1001-1004) uses only the type bits and `backing_ino`; Rename (:533-534) uses only `S_ISDIR`. Both are immutable per identity, so these are safe. Release (:870) checks `valid`. |
| `EntryFor` (dir_cache_fs.cc:117) | serves | ESTALE | refreshes (RefreshAttrsOf), then serves | Population path; see F1 |
| `cache::GetXattr` (:337) | value | NotFound → ENODATA | nullopt → full RefreshXattrs | Correct encoding, but serves stale "present" rows (F2, F3) |
| `cache::ListXattrs` (:321) | names | — | nullopt (whole set) → full RefreshXattrs | Coarse |
| `cache::Readlink` (:289) | target | — | NotFound → fill-in | OK (immutable) |
| `cache::GetHandle` (:359) | handle | — | NotFound | OK |
| `backing::ReconcileAttrs` (backing.cc:373) | compares | — | no-op when `!cached.valid` | Its `cached` snapshot can be stale (F11) |

Only ParentOf treats unknown as absent (it answers ENOENT). Every other reader distinguishes the
states correctly. The real problems are writers: they write present from stale data (F1, F2), fail
to set unknown (F3, F5), or overwrite unknown from a snapshot (F4).

---

## 3. Findings

Severity key: **[TODAY]** wrong data possible on main now; **[CRASH]** wrong only after a crash on
main; **[CORO]** wrong only under the planned coroutine model; **[COARSE]** correct but over-invalidates.

### F1 [CORO, HIGH]: No population is guarded against concurrent mutations (R3)

Every population writes present unconditionally:

- `RecordAttrs` → `cache::UpdateAttr` sets `attrs_valid = 1` (backing.cc:110-118; metadata_cache.cc:709-721, via `DCFS_ATTR_ASSIGNMENTS` :110).
- `RefreshXattrs` → `ReplaceXattrs` sets `xattrs_complete = 1` (backing.cc:575-580; metadata_cache.cc:754-766).
- `PopulateDirectory` phase B (backing.cc:659-708) does `UpsertInode` (attrs valid, :689), `ReplaceXattrs` (:703), `SetSymlink`, `LinkDentry`, `PruneDentriesNotIn` and `MarkDirComplete(true)`. It does all of this from I/O done in phase A (:637-656), possibly long before: one openat, statx, name_to_handle, ioctl, listxattr and N getxattr per child.
- `RecordNewLink` (:822-831) and `ReconcileAttrs` (:431-440).

Nothing checks whether a mutation began or finished after the population's I/O started. The only
check-at-commit guard in the codebase is `OpenForWrite()` inside `RecordAttrs` and
PopulateDirectory (:113, :692), and it is the right pattern.

**Interleaving A (attrs, stale forever).**

1. A: `Getattr(X)` finds attrs unknown (for example, Setattr B's phase 1 at dir_cache_fs.cc:219 already ran). It goes to `RefreshAttrs`. The statx at t1 happens before B's chmod lands.
2. A suspends.
3. B's `fchmod` completes. B's phase 3 `RefreshAttrs` statx's the new mode and commits `valid=1` with it.
4. A resumes and commits `valid=1` with the **old** mode.

The cache now serves the old mode indefinitely, with a 1 h attr_timeout. getattr takes no inode
lock, so the kernel does not serialize A against B. **Wrong data.**

**Interleaving B (xattrs, stale forever; this is the owner's case).**

1. B: `Setxattr(X, n, v2)` phase 1 (`ForgetXattr` + attrs unknown, dir_cache_fs.cc:1094-1097). B suspends in `fsetxattr`.
2. A: `Getxattr(X, n)` gets nullopt (row gone, set incomplete) and calls `RefreshXattrs`. `listxattr`/`getxattr` read `n=v1` before B's syscall lands. A suspends.
3. B's syscall completes. B's phase 3 `cache::SetXattr(n, v2)` commits.
4. A commits `ReplaceXattrs({n: v1, ...})`, which deletes all rows, reinserts v1 and sets `xattrs_complete=1`.

Every later getxattr/listxattr serves v1 as authoritative. **Wrong data.** getxattr and listxattr
take no inode lock, so the kernel does not prevent this.

**Interleaving C (population of D vs a child mutation).**

1. A: `Readdir(D)` → `PopulateDirectory(D)` probes child C's attrs and xattrs.
2. B: `Setattr(C)` or `Setxattr(C)` holds C's lock, not D's, and runs phases 1-3 to completion.
3. A commits `UpsertInode(C, old stx)` and `ReplaceXattrs(C, old)`.

Stale forever. Also: `ReconcileAttrs` at :684 compares commit-time cache (new) with probe-time stx
(old). It logs a **false** "out-of-band change" warning and *adopts the old values*. The same holds
for link/unlink of C through another directory E, which changes C's nlink and locks E and C but
not D.

**Interleaving D (transient).** If A commits between B's phase 1 and phase 3, readers see
`valid=1` during B's phase 2. That is linearizable-before-B, so acceptable, as long as B's phase 3
is the last writer. Interleavings A through C are exactly the cases where it isn't.

**Proposal: an optimistic version check on every population commit, plus an in-flight guard.**

- Add a per-record-class epoch: `inodes.attrs_epoch`, `inodes.xattrs_epoch`, `directories.epoch`.
  Every phase-1 and every phase-3 write bumps it (`UPDATE ... SET x_epoch = x_epoch + 1`).
- Each mutation also increments an in-memory `inflight[record]` counter in phase 1 and decrements it
  after phase 3. It is non-durable; a crash makes everything in flight unknown via R1 or 4.10's
  RecoverDirty anyway.
- A population snapshots the epoch *before* its first syscall. It commits with
  `UPDATE ... SET ..., state='present', x_epoch = x_epoch + 1 WHERE id = ? AND x_epoch = :snap`,
  and only when `inflight == 0`. If zero rows change, it discards the cache write, still uses its
  fresh data for *its own reply*, and leaves the record unknown.
- PopulateDirectory applies this per child (skip only the children whose epochs moved) and per
  directory (don't set `children_complete` if `directories.epoch` moved).

The epoch can also live purely in memory (`flat_hash_map<key, uint64>`), since it guards only
in-process concurrency. A DB column is simpler to use in SQL predicates such as PruneDentriesNotIn,
and costs 8 bytes a row and no extra I/O (read in the same SELECT, checked in the same UPDATE).

API cost: every population write function (`UpdateAttr`, `UpsertInode`, `ReplaceXattrs`,
`LinkDentry` from populate, `MarkDirComplete`, `SetNegative` from LookupOrPopulate) takes an
`expected_epoch`. Readers such as `EntryFor` and `Getxattr` must use the data the refresh *returns*,
not re-read the cache. Today `Getxattr` (dir_cache_fs.cc:1142) and `Listxattr` (:1165) `RET_CHECK`
that the re-read is known; that turns a lost race into an internal error once commits can be
rejected (and even today, if SQLite I/O becomes a suspension point).

### F2 [TODAY, HIGH]: Setxattr phase 3 caches the caller's value, not the backing's

At dir_cache_fs.cc:1116, `cache::SetXattr(ctx_, id, name, value)` records exactly what the client
sent. The backing filesystem can store something else:

- **POSIX ACLs.** Without `FUSE_POSIX_ACL` (dcfs never requests it), `fuse_set_acl` forwards the
  ACL as a plain setxattr of `system.posix_acl_access` (`fs/fuse/acl.c:93-160`, verified). On
  ext4, `ext4_set_acl` → `posix_acl_update_mode` (`fs/ext4/acl.c:252`, verified) turns an ACL
  equivalent to the mode bits into *no xattr*, and rewrites the mode.
- `security.*` LSM labels can be canonicalized, and `security.capability` can be converted to or
  from namespaced v3 form. These are unverified here.

Interleaving, single-threaded today: `setfacl -m u::rw-,g::r--,o::r-- f` (a minimal ACL). The
backing removes the xattr and chmods. The cache now holds `system.posix_acl_access` as present.
`getfattr -n system.posix_acl_access f` answers from the cached row (`GetXattr` serves a present
row whatever the set's completeness). **Wrong until the set is refreshed.** listxattr is correct,
because the set was left incomplete.

Fix, either of:

- (a) Phase 3 reads back: `fgetxattr` on the same fd right after the set (one extra syscall), then
  records present with the real value, or absent on ENODATA.
- (b) Phase 3 leaves the name **unknown**, and the next getxattr fetches it lazily. This needs a
  per-name unknown state; see §5.

Prefer (a) for `system.*`/`security.*` and (b) or (a) for `user.*`/`trusted.*`. Cost: one syscall
per setxattr.

### F3 [TODAY, HIGH]: Mutations that change xattrs as a side effect don't mark them unknown

- `Setattr` phase 1 marks only attrs unknown (dir_cache_fs.cc:219).
  - chmod on a file with an ACL rewrites `system.posix_acl_access` via `posix_acl_chmod`. Verified
    for ext4 (`fs/ext4/inode.c:6243`), xfs (`fs/xfs/xfs_iops.c:888`) and btrfs
    (`fs/btrfs/inode.c:5527`).
  - chown and truncate on the backing set `ATTR_KILL_PRIV` (`fs/open.c:817`; `fs/attr.c:480`,
    verified). That removes `security.capability`.
  - `security.evm`/`security.ima` are rewritten when EVM/IMA are enabled (unverified).
- A writable open (`BeginWriting`, :143) and fallback `Write` (:768) mark only attrs unknown.
  Passthrough writes on the backing file call `file_remove_privs` (`mm/filemap.c:4468`, verified),
  which removes `security.capability`.
- `ReconcileAttrs` does mark xattrs unknown when ctime changes (backing.cc:436-438), but only for
  *out-of-band* changes. dcfs's own ctime-changing ops are exempt.

Interleaving, today: a file carries `security.capability` (setcap) and its xattrs are cached. The
user runs `chown`. Backing removes the capability. dcfs still reports it via getxattr and listxattr.
**Wrong data**, and security-relevant for anything that reads caps through dcfs.

Fix, in phase 1:

- Setattr with MODE marks `system.posix_acl_access` unknown.
- Setattr with UID/GID/SIZE, or KILL_SUID/SGID, marks `security.capability` unknown.
- A writable open and a fallback Write mark `security.capability` (and `security.ima`) unknown.

With per-name unknown this is cheap. Today the only tool is ForgetXattr per name, which clears the
whole set's completeness, or MarkXattrsUnknown for all names. Simplest correct stopgap:
`MarkXattrsUnknown` in those phase 1s. It is coarse, and costs a full re-list on the next xattr
access.

### F4 [TODAY after out-of-band change; CORO generally, HIGH]: "Restore complete" from a pre-phase-1 snapshot is a lost update

CreateChild (dir_cache_fs.cc:159 snapshot, :186-188 restore), RemoveChild (:368, :395-397),
Rename (:466-469, :510-515) and Link (:596-597, :624-626) all do the same thing:

1. Read `IsDirComplete(parent)` before phase 1.
2. Phase 1 clears it.
3. After phase 2, `MarkDirComplete(parent, true)` if the snapshot said complete.

Anything else that cleared completeness in between is silently undone. That breaks invariant I1,
and a missing name is then served as **absent** (LookupOrPopulate :733-741 caches it negative
→ ENOENT).

*Today, single-threaded:*

1. CreateChild phase 1 does **not** mark the parent's attrs unknown (see F5).
2. The phase-2 `OpenNode(parent)` (:176) → `VerifyBackingIdentity` → `ReconcileAttrs(parent)` sees
   valid cached attrs. If the parent was changed out of band, it calls `ForgetNegativeDentries`,
   which clears completeness so that new names become visible (backing.cc:433-435).
3. :186-188 then sets complete again.

Any name that appeared out of band is now reported ENOENT. This undoes the detection mechanism the
README describes. Out-of-band changes are "unsupported" but explicitly detected.

*CORO:* between phase 1 and the restore, a concurrent request on sibling S (for example getattr of
S → RefreshAttrs → OpenNode ESTALE) runs `InvalidateInode(S)`. That deletes `(P, s)` and clears P's
completeness (metadata_cache.cc:809-819). The restore then makes `s` absent while the name exists
on backing. Other writers of "incomplete" that the restore can clobber: `UpsertInode`'s
recycled-number invalidation from a population elsewhere (:500-502), `PurgeFilesystem` (:874) and
4.10's `RecoverDirty` (not at runtime).

Fix:

- (short term) make it compare-and-set: `UPDATE directories SET children_complete=1 WHERE inode=? AND epoch=:epoch_after_phase1`. Every "mark incomplete" bumps `epoch`.
- (right fix) per-dentry `unknown` state. Then phase 1 sets that one name's row to `state='unknown'` and **never touches `children_complete`**. Phase 3 sets the row present or absent. There is nothing to restore, so no lost update.

Also mark the parent's attrs unknown in CreateChild's phase 1 (F5). That removes today's trigger,
because ReconcileAttrs skips invalid attrs.

### F5 [CRASH on main; fixed for crash by 4.10; CORO benign]: CreateChild phase 1 doesn't mark the parent's attrs unknown

Mkdir, mknod, symlink and create change the parent's mtime and ctime, and nlink for mkdir. Phase 1
(dir_cache_fs.cc:166) only calls `MarkUnknown(parent, {name})`. Phase 3 refreshes the parent in a
*separate* transaction at the end (:193). A crash after phase 2 and before :193 leaves the parent's
pre-create mtime and nlink marked **valid**. That violates R1, and it compares with RemoveChild,
Rename and Link, which do mark the parent (:377, :478-479, :604).

Separately, on main every commit is `synchronous=NORMAL` in WAL (sqlite.cc:410). On power loss the
phase-1 commit itself may roll back while the backing syscall persisted, so R1 holds only for
process crashes.

4.10 changes this (46d6218): phase 1 goes through `BeginMutation` with a `kSync` commit, and
`BeginCreate` puts the parent in the durable `dirty` set. `RecoverDirty` then marks it unknown after
an unclean shutdown. That closes the crash window. `BeginCreate` still doesn't mark the parent's
attrs unknown *at runtime* (per the 4.10 header diff), so F4's today-trigger remains. Recommend
adding `MarkAttrsUnknown(parent)` to `BeginCreate`.

### F6 [CRASH on main and in 4.10; CORO, MEDIUM]: `ParentOf` treats unknown as an error

`ParentOf` (metadata_cache.cc:383-403) returns NotFound when a directory has no positive dentry.
Callers map that to ENOENT: Lookup "..", dir_cache_fs.cc:249; Readdir/Readdirplus "..", :987 and
:1030. A directory's own dentry becomes unknown when:

- Rename phase 1 runs on it (it deletes both names). A crash before phase 3 leaves D dentry-less.
- `InvalidateInode` of its parent runs. Child dentries cascade away and D is orphaned until the
  *new* parent row is populated.
- 4.10's `RecoverDirty` forgets dentries pointing at dirty inodes.
- A failed op whose `ReresolveAfterFailure` itself fails.

After that, `readdir` of D, or an NFS reconnect of D (`get_parent` = LOOKUP ".."), fails with
ENOENT until something happens to populate the right parent. Nothing triggers that from D's side.

*CORO:* an NFS `get_parent` on D during D's rename phase 2 gets ENOENT. It is believed to hold no
lock on D; unverified.

Fix: when ParentOf's answer is unknown, resolve it from backing. Open D, `openat(D_fd, "..")`,
statx plus handle, look up the row by (device, ino) (an index already exists via the UNIQUE), or
populate the grandparent. Alternatively, keep an `unknown` dentry row carrying the last inode as a
*hint* (see §5), and verify the hint with the `..` statx.

### F7 [COARSE, MEDIUM]: One flag covers many records

- **Xattrs.** `ForgetXattr` (metadata_cache.cc:784-793) clears `xattrs_complete` for one name.
  Phase 3 (`SetXattr` :768, `RemoveXattr` :776) never sets it back. So *every* setxattr or
  removexattr leaves the set incomplete until the next `listxattr` pays a full `RefreshXattrs`: an
  OpenNode plus listxattr plus N getxattr. A getxattr of a *removed* name also pays it. `RemoveXattr`'s
  "remains complete" branch is dead in practice, because phase 1 always cleared it.
  Correct-but-coarse, because GetXattr still serves other names' present rows.
  With per-name state, phase 1 marks only `(id, name)` unknown, the set stays complete, and phase 3
  sets present or absent. listxattr then needs a refresh only if some name is unknown, and can
  resolve only those names individually (one getxattr each, ENODATA meaning absent).
- **Dentries.** `MarkUnknown` (:675) clears `children_complete` for one name. Phase 3 restores it
  (F4, which is where the coarseness becomes *incorrect*). On a failed op, `ReresolveAfterFailure`
  (dir_cache_fs.cc:544) → `LookupOrPopulate` finds the parent incomplete and runs a **full
  `PopulateDirectory`**. For example, `mkdir existing` (EEXIST) in a 100k-entry directory probes all
  100k children, each with openat, statx, handle, ioctl, listxattr and getxattr. With a per-name
  `unknown` row, the reresolve is one `openat(parent_fd, name, O_PATH)` + statx + probe.
- **`InvalidateInode`** (:804) marks every parent of the object incomplete to express "these names
  are unknown". With per-name state it would set those dentry rows to `unknown` and leave
  completeness alone.
- **`ReconcileAttrs` for a directory** (`ForgetNegativeDentries`) must stay coarse: an out-of-band
  change can add names nobody knows. Clearing completeness is the right tool there.
- **Per-inode `attrs_valid`** covers all 16 attribute columns as one record. That is fine: they come
  from one statx.

### F8 [latent, MEDIUM]: The schema's `ON DELETE SET NULL` turns unknown into absent

`dentries.inode ... ON DELETE SET NULL` (schema.sql:85) makes every dentry pointing at a deleted
inode row into a **negative** entry, which reads as absent. The correct meaning is unknown. Today
two places work around it by hand:

- `InvalidateInode` deletes the dentries first (metadata_cache.cc:815-819).
- `PurgeFilesystem` deletes the boundary negative afterwards (:864-873).

Deletions by cascade (filesystems → inodes) only reach the boundary dentry. I traced this and found
it correct today, but it is fragile: any new deletion path gets it silently wrong.

Fix: replace the FK action with a trigger:

```sql
CREATE TRIGGER inode_delete_unknowns BEFORE DELETE ON inodes BEGIN
  UPDATE dentries SET state='unknown', inode=NULL WHERE inode=OLD.id;
END;
```

Keep the FK as NO ACTION. With per-dentry state, the directory stays complete. This removes both
hand-written workarounds.

### F9 [TODAY, LOW; 4.10 changes it]: The unknown generation (0) is compared as a value

`ReadGeneration` returns 0 for "unknown": no ioctl support, EACCES or EPERM (backing.cc:349-371).
`VerifyBackingIdentity` treats 0 as "matches anything" (:460). But `UpsertInode` compares
`backing_gen` exactly (metadata_cache.cc:474). So a transient 0 from a probe, against a row with a
known generation, makes the row count as a *recycled* inode:

- it is invalidated (:500-502);
- a new nodeid and fuse_gen are minted;
- the old NFS handles get ESTALE;
- its parents are marked incomplete.

The reverse case (row 0, probe N) behaves the same way. 4.10 (3ec9a92) reworks identity to use
handle bytes plus birth time. I have not verified whether it treats 0 as unknown rather than as a
distinct value.

### F10 [CORO if SQLite I/O is async, MEDIUM]: Read-then-write sequences spanning transactions

- LookupOrPopulate: `Lookup` → `IsDirComplete` → `SetNegative`, three transactions (backing.cc:729-740).
- CreateChild, RemoveChild, Rename, Link: completeness snapshot → phase 1.
- Getxattr and Listxattr: refresh commit → re-read with RET_CHECK (dir_cache_fs.cc:1136-1144, 1163-1167).
- EntryFor: refresh → `RequireAttr` (:119-122).

With synchronous SQLite, cooperative coroutines don't suspend between these, so today's code is
safe. With the planned io_uring VFS every step is a suspension point. Fix: make them single
conditional statements. For example:
`INSERT INTO dentries(...,state) SELECT ?,?,'absent' WHERE (SELECT children_complete FROM directories WHERE inode=?)=1 ON CONFLICT DO NOTHING`.
Also have population functions return their fresh data.

### F11 [CORO, LOW-MEDIUM]: `OpenNode`'s reconcile snapshot is taken before the suspension

`OpenNode` reads `GetAttr` (backing.cc:481) *before* `handle.Open` (:484) and the statx (:453),
both suspension points. A mutation that completes in between produces:

- a false "out-of-band change" WARNING;
- a spurious `ForgetNegativeDentries` (a full relist) or `MarkXattrsUnknown`;
- a `RecordAttrs(fresh)` that can land mid-mutation (the F1 pattern).

Fix: re-read the row (and its epoch) after the statx, and skip the reconcile when the epoch moved or
a mutation is in flight. The same applies to `PopulateDirectory`'s reconcile at :675-686.

### F12 [CORO, LOW]: A row invalidated between resolve and phase 1 fails the op

RemoveChild resolves the child (dir_cache_fs.cc:360) and then, in phase 1, runs
`MarkAttrsUnknown(child.id)` (:378). That returns NotFound if an `InvalidateInode(child)` ran in
between; `LookupOrPopulate` can suspend, so this is possible. The whole transaction fails and
unlink returns ENOENT for an existing file. Rename (:480-482) and Link (:605) have the same shape.
Fix: in phase 1 treat NotFound on a child as "already unknown", or re-resolve.

### Non-findings (checked, OK)

- **Symlink targets** are immutable per backing identity. Fill-in races (Readlink
  dir_cache_fs.cc:298-299, PopulateDirectory :699-702) can only write the same value, so two states
  suffice.
- **File handles** are immutable per identity. The UpsertInode overwrite writes the same bytes.
- **Readers ignoring `valid`** (Readdir :1001-1004, Rename :533-534) use only immutable fields.
- **The open-for-write guard** is checked at commit time in `RecordAttrs`/`PopulateDirectory`/`RecordNewChild`, so it is race-free. That is the model F1's fix should follow. `Release` (:845-852) erases the inode from the set before its final refresh, and a concurrent writable Open re-inserts it and re-marks unknown (BeginWriting), so that is fine too.
- **Setattr, Link, Rename, Unlink, Fallocate and fallback Write** follow phase 1 unknown → phase 2 → phase 3 refresh for the attrs they name, except F5 (Create's parent) and F3 (xattr side effects).
- **Mkdir's `MarkDirComplete(true)` on the new directory** (backing.cc:774) is fine: the kernel hasn't instantiated the dentry yet, and no handle exists.
- **filesystems rows** are dormant after 4.8 (99cfd3e: submounts are refused, StartupPurge purges every non-source row). 4.8 adds a fourth, in-memory "refused boundary" state: the name is left out of a *complete* listing, and LookupOrPopulate answers it with EXDEV. After a restart the in-memory set is empty and the complete listing reads as **absent** (ENOENT, not EXDEV). Startup refuses to run with submounts present, so this is only reachable when a mount appears at runtime. Unverified beyond the commit message.

### The owner's suspected instance (Setxattr), specifically

- `ForgetXattr` in phase 1 is *R1- and R2-correct*. Unknown is encoded as "row absent + set
  incomplete", and GetXattr correctly returns nullopt for it.
- The coarseness is real: it forces a whole-set refresh on the next listxattr (F7).
- The *bugs* in that flow are elsewhere:
  - phase 3 records the caller's value (F2);
  - chmod, chown and writes don't mark ACL/capability names unknown at all (F3);
  - a concurrent getxattr or listxattr refresh can commit a pre-syscall snapshot after phase 3 (F1,
    Interleaving B). The kernel serializes setxattr against setattr and setxattr, not against
    getxattr or listxattr.
- Converting xattrs to explicit tri-state fixes the coarseness and makes F2 and F3 cheap to fix. It
  does **not** fix F1 without the epoch check.

---

## 4. Crash windows per op (main)

"OK" means that at every point, each affected record is unknown or correct.

| Op | Windows | Status |
|---|---|---|
| Setattr | 1 → 2 → 3 | OK (process crash). Power loss is not OK on main: phase 1 isn't fsync'd (sqlite.cc:410). Fixed by 4.10. |
| Setxattr / Removexattr | 1 → 2 → 3a (xattr) → 3b (attrs) | OK |
| Create/Mkdir/Mknod/Symlink | 1 → 2 → RecordNewChild → restore → parent refresh | **Parent attrs stale-valid after phase 2** (F5). 4.10's recovery fixes the crash case. |
| Unlink/Rmdir | 1 → 2 → 3 → parent refresh → child settle | OK. The child row lingers with unknown attrs and is invalidated at next open (ESTALE). |
| Rename | 1 → 2 → 3 → refreshes | Records are OK, but a **moved directory has no dentry → F6 ENOENT on ".."** |
| Link | 1 → 2 → 3 → restore → refresh | OK |
| Writable open / Write / Fallocate | attrs unknown for the duration | OK for attrs. **Xattrs not marked (F3)** |
| Populations (all) | single transaction after I/O | OK for crashes (they only write what they read). The problem is concurrency (F1). |

## 5. Proposed design: explicit states for every record type

Principle: **name-keyed records** (dentries, xattrs) get real three-state rows. **Object-keyed
records** (attrs, symlink target, handle) keep present/unknown, and "absent" is "no row", because
the object's absence is expressed by its dentries. **Aggregate flags** (`children_complete`,
`xattrs_complete`) keep one meaning only: *every name without a row is absent*. No single-name
operation ever clears them. Every record class gets an epoch for optimistic population commits.

### Schema (v3 relative to 4.10's v2)

```sql
CREATE TABLE inodes (
  ...,
  attrs_state TEXT NOT NULL DEFAULT 'unknown'
      CHECK (attrs_state IN ('present','unknown')),   -- replaces attrs_valid
  attrs_epoch INTEGER NOT NULL DEFAULT 0,
  xattrs_complete INTEGER NOT NULL DEFAULT 0,          -- = "names without a row are absent"
  xattrs_epoch INTEGER NOT NULL DEFAULT 0,
  ...
) STRICT;

CREATE TABLE dentries (
  parent INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('present','absent','unknown')),
  inode INTEGER NULL REFERENCES inodes (id),   -- NO ACTION; see trigger
  CHECK ((state = 'present') = (inode IS NOT NULL)),
  PRIMARY KEY (parent, name)
) STRICT;
-- Optionally keep a hint for ParentOf/F6: hint_inode INTEGER NULL, set on present->unknown.

CREATE TABLE directories (
  inode INTEGER PRIMARY KEY REFERENCES inodes (id) ON DELETE CASCADE,
  children_complete INTEGER NOT NULL DEFAULT 0,  -- "names without a row are absent"
  epoch INTEGER NOT NULL DEFAULT 0               -- bumped by every dentry/completeness write
) STRICT;

CREATE TABLE xattrs (
  inode INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('present','absent','unknown')),
  value BLOB NULL,
  CHECK ((state = 'present') = (value IS NOT NULL)),
  PRIMARY KEY (inode, name)
) STRICT;

CREATE TRIGGER inode_delete_unknowns BEFORE DELETE ON inodes BEGIN
  UPDATE dentries SET state = 'unknown', inode = NULL WHERE inode = OLD.id;
END;
```

`TEXT` states cost a few bytes a row. `INTEGER` 0/1/2 with a CHECK is equally fine and smaller.
The in-flight counters stay in memory (`Context`), keyed by (inode, class) and (dir).

### API

- **Reads.** Return `enum class State { kPresent, kAbsent, kUnknown }` everywhere.
  - `Lookup` gains kUnknown for an explicit unknown row, and for a missing row only when the dir is
    incomplete. A missing row in a complete dir → kAbsent, decided in SQL.
  - `GetXattr(id, name)` → {state, value}.
  - `ListXattrs` → names plus the list of unknown names (resolve only those).
  - `ParentOf` → {kPresent id | kUnknown hint}.
  - `GetAttr` → {state, epoch, values}.
- **Phase 1:**
  - `BeginDentryChange(parent, name)` sets the row to `unknown` and bumps `directories.epoch`.
  - `BeginXattrChange(id, name)` sets the row to `unknown` and bumps `xattrs_epoch`.
  - `BeginAttrChange(id)`.
  - All increment in-flight counters, and fold into 4.10's `BeginMutation` (same transaction as the dirty insert).
- **Phase 3:** `SetDentry(parent, name, present|absent)`, `SetXattrState(id, name, present(value)|absent)`, `RecordAttrs(id, stx)`. Each bumps its epoch and decrements in-flight. No completeness restore.
- **Populations:** `Snapshot{epochs}` taken before I/O. `Commit*(…, snapshot)` is a conditional UPDATE and returns whether it applied. Callers reply from their own data. A PopulateDirectory commit applies per child and per directory.
- **Single-name resolvers:** `ResolveName(parent, name)` does openat O_PATH + probe of one child, and replaces full-populate fallbacks in `ReresolveAfterFailure` and in LookupOrPopulate when only one name is unknown in a complete dir. `ResolveXattr(id, name)` does one getxattr.
- **Readdir of a complete dir with unknown rows:** resolve those rows first (they are few), or fall back to populate.

### Migration sketch (v2 → v3, in Migrate(), one transaction; rows keep ids so NFS handles survive)

1. `ALTER TABLE inodes ADD COLUMN attrs_state ... DEFAULT 'unknown'`, then `UPDATE inodes SET attrs_state = CASE attrs_valid WHEN 1 THEN 'present' ELSE 'unknown' END`. Keep `attrs_valid` unused, or rebuild the table.
2. Add `attrs_epoch`, `xattrs_epoch` and `directories.epoch` (DEFAULT 0).
3. Rebuild `dentries` and `xattrs`. Changing FK actions and table CHECKs requires the 12-step SQLite table rebuild: create `*_new`, copy, drop, rename.
   - dentries: `state = CASE WHEN inode IS NULL THEN 'absent' ELSE 'present' END`.
   - xattrs: all `'present'`.
4. Create the trigger. Bump the schema version.
5. Simplest safe option if a migration is ever in doubt: set everything unknown (`attrs_state='unknown'`, `children_complete=0`, `xattrs_complete=0`) and keep the rows. That gives a cold but correct cache with NFS handles intact. This is the same move as 4.10's `RecoverDirty`, applied globally.

4.10 interplay: `RecoverDirty` would set dirty inodes' dentries and xattr rows to `unknown`. It must
still clear `children_complete` for dirty *directories*, because a lost backing mutation can add
unknown names. The durable `dirty` table and the in-memory in-flight counters are complementary:
dirty covers crashes, in-flight covers concurrency.

## 6. Prioritized list

1. **F3 + F2 (TODAY, small).**
   - Setattr (MODE / UID / GID / SIZE / KILL_*) and writable open / fallback Write mark
     `system.posix_acl_access` / `security.capability` (or, as a stopgap, the whole xattr set)
     unknown in phase 1.
   - Setxattr phase 3 reads back with `fgetxattr` (ENODATA → absent) instead of caching the caller's
     value.
   - Tests: a minimal `setfacl`; `setcap` then `chown`; `setcap` then write.
2. **F4 + F5 (TODAY with out-of-band detection).** Add `MarkAttrsUnknown(parent)` to Create-family
   phase 1 (in 4.10: `BeginCreate`), and make the completeness restore a compare-and-set on a
   directory epoch. Small.
3. **F1 (prerequisite for coroutines).** Epochs plus in-flight counters, and conditional commits for
   RefreshAttrs, RefreshXattrs, PopulateDirectory, RecordNewLink, ReconcileAttrs and
   LookupOrPopulate's SetNegative. Remove the post-refresh RET_CHECKs; reply from the fetched data.
   Medium: touches every population writer.
4. **§5 per-name tri-state for dentries and xattrs, plus the F8 trigger.** This removes F7's
   coarseness and F4's restore pattern entirely, and makes F2/F3's fixes per-name. Medium-large:
   schema v3 and the API rewrite of MarkUnknown, InvalidateInode, ForgetXattr, RemoveXattr and
   PruneDentriesNotIn.
5. **F6.** Resolve ParentOf's unknown from backing (`openat(dir, "..")`). Small to medium.
6. **F11 / F10 / F12.** Re-read after suspension; single-statement conditional writes;
   NotFound-tolerant phase 1. These matter once coroutines, or an async SQLite VFS, land.
7. **F9.** Treat generation 0 as unknown in UpsertInode's identity match. Check first whether 4.10's
   handle-bytes identity already covers it.

Unverified claims in this report:

- that getattr, getxattr and listxattr take no inode lock;
- that NFS `get_parent` holds no lock on the child directory;
- LSM/EVM/IMA xattr rewriting;
- 4.10's exact phase-1 contents beyond `metadata_cache.h`'s header diff;
- 4.8's refused-boundary behavior beyond its commit message.
