# Phase 8 — Coverage close to 100%

**Decision (russ, 2026-10-05).** Coverage is a priority: get close to
100% as soon as possible, then keep it there.
- Measurement is 7.2 (clang source-based coverage, unit and QEMU tests
  merged). It moves to the front of the toolchain phase: switch compiler,
  then coverage, then warnings and UBSan.
- Scope: production code (`dcfs/`, the wrapper, helpers); tests and
  `third_party/` excluded. Line and branch coverage both reported; the
  goal is ~100% of lines and every branch, with each remaining gap either
  covered or listed in `docs/coverage.md` with the reason and agreed with
  russ (no silent exclusions; unreachable code is removed or turned into
  a CHECK).
- Error paths are most of what is uncovered in code like this. Reach them
  with fault injection, not one-off tests:
  - **Syscall failure sweeps:** a test-only interposer (the existing
    link-time `-Wl,--wrap` fakes) that fails the Nth call of a given
    syscall with a given errno; a test runs a request once to count calls,
    then reruns it failing each call in turn, asserting the request
    returns an error, the cache stays consistent (the cache checker,
    below), and
    nothing leaks. The approach SQLite uses for its own I/O error tests.
  - **SQLite failures:** a test VFS shim (SQLite's documented VFS
    interface) that fails the Nth read, write, sync or lock with
    SQLITE_IOERR, SQLITE_FULL or SQLITE_BUSY, swept the same way.
- **Cache checker, run after every test** (built here because the
  failure sweeps need it; Phase 11 relies on it too). A test-only tool
  compares every cached entry (present/absent dentries, attributes, xattrs, symlink
  targets) with the backing filesystem. Every QEMU test runs it at teardown
  against the cache it used, new or reused: a cache filled during the test
  is exactly what drift would corrupt. A mismatch fails the test.
- Ratchet: CI fails if coverage drops below the last recorded value
  (stored in the repo). Every later phase's "done" includes full coverage
  of the code it adds; AGENTS.md states the rule.
Owner: Sonnet for the sweeps' infrastructure (Opus review); gap-closing
by area owners. Order: right after the toolchain phase, before
everything after it.

## Decisions from the first report (russ, 2026-10-08; notes/coverage-baseline-2026-10-08.md)

- The gate: `dcfs/*.cc` only, lines AND branches, fails on any drop
  below a committed baseline file that starts at today's 92.4 / 73.3 and
  only moves up (bumped in the commit that raised it); bench/ and tools/
  reported, not gated.
- The non-passthrough data path is NOT dead: the kernel grants
  passthrough per mount but can refuse it per file (`fuse_backing_open`
  returns ELOOP past the backing stack depth, and kernels without
  CONFIG_FUSE_PASSTHROUGH never grant it), so READ/WRITE fall back to
  dcfs. Corrected 2026-10-08 (8.4): the planned dcfs-over-dcfs guest
  variant is impossible today, because dcfs refuses a source on a FUSE
  mount at startup (FUSE has no filesystem UUID, `FS_IOC_GETFSUUID` is
  UNIMPLEMENTED, and README requires a stable source identity). The
  fallback is covered by a harness test instead (the harness's
  passthrough_open fails with ENOTTY, so a forged READ reaches it).
  Needs russ: whether sources without a filesystem UUID (FUSE, others)
  should be supported, which needs another source identity (Phase 14).
- `default_permissions` is REQUIRED (russ, 2026-10-08): `Init()` verifies
  the mount options dcfs built contain it and refuses to start otherwise
  (test); a `--fuse_opt` value naming `default_permissions` is rejected as
  redundant (test); the FUSE `Access` handler stays as a fail-closed path
  (EACCES + an error log "ACCESS received: default_permissions is not in
  effect"; an ENOSYS reply would make the kernel ALLOW), covered by a
  forged ACCESS request in the harness. The kernel's checks from cached
  attributes cost no backing I/O; an ACCESS request per permission
  decision would be either no more faithful or a backing syscall per
  check. Dead for real, delete: `FuseRequest`'s move-assignment, the
  `openat2` wrapper.
- `FileHandle::ToString` gets a test; style rule to add to docs/style.md
  (tests): a debug/log string is tested for the important fields it must
  contain (substrings or a regex per field), never compared to a
  hard-coded whole.
- Cheap gaps, one mechanical step: status.cc errno-name edges, device_id
  parse failures, mountinfo parsing edges, utimensat/removexattr on a
  closed file, main.cc startup failure modes (a guest misuse test).
- Error branches after syscalls and SQLite steps: not hand-written; 26.6
  and Phase 11's cache-disk error injection cover them.

## 8.2 Mutation survivors (from 26.5's first run, 2026-10-08)

Each survivor is a missing test; tests first, then the mutant must die:
1. `dir_cache_fs.cc:1257` `ForgetRemoved`: `DeleteInode` reporting NotFound
   must still let the FORGET of a removed inode succeed.
2. `dir_cache_fs.cc:1619` `RecordWrittenAttrs`: the double failure
   (attribute refresh fails AND `MarkAttrsUnknown` fails): needs the
   cache-disk error injection of Phase 11; recorded, not now.
3. `dir_cache_fs.cc:1779` `Release`: a failing `RetireRemoved`.
4. `dir_cache_fs.cc:2406` `CopyFileRange`: a deleted `mutation->End()`
   goes unnoticed: a copy_file_range followed by a listing or sync point
   that depends on the mutation having ended.

8.2 done 2026-10-08 (4af0981): #1 `TmpfileUndoToleratesARowThatIsAlreadyGone`,
#3 `ReleaseOfAnUnlinkedFileWhoseRowCannotBeDeletedWarns`, #4
`CopyFileRangeEndsItsMutationBeforeItsRefreshes`, each shown killing its
mutant; #2 waits for Phase 11. A missing `Mutation::End` cannot make trace
validation reject: no model has an event for the end of an attribute
change (a 12.x item). 8.2b, from the first per-push run: `Release`
(`dir_cache_fs.cc:1704`, `writable` negated in a `?:`) and `Fallocate`
(`:2370`, `mutation->End()` deleted: the #4 pattern).

8.2b done 2026-10-08 (55d0128): `ReleaseReportsWhetherTheOpenCouldWrite`
(kills mutants 211 and 224), `FallocateEndsItsMutationBeforeItsRefreshes`
(250). 8.2c, from a 60-mutant sweep (51 killed, 7 survived, 2 invalid,
211 s per mutant under load), all in `dcfs/dir_cache_fs.cc`:
`Setattr:655` End deleted; `ReconcileWritten:762` `!IsNotFound` negated;
`ReconcileWritten:832` `!refreshed.ok() && !IsNotFound` negated;
`OpenInode:1488` `!shared` negated; `OpenInode:1525` `backing_id > 0`
negated; `Release:1738` `!closed.ok()` negated; `Setxattr:2103`
`!stored.ok()` negated.

## 8.2d Survivors from the per-push mutation job (2026-10-08)

The first `mutation-changed` run that got past its tooling error (local,
range 38303ec..fee154d, 22 of 30 mutants run before the host load made it
too slow): 17 killed, 5 survived, each to be killed by a test or listed
as equivalent in `tools/mutation/README.md` (the `End()` deletions that
return at once are the known equivalent class):
- `dir_cache_fs.cc:989` negate-?:
- `dir_cache_fs.cc:1604` negate-if
- `dir_cache_fs.cc:1671` negate-if
- `dir_cache_fs.cc:2178` delete-call End
- `dir_cache_fs.cc:2189` delete-call End
After 11.4 and 25.3 land (both edit dir_cache_fs.cc), so line numbers
are re-derived with `mutate.py generate`.

## 8.2e Survivors from the first expanded sweep (26.5d, 2026-10-08)

**Provisional:** the 26.5d review found the per-function sampler biased
toward the alphabetically first operators, so this list is from a biased
sample; the sweep is being rerun after the fix and the list replaced.

A 12% sample (40 of 340 mutants, seed 1, 5 per operator) over
`dir_cache_fs.cc`, `backing.cc` and `metadata_cache.cc`: 24 killed, 14
survived, 2 invalid, 18 mutants/hour at host load 16. Each survivor gets
a test or an `equivalent.txt` entry with a reason; the protocol ones
first (an invariant a test should pin):
- `backing.cc` `ProbeChild` swap kAbsent/kPresent; `RecordNewChild` swap
  kAbsent/kPresent; `FsyncDirFd` error->ok; `StartRun` `||` to `&&`;
  `ProbeRecoveredRows` delete `gone = true`; `RecordNewLink` value->error;
  `InitRoot` delete BackingCall and negate-if; `UnlinkAt` delete
  BackingCall; `ReconcileAttrs` nudge 0 to 1; `WriteFile` nudge 0 to 1.
- `dir_cache_fs.cc` `Fallocate` negate `removed`; `Tmpfile` nudge 0 to 1.
- `metadata_cache.cc` `WithStatx` swap `stx_rdev_major`/`stx_rdev_minor`.
Line numbers are in the 26.5d README's "First sweep" table; re-derive
with `mutate.py generate --all` after the dcfs/ branches in flight land.

## 8.2f Survivors from the per-push mutation job on 4bf7182 (2026-10-08)

The first complete `mutation-changed` run (30 mutants over the push's
touched functions, 26 killed, 1147 s): `dir_cache_fs.cc` `OpenInode:1608`
negate-?: `(flags & FS_IMMUTABLE_FL)`; `Removexattr:2427` delete-call
`mutation.End()`; `Fallocate:2542` negate-if `!removed`;
`CreateChild:633` delete-call `mutation.End()`. The two `End()` deletions
are candidates for the equivalent class (12.11 covers directories only;
a file's End is caught by the harness tests or not at all). Join the
8.2e list when the re-sweep replaces it.

