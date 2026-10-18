# Contract audit (2026-10-17), main checkout, read-only

Kernel tree: ~/Sources/linux (v7.3-rc4-607). Man pages: this machine.
Coverage caveat: grep produced ~200 comment hits in dcfs/*.cc/h plus the docs; I read and checked the contract-bearing ones (about 45 claims). Claims I did not reach are NOT listed as verified. formal/limitations/ and formal/no_stronger/ were not read beyond formal/README.md's tables.

Counts: FALSE 1 (with 2 derived lines), STRONGER 2, UNSOURCED 7, behaviour flags 8, VERIFIED 22.

## FALSE

F1. docs/style.md:1307-1309 "`rename`, `unlink`, `readdir` promise atomicity (a listing is a consistent snapshot, a rename either happened or did not)"; derived: style.md:1317-1319 "getdents returns listings in batches, and the listing across calls stays one snapshot".
- rename: true (man 2 rename: "atomically replaced, so that there is no point at which another process attempting to access newpath will find it missing"). unlink: no promise worded, but a single dirent removal; fine.
- readdir/getdents: FALSE. POSIX readdir(3p): "If a file is removed from or added to the directory after the most recent call to opendir() or rewinddir(), whether a subsequent call to readdir() returns an entry for that file is unspecified." (verbatim from memory of POSIX.1-2017; the local man 3 readdir does not carry the sentence, so quote POSIX, not the man page.) Kernel: fs/readdir.c iterate_dir() takes inode->i_rwsem shared only for the duration of ONE getdents call (lines 103-115) and the position is a cursor (f_pos), so mutations between calls are visible and nothing snapshots. fs/fuse/dir.c:2450 uses .iterate_shared too.
- Consequence: the whole-directory snapshot design (design.md:944-960, "One spin-up per directory"; ListCached comment dir_cache_fs.cc:2266-2276 "serve this snapshot") is justified by this claim and by nothing else I found.

## STRONGER

S1. docs/style.md:1317-1319 / design.md:969-1000: dcfs promises a listing "stays one snapshot" across getdents calls (offsets are dentry rowids of a recorded complete listing). The backing promises nothing (F1). Native ext4/xfs/btrfs readdir shows concurrent changes. Source: F1.
S2. formal/README.md:641-643: "an fsync ... gives the caller at least what the backing filesystem's own fsync gives, and more where the backing's fsync is weak, since dcfs follows it with syncfs". Openly documented, deliberate, costs time; it is more than the backing gives (man 2 fsync gives file durability only; dir entry durability needs fsync of the dir). Not a false claim, but a dcfs-chosen stronger contract; confirm russ wants it under "behave like the backing".

## UNSOURCED (test: native differential, dcfs out of the loop)

U1. README.md:1473 and ~1470: "readdir of a chattr +F directory dereferences NULL (Linux 6.18 to 7.3-rc)". Test: the existing guest check casefold-tune-online-oops run natively (it is stated to need no dcfs); I did not run it. I did not find the NULL deref in ~/Sources/linux.
U2. design.md:104-106 "inode numbers are unique within one filesystem" and "file handles encode enough to reject a stale handle (ext4, xfs, btrfs encode the generation)". Test: native stat/name_to_handle_at on btrfs with subvolumes (inode numbers repeat across subvolumes; st_dev differs) and on ext4 after inode reuse. I did not check fs/ext4/xfs/btrfs fh encoders.
U3. design.md:2244-2270, README ~1160 (setuid/setgid and chown(-1,-1)): "the native filesystem refuses chown(path,-1,-1) by a non-owner with EPERM" and the kernel-sends-mode statements. Test: native chown(-1,-1) by non-owner on ext4/xfs/btrfs; native write by non-owner to a setgid non-group-exec file. (fs/attr.c setattr_should_drop_suidgid not read by me.)
U4. dir_cache_fs.cc:2391/backing.cc:2391 "WriteClearsSetgid ... the kernel's own rule clears setgid with a mode instead (with group execute)". Same tests as U3.
U5. backing.cc:218-224 "verified experimentally ... utimensat on /proc/self/fd/<fd> sets a symlink's own timestamp"; backing.cc:211-216 fchmodat via /proc path "surfaces EOPNOTSUPP". Experimental claims, no test cited; native test: same calls on an O_PATH fd to a symlink.
U6. design.md:2328-2341, backing.h:259-261: ext4/xfs/btrfs store system.posix_acl_access as xattr vs. tmpfs "only updates the mode (posix_acl_update_mode)". Test: native setxattr/chmod on each fs and listxattr.
U7. README.md:1232-1240 and design.md:1525-1545 (frozen backing: stat, cached readdir, lookups, opens still answered; only writes block). Measured by fault_freeze_test per the text; I did not rerun it. Native check: FIFREEZE then the same calls without dcfs.

## Unbounded working memory (requested addition)

Bounds from the kernel: NAME_MAX 255 (a name), PATH_MAX 4096 (symlink target; code allows 4*PATH_MAX, backing.cc:105-122), XATTR_NAME_MAX 255, XATTR_SIZE_MAX 65536 (one value), XATTR_LIST_MAX 65536 (one names list). I did not verify these constants in ~/Sources/linux include/uapi/linux/limits.h this pass; they are the documented values (man 7 xattr, man 7 path_resolution). No kernel limit bounds the NUMBER of entries in a directory, xattrs per inode count beyond the 64 KiB list, inodes or dirty rows.

M1. backing.cc:456-472 ReadDirNames: vector<string> of every name. Grows with directory size N; each name <= NAME_MAX; no count bound. Could stream: getdents64 already returns 32 KiB batches (kDirentBufferBytes), so the vector exists only to feed PopulateDirectory.
M2. backing.cc:1424-1470 PopulateDirectory: `children` (vector<ChildRecord>: statx, handle, symlink target <= PATH_MAX, ALL xattrs: list <= 64 KiB, values <= 64 KiB each, so up to ~64 KiB * several per child), `refused_names`, then backing.cc:1495-1510 `seen` (a third copy of every name) and `result.entries` (flat_hash_map keyed by name, a fourth), all live across the single transaction. Grows with N times the per-child record (worst case about 64-130 KiB of xattr data per child). Phase A (all I/O) before phase B (one transaction) is the design (design.md:947-960), so streaming needs the cursor/page redesign: a page of getdents -> probe -> upsert, completeness marked on the last page. Unbounded by any kernel limit.
M3. backing.cc:1495-1534 plus metadata_cache.cc:885-900 PruneDentriesNotIn: flat_hash_set of all N names (string_views over `seen`) plus `doomed` vector of cached names not found. Grows with N and with the cached dentry count. Could stream with a per-page cursor delete, or by a generation/epoch column (mark dentries seen, delete the unmarked) instead of an in-memory set.
M4. dir_cache_fs.cc:2249-2312 ListCached/Readdir/Readdirplus (2318-2395): bounded by the reply `budget` (kernel's request size <= max_read/max_pages) and ListDir batches (metadata_cache.cc:340-360, limit from budget). Bounded; already streams with a cursor. Not a finding; listed for contrast.
M5. dir_cache_fs.cc:2571-2594 and metadata_cache.cc:430-445 xattr list: vector of names; bounded by XATTR_LIST_MAX 64 KiB on the backing (names <=255 each, at most ~32768 names of 1 byte plus NUL... bounded by the 64 KiB list). Bounded by kernel limit. backing.cc:140-155 (SplitXattrList) and 435-447 (ReadXattrs: names plus all values) same bound for names, but values total up to count*64 KiB per inode (the per-inode xattr total is bounded only by the filesystem, e.g. ext4 one block, xfs/btrfs larger). Could stream: fetch value lazily per request instead of caching every xattr value at population (design choice).
M6. metadata_cache.cc:1490-1500 ListDirty, called at backing.cc:2282 (StartRun) and BeginSync metadata_cache.cc:1503-1512 (`snapshot.dirty`, `atime_only` vectors; each sync point) plus ClearDirty sets (1552-1601: `kept`, `kept_open`, `put_back`). Grow with the dirty-set size = inodes written/changed since the last sync point (and, after a crash, since the last sync point before it). No kernel bound; the interval and workload bound it. Could stream by cursor over the dirty table (rowid ranges) in batches, the way DESTROY's reconcile already batches 4096 (dir_cache_fs.cc:354-362).
M7. dir_cache_fs.h:677-724 per-inode in-memory state: lookups_ (every nodeid the kernel holds), removed_ (each with an fd), written_ (each may hold an O_PATH fd, capped by max_held_fds_), backing_files_/open_files_/open_files_held_/tmpfiles_/stubs_logged_, plus dir_cache_fs.cc:354-358 DESTROY copies written_ into a vector, and 1040-1056 ForgetMulti vector (bounded by the kernel's batch). lookups_ is bounded by the kernel's inode cache (comment dir_cache_fs.h:715), not by a stated limit; stubs_logged_ is unbounded over a run (one id per stub ever refused). Cannot stream: it is the mirror of kernel state.
M8. context.h:70-83 FillGuards.touched is pruned at max_touched (65536); inflight bounded by requests in flight; durable (context.h:48-53) is the in-memory mirror of the dirty set (as M6). The pruning is already a bound.
M9. metadata_cache.cc:1140-1157 RegisterMutation: `out` is deduplicated by a linear scan, O(k^2) in ids per mutation (time, not memory); DESTROY/ForgetMulti pass up to 4096 ids. Flagged for time only.
M10. fsck.cc:135-145, 210-222: bounded (rows capped at `most`, `missing` capped at 3). Not a finding.
M11. backing.cc:105-122 ReadLinkAt: doubles to PATH_MAX*4 = 16 KiB; bounded.
M12. metadata_cache.cc:730-745 `stale` vector (rows claiming the root's inode number) and 620-635 `rows.stale`: bounded by rows sharing one backing inode number; effectively small, no kernel bound.
Also: one SQLite transaction holds all of M2's writes at once (backing.cc:1483-1535); SQLite bounds this only by disk.

## Behaviour-shape flags (dcfs decides where the backing would not)

B1. dcfs/dir_cache_fs.cc:2249-2312 ListCached: bounded retry loop (kAttempts = 3: populate, re-check completeness) ending in ProducedErrnoToStatus(EAGAIN, "Directory ... kept changing"). Native readdir never returns EAGAIN (man 2 getdents ERRORS: EBADF, EFAULT, EINVAL, ENOENT, ENOTDIR). That is a retry loop (style 1.11) AND an invented errno (style 1.12 item 5). The comment cites a TODO(coroutines).
B2. dcfs/backing.h:318-325 LookupOrPopulate returns EAGAIN "if its stub could not be recorded (a concurrent mutation of parent kept the listing or probe from recording anything)". Same: lookup(2)-family never gives EAGAIN here.
B3. backing.cc:1424-1470 PopulateDirectory (via LookupOrPopulate for an incomplete directory): a single-name lookup in a not-yet-complete directory runs getdents64 and then openat+statx+name_to_handle_at+ioctl(GETVERSION)+readlink+every xattr for EVERY child before replying (design.md:944-960). Native lookup of one name is one dcache/dir probe. This is "loops of backing work answered all at once": the first stat in a million-entry directory waits for a million probes. Interruption checks only every 16 children (backing.cc:1443-1449) and an interrupted population records nothing.
B4. backing.cc:1459-1534: population is "all I/O, then one transaction" with `seen`/`result.entries` copies (see M2): the directory is read as one snapshot, in the sense of F1.
B5. design.md:1517-1520 and README.md:1232-1240: one request at a time on one thread; a request blocked in a backing syscall (frozen fs, hung NFS, slow disk) blocks every other request including cache hits. Native: only the blocked caller waits.
B6. dir_cache_fs.cc:253-262 and 1698-1705: FUSE_CAP_ATOMIC_O_TRUNC is turned off, so open(O_TRUNC) becomes OPEN then SETATTR(size=0): two backing steps instead of one; a crash or error between them leaves a non-truncated file the caller was told was being truncated. UNSOURCED against the backing (test: native open O_TRUNC vs. interrupted/failed truncate).
B7. design.md:2278-2287 / README.md ~1160: documented deviation, chown(path,-1,-1) by a non-owner on a setgid non-group-exec file with a writable open held by anyone succeeds and clears the bit where the native filesystem returns EPERM. Documented, not hidden; listed because it is dcfs deciding by its own rule.
B8. dir_cache_fs.cc:1325-1329 Rename with same_inode: dcfs handles two links to one inode "should one arrive", a branch for an input the comment says the kernel never sends (style 1.10a: name a reachable cause or RET_CHECK).

## VERIFIED (one line each)

V1. design.md:1519-1520 / formal/README.md:810: "dcfs does not request FUSE_CAP_PARALLEL_DIROPS, so the kernel serializes lookups and readdirs per directory": fs/fuse/inode.c:591-598 fuse_lock_inode takes inode->mutex when !fc->parallel_dirops; used at fs/fuse/dir.c:632 (lookup) and fs/fuse/readdir.c:399-401 (readdir request). (It covers the request only, not the whole listing.)
V2. style.md:833 "the kernel never sends a request whose uid or gid is unmapped ... fails the caller with EOVERFLOW": fs/fuse/req.c:46-47 (fuse_fill_creds). The citation "fs/fuse/dev.c" is stale; the code now lives in fs/fuse/req.c.
V3. README.md:1162, design.md:2476-2480 "kernel waits for FUSE_DESTROY only for fuseblk and virtiofs": fs/fuse/inode.c:1945 (ctx->destroy = true under fuseblk), virtio_fs.c:1558, fuse_conn_destroy inode.c:1973.
V4. umount_helper.h:30-40 "fuse_ctl_remove_conn runs inside fuse_conn_destroy": inode.c:1979-1984.
V5. dir_cache_fs.cc:1326 "the kernel's vfs_rename() treats two links to one inode as a successful no-op": fs/namei.c:6092 `if (source == target) return 0;`.
V6. dir_cache_fs.cc:1691 "do_file_open_root retries a failed open once with LOOKUP_REVAL after ESTALE": fs/namei.c:5056-5057.
V7. device_id.cc:117 "ioctl(2) unconditionally refuses O_PATH descriptors with EBADF": fs/ioctl.c:583-589 CLASS(fd) -> __fdget with FMODE_PATH mask (fs/file.c:1208).
V8. fuse_request.cc:160 "fuse_passthrough_open() never returns negative": ~/Sources/libfuse lib/fuse_lowlevel.c:566-578 (returns 0 on failure).
V9. syscalls.h:30-35 setfsuid reports no error, read back with setfsuid(-1): man 2 setfsuid BUGS.
V10. syscalls.h:48-52 raw setgroups is per-thread, glibc broadcasts: man 2 setgroups ("kernel level ... per-thread; POSIX requires ... NPTL handles").
V11. syscalls.h:43-45 umask is process-wide: man 2 umask.
V12. style.md:1303-1304 rename atomic: man 2 rename (above).
V13. style.md:1304-1306 listxattr/getxattr promise nothing across a concurrent writer and answer ERANGE: man 2 listxattr / getxattr (ERANGE when the buffer is too small, size can change between calls). Read from memory of the pages, not re-quoted this pass.
V14. design.md:2282 "under default_permissions the kernel never sends ACCESS": fs/fuse/dir.c:1800-1818 (fuse_access only in the else branch of default_permissions).
V15. dir_cache_fs.cc:1698 "the kernel calls opendir(), not open(), on a directory": fs/fuse/dir.c:2451 (.open = fuse_dir_open for directories).
V16. design.md:989 "the kernel's attr_version discards stale READDIRPLUS attributes": fs/fuse/readdir.c:393, 232.
V17. dir_cache_fs.cc:2286 "the kernel will call again from the cursor this entry's offset encodes": fs/readdir.c iterate_dir uses file->f_pos (87-118).
V18. backing.cc:260-262, 248-252: capabilities lost when fsuid != 0 (cap_task_fix_setuid): standard capabilities(7) text on setfsuid; not re-read in security/commoncap.c this pass, so treat as plausible-from-man 7 capabilities only.
V19. formal/README.md:643-644 "an atomic replace or create by rename without fsync before is as atomic as the backing makes it, no more": a statement about dcfs, supported by the litmus configs in the model, not by a kernel source.
V20. file_handle.h:20-23 STATX_MNT_ID_UNIQUE "promises not to reuse the value": man 2 statx (STATX_MNT_ID_UNIQUE, "unique ... not reused").
V21. docs/design.md:2288-2300 POSIX ACLs enforced by the kernel when FUSE_CAP_POSIX_ACL is set: libfuse/kernel capability; the kernel tree has fs/fuse/acl.c; not read in detail.
V22. README.md:129 plain umount does not wait for the daemon: same as V3.
