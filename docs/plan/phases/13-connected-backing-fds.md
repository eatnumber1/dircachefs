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

## Status 2026-10-09: built, one conflict needs russ

Branch step-13.1 in lane-2 (four commits: 13.1 tests, 13.2 code and
goldens, 13.2 ident.tla, 13.3 design.md). As built: `cache::DentryOf`
(one copy, `NamesToAskAbout` uses it); `OpenNode` tries `OpenByName`
first (skips directories; the source root as parent uses the mount fd, so
a root-level file costs no extra syscall; any other parent opens by
handle, so the cost does not grow with depth: 6 announced calls instead of
3 for a file below the root, 3 extra SQLite steps); ENOENT or an identity
mismatch logs the out-of-band WARNING, marks the name unknown and falls
back to the handle open; other openat errors are returned; no checkpoint
before the named open (it runs inside phase 3); no parent-fd cache (the
measurement does not call for one). ident.tla gains a present-dentry
field, `NamedHit`, `NamedMismatch`, and known_bug
`ident_named_open_no_fallback`; a finding while modelling: without durably
clearing the dentry in phase 1 and in recovery, `ident_power` reaches a
recycled inode through a rolled-back dentry (the code already clears it).
Goldens and budgets updated with before/after; presubmit over dcfs, qemu,
tools, formal: 337 pass, 2 fail, both `fault_freeze_test`.

CONFLICT (needs russ): a by-name open with write access takes freeze
protection (`mnt_want_write` in `path_openat`); `open_by_handle_at` does
not. dcfs opens files O_RDWR even for a user's read-only open, so on a
frozen backing every named open of a file now blocks the daemon, and six
freeze checks fail (read through passthrough, read content, open for
read, open for write, stat and readdir while a write is held). Options:
(a) accept the blocking and change the test and the README freeze table
(rejected by the quality rule unless russ says so); (b) named opens only
for opens without write intent, writable fds stay disconnected by handle
(half the feature: writable fds show `/` to lsof, path LSMs and audit);
(c) a user's read-only open gets a read-only connected fd (never blocks
under freeze) and only a writable open takes the writable fd, by name
(blocks under freeze, as a write on the backing itself would) or by
handle; an investigation of what (c) needs is running. Note that
"open for write answered during a freeze" works today only because
open_by_handle_at bypasses freeze protection, a kernel quirk; russ's
"mirror a real filesystem" rule suggests a writable open should block as
the backing's would, while the daemon-wide stall it causes is the 11.6
finding, fixed by the concurrency design, not here.


Investigation of (c) (2026-10-09): one `BackingFile` per inode (one fd, one
passthrough `backing_id`; the kernel allows one backing_id per inode, EBUSY
for a second, ETXTBSY for mixing cached and passthrough opens) is opened
O_RDWR on purpose so later writers share it. Under (c) a writer after a
reader would need a disconnected handle-opened write fd and a direct-IO
reply with no passthrough, and writer-first fds stay disconnected: a poor
trade. (b) alone leaves every long-lived fd disconnected (only the
transient O_PATH opens become connected): almost none of the phase's value.

## 13.4 Experiment (2026-10-09): connected AND freeze-safe

Both candidates keep the writable fd on the handle path (`do_handle_open`
takes no `mnt_want_write`, so no freeze blocking) and get a connected
dentry another way: (e) a named `openat(parent, name, O_PATH)` first,
which reconnects the dcache dentry, then the handle open: does
`d_obtain_alias` then return the connected alias, also when a disconnected
alias from an earlier handle fd is still alive? (f) `AT_HANDLE_CONNECTABLE`
(Linux 6.13, the floor FS_IOC_GETFSUUID already imposes): a connectable
handle taken from (parent fd, name), decoded through its parent to a
connected dentry, handle-only (russ's preference), identity unaffected;
questions: ext4/xfs/btrfs support, hard links, renamed parents, gone or
replaced names, cost, stored next to the identity handle or computed per
open. Running in lane-2 on a throwaway branch off step-13.1; a table per
candidate and filesystem (connected, blocks under freeze, hard link, gone
name) decides. "No AT_HANDLE_CONNECTABLE" in the design above dates from
when the kernel floor was lower; revisit with the result.
