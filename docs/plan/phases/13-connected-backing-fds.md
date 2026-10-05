# Phase 13 — Connected backing fds

**Problem.** `backing::OpenNode` opens objects with `open_by_handle_at`. For a
non-directory that is not in the backing dentry cache (after eviction or a
restart) the kernel returns a *disconnected* dentry: its path reads `/`.
Effects on the BACKING side only (the dcfs mount is unaffected):
- fsnotify delivers child events to the parent through `dget_parent()`
  (`__fsnotify_parent`, fs/notify/fsnotify.c); a disconnected dentry is its own
  parent, so inotify/fanotify watches on a backing directory miss changes made
  through dcfs, including kernel passthrough writes through dcfs's backing fd;
- fanotify event fds, audit records and path-based LSMs see `/`.
FID-reporting filesystem marks are unaffected.

**Design (no AT_HANDLE_CONNECTABLE, no path strings).**
- In `OpenNode`, choose by cache state, not by retry:
  - the object has a cached **present** dentry whose name is not mid-mutation:
    open the parent with `OpenNode(parent, O_PATH | O_DIRECTORY)` (directories
    always decode connected), then `openat(parent_fd, name, flags | O_NOFOLLOW)`
    as root, then the existing `VerifyBackingIdentity` statx (no extra
    syscall). Hard links: any present dentry of the inode.
  - otherwise (removed-but-referenced objects, unknown names after recovery or
    invalidation, objects reached only by handle, the root): handle open as
    today.
- Identity mismatch after a named open can only be out-of-band (exclusive
  access): log the out-of-band WARNING (the detection added in the original plan's
  step 4.6, history.md), mark that name unknown, then open by handle.
- The per-inode `BackingFile` used for kernel passthrough comes from `OpenNode`,
  so its kernel-visible path is the real one too.
- Cost: one extra `openat` of a name in an already-open directory per
  `OpenNode` of a named object. Measure; if it shows up, cache parent
  directory fds in a small bounded map (owned by DirCacheFS, no globals).

**13.1 Tests first (must fail on today's main; quote the output).**
Owner decision (2026-10-04): backing-side watchers are NOT a supported use
case (exclusive access). Connected fds are added because they are cheap and
help diagnostics (lsof, /proc/<pid>/fd, paths in log lines), path-based LSM
confinement of dcfs (e.g. AppArmor "/src/**" rules) and audit trails. Test the
property, not watchers:
- Unit (`backing_test`, guest, root): populate a tree, `echo 2 >
  /proc/sys/vm/drop_caches`, `OpenNode(file, O_RDONLY)` and `OpenNode(file,
  O_PATH)`: `readlink("/proc/self/fd/N")` must be the real path, not `/`;
  also for a hard-linked file and a file two directories deep.
- Guest (`qemu_test_matrix`): after restarting dcfs and dropping caches, open
  a file through the mount for writing (passthrough) and check that every
  link in `/proc/<dcfs pid>/fd` pointing at the backing filesystem shows its
  real path (no bare `/`).
- Unit: out-of-band mismatch after a named open: one WARNING, name marked
  unknown, handle open still reaches the right object (`ScopedMockLog`).
- Unit: removed-but-referenced objects, unknown dentries and the root still
  open by handle (control).
- Guard: existing zero-sector metadata checks and passthrough-speed check
  unchanged.

**13.2 Implement** in `dcfs/backing.cc` (`OpenNode`, a `OpenByName` helper) and
`cache::DentryOf(ctx, id)` (present dentry lookup by inode; the
`dentries(inode)` index exists). Owner: one agent, Sonnet; Opus not needed.

**13.3 Docs:** design.md "fd/handle-only rule" section (why fds are connected);
optionally print backing paths in warning log lines (diagnostics only).

**Done when:** all 13.1 tests fail before and pass after, on ext4/xfs/btrfs;
full suite and pjdfstest unchanged.
