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

13.4 result (2026-10-09, guest kernel 6.18.55, ext4/xfs/btrfs): both
candidates give a connected fd and neither blocks on a frozen filesystem
(the by-name O_RDWR control blocks on ext4 and xfs; btrfs answers it). (e)
works because `__d_obtain_alias` returns the first alias and a name lookup
puts a connected one at the head, so it depends on alias-list order and
on the lookup happening first: fragile. (f) decodes deterministically
through the encoded parent (`FILEID_IS_CONNECTABLE`, `reconnect_path`),
survives a parent rename, gives some valid path for a hard link or a moved
file, ESTALE for a freed or replaced object, costs one extra syscall
(name_to_handle_at) over the handle open and 16-32 byte handles computed
per open, never stored; identity stays on the plain handle. DECISION
(orchestrator, 2026-10-09, under russ's handles-only preference; russ may
veto): (f). The design's "no AT_HANDLE_CONNECTABLE" is withdrawn: the
floor is 6.13 either way. Being implemented on step-13.1; the experiment
becomes a permanent kernel-behaviour guard test (connected decode, no
freeze blocking, by-name O_RDWR blocks on ext4/xfs).

Correction (2026-10-09): the floors do NOT coincide. README's kernel
floor is 6.9 (FS_IOC_GETFSUUID); AT_HANDLE_CONNECTABLE needs 6.13. On 6.9
to 6.12 `name_to_handle_at` returns EINVAL and dcfs falls back to the
plain handle (disconnected fds, `/` paths), one INFO line per run.

Built 2026-10-09 (step-13.1 rebuilt as four commits: 64a479e tests,
9d9b9dd backing, 3afdb49 formal, d0debf7 docs; Opus review running). As
built: `FileHandle::ConnectableFromDirEntry(dirfd, name, device)`;
`OpenByName` computes the connectable handle per open and opens it through
the one existing handle-open abstraction, identity statx after; parent =
mount fd for the root, else `OpenNode` (directories by handle, cost
constant with depth); fallbacks all to the plain handle (parent unopenable:
silent; ENOENT or identity mismatch: out-of-band WARNING + name unknown;
EOPNOTSUPP/EINVAL: one INFO via LOG_FIRST_N; any connectable-open error:
plain handle reports the real error). `connectable_handles_test` (medium,
ext4/xfs/btrfs) guards the kernel behaviour relied on. fault_freeze_test
unchanged, green on all three. Goldens: `name_to_handle_at` +
`open_by_handle_at` per named open; backing budgets +1 (create 21->22,
chmod 19->22, ...). Presubmit over dcfs/qemu/tools/formal 341 pass (two
load timeouts passed on rerun); asan on backing and dir_cache_fs tests
green.

Opus review 2026-10-09: ANOTHER ROUND, and the cost-benefit now needs
russ. H1 (HIGH): the connectable handle is derived from the current
name, so the kernel's generation check never applies to the row's object
and the only identity check left is the statx probe; for symlinks, FIFOs,
sockets and devices `ReadGeneration` returns 0, and without a birth time
the probe rests on the inode number alone. TLC-verified: `MC_ident_power`
(Evidence {"handle_gen"}) with OutOfBand = TRUE passes on main (721,557
states) and violates `HeldResolvesToItsObject` in 4 states on the branch
(an out-of-band `rm s; ln -s x s` reusing the inode). No existing config
combined generation-only evidence with out-of-band changes. Fix: keep the
connectable fd only when a generation or birth time was actually
compared, else the plain handle, plus the config and a known_bug. M1: the
guard test decides "blocked" with a timer (30 x 100 ms) and its control
depends on the dentry cache (`mnt_want_write` is taken in `lookup_open`
on a miss, VFS-generic; btrfs "answered" because it was warm). M2: the
experiment code sits in the hand-synced third-party copy tools/fhtest.c.
M3: the documented benefits do not hold for the default capture forms:
a cloned mount in no namespace shows paths relative to the clone, and
AppArmor treats them as disconnected unless the profile allows it; only
`fstype=none` gets real paths; fallbacks stay disconnected in every
form. M4: inconsistent fallbacks, consumed statuses not logged,
`return connectable.status()` for other errnos untested. M5: the kernel
capability is rediscovered per open (probe once at startup, a policy in
Context). M6: the cost is understated (below the root about +6 syscalls;
a cold deep open runs the kernel's reconnect walk with a readdir per
uncached ancestor; every named open also runs the parent's
ReconcileAttrs). L1-L5 docs and tests. NEEDS RUSS (2026-10-09):
orchestrator recommends shelving the phase (keep the branch and the
experiment record; merge separately the TLC configuration that found H1)
over another round for a feature that helps diagnostics only in the
`none` form.

DECISION (russ, 2026-10-09): "Agreed, we're shelving it." Branches
step-13.1 and step-13.4 stay in lane-2 as the record. Merged on its own:
the `MC_ident_power` + out-of-band TLC configuration that found H1, as a
standing identity check (it passes on main). Revisit as 13.5, below, when
a kernel with the dcache preference ships.

## 13.5 Nice-to-have, much later: connected handle opens via a kernel dcache patch (russ, 2026-10-09)

Problem. Phase 13 (connected backing fds) was shelved. Its by-name open
design blocked under filesystem freeze (write-intent path opens take
freeze protection; `open_by_handle_at` does not), and its replacement,
connectable handles built from the current name, weakened identity: the
handle being opened was derived from whatever is at the name now, so the
kernel's generation check no longer applied to the object the cache row
describes, and for symlinks, FIFOs, sockets and devices on filesystems
without birth times nothing was left to compare but the inode number
(TLC-verified: `MC_ident_power` with out-of-band changes violated
`HeldResolvesToItsObject` on the branch, passed on `main`). The review
also found the benefit holds only for the `fstype=none` form: the native
and bind capture forms reach the backing through a cloned mount in no
namespace, so paths are clone-relative and AppArmor treats them as
disconnected anyway.

The 13.4 experiment's observation. `open_by_handle_at` ends in
`__d_obtain_alias`, which returns the first alias in the inode's alias
list and allocates an anonymous (disconnected) dentry only if there is
none. A prior name lookup adds a connected alias at the head of that
list, so a handle open that follows a lookup comes out connected. That
worked on ext4, xfs and btrfs in the experiment but is fragile: it
depends on alias-list order, and a disconnected alias kept alive by an
earlier handle fd stays disconnected.

The patch. Make `__d_obtain_alias` prefer a connected alias when one
exists, skipping `DCACHE_DISCONNECTED` aliases the way `d_find_alias`
already does, and allocate an anonymous dentry only when no connected
alias is present. A few lines in fs/dcache.c, no new ABI, no flag. It is
a generalisation of an existing kernel preference and reads as a bug fix;
nfsd and audit paths benefit too. exportfs territory (Amir Goldstein's
area). The exact current text of `__d_obtain_alias` should be read before
writing it; the description above is from memory plus the observed
behaviour, not from the source.

How dcfs would use it. In `OpenNode`, when the object has a cached
present dentry: an `openat(parent_fd, name, O_PATH | O_NOFOLLOW)` first
(no write intent, so no freeze protection; its only purpose is to populate
the dcache), close it, then the normal `open_by_handle_at` of the stored
handle exactly as today. Identity stays with the stored handle's (ino,
generation): if the name still points at the same inode, the open is
connected; if the name now points elsewhere, the stored handle still
resolves to the right object by generation (or ESTALE), and dcfs gets
today's disconnected fd. No file is ever created, no other inode can be
attached, no dcfs identity code changes, the H1 hole cannot open, and the
freeze property is kept because the open stays on the `dentry_open`
route.

Why it is sound. The name is never evidence of identity; it only supplies
a connected dentry for an inode the kernel has already resolved from the
handle. Hard links give whichever connected alias is first (fine for
diagnostics). A replaced name populates some other inode's alias and has
no effect on the handle open.

Cost and limits.
- Two syscalls per open of a named object instead of one (the O_PATH
  lookup plus the handle open), plus the parent open below the root as in
  the shelved design.
- Best-effort connectivity: memory pressure between the two calls, or a
  name that moved, gives a disconnected fd with no error; dcfs cannot tell
  which it got without reading `/proc/self/fd`.
- Benefit limited to the `fstype=none` form (M3 from the Phase 13 review);
  native and bind captures still show clone-relative paths, and a
  path-LSM profile must still allow disconnected paths for the fallback
  cases (recovery-unknown names, unlinked-but-open files, older kernels).
- Long horizon: no effect until the patch is in a released kernel;
  everywhere else dcfs keeps today's behaviour. Same shape as the pending
  FUSE generation patch.

What exists to reuse. The `step-13.1` and `step-13.4` branches in lane-2
(the `OpenByName` structure, the parent-fd handling, the fallbacks, the
kernel-behaviour guard test `connectable_handles_test`, the
`/proc/self/fd` readlink oracle in `connected_fds_test`); the
`MC_ident_power` + out-of-band TLC configuration that found H1, which
should be merged on its own regardless as a standing identity check; the
review's M1 note that any "blocked under freeze" assertion must be
event-based, not a timer.

Plan: Phase 13 "shelved; revisit as 13.5 when a kernel with the
`__d_obtain_alias` preference ships": (1) draft and send the dcache patch
(russ sends); (2) once released, the O_PATH-then-handle open in `OpenNode`
behind the existing handle-open abstraction, identity code untouched;
(3) the `fstype=none`-only benefit and the LSM requirement documented
honestly in README and design.md; (4) `connected_fds_test` and the guard
test revived, with the freeze assertion made event-based.

