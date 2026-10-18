# How dcfs's correctness is established

This document is for a reader who wants to judge whether dcfs is correct, or
on the way to being so, and who would rather check than trust. It lists every
testing and verification regime the repository runs, says what each one
checks and what it cannot, and gives the file, Bazel target, CI job or plan
step where each claim lives. Counts come from the tree at commit `44676d75`
(2026-10-18) unless a number says otherwise, and the commands in
[How to check these claims yourself](#how-to-check-these-claims-yourself)
reproduce them.

Two rules govern how to read it.

- **Enforced versus planned.** A statement in the present tense describes
  something that exists and runs. Planned work is named as planned, with its
  plan step (`docs/plan/phases/`), and appears in
  [Known gaps and open risks](#known-gaps-and-open-risks) against the gap it
  would close. dcfs is pre-release (`README.md`, "Status"): nothing here
  claims it has run in production.
- **What "passes" means.** A passing test is evidence, not proof. The
  [formal model](#the-formal-model) is a proof only within its bounds and
  its abstractions, both of which are listed. The
  [known gaps section](#known-gaps-and-open-risks) is the part to read first
  if you are deciding how far to trust the rest.

Style and maintainability enforcement (linters, clang-tidy, the style rules,
code review as a style gate) is the subject of
[`docs/maintainability.md`](maintainability.md). Mutation testing and
coverage overlap with it and are covered here.

## What is being tested

dcfs (`README.md`, `docs/design.md`) is a FUSE filesystem that mirrors one
backing directory tree and answers metadata operations from a persistent
SQLite cache, so that the disks holding the tree only spin up for file
contents and writes. File contents go from the kernel straight to the backing
file through FUSE passthrough; dcfs never copies them. It is a write-through
cache: the backing filesystem is the only authority, and deleting the cache
database at any time, even mid-operation, must never lose or corrupt data
(`docs/design.md`, "Goals and non-goals"). It runs as root and is meant to be
exported over NFS, so its file handles survive restarts and a handle that
cannot be honoured fails with `ESTALE`.

The specification is, in the project's words, "behave like the backing, only
faster" (`docs/design.md`, "Behave like the backing, only faster"; `AGENTS.md`).
For each operation: with dcfs out of the loop, what does the backing
filesystem do, over time? dcfs does that, with the same results (errnos,
attributes, inode numbers), the same incremental shape (a listing streams
pages), the same interruptibility, and no stronger or weaker guarantees. This
gives the testing a concrete oracle: the backing filesystem itself, run
without dcfs. Where a claim about the backing, the kernel or POSIX is made, it
cites its source (a man page, POSIX text, or `path:line (Linux v7.3-rc7)`),
and where it can be measured a native differential test backs it
(`AGENTS.md`).

### Assumptions the tests do not question

These are stated in `docs/design.md`, "Assumptions", and every regime below
tests dcfs under them, not against them.

- Exclusive access: the backing tree changes only through dcfs. Out-of-band
  changes are detected where that is free and otherwise unnoticed; the
  revalidation model shows which cached answers do not survive them
  ([reval](#the-formal-model)).
- One daemon per cache database (an `flock`), run as root.
- Linux 6.9 or later (`FS_IOC_GETFSUUID`, FUSE passthrough). Tests run on one
  kernel series, Alpine's `linux-virt` 6.18
  ([QEMU guests](#qemu-guests)).
- The backing filesystem keeps its promises: `syncfs` makes earlier changes
  durable, file handles reject stale inodes, inode numbers are unique. Nothing
  is assumed about the order in which unsynced changes persist.
- Single-threaded request handling today. Concurrency is modelled (requests
  interleave at every backing syscall, as they will under the planned
  coroutines) but not exercised by the real daemon
  (`docs/design.md`, "Concurrency, today and with coroutines").

### The invariants that matter

| Invariant | Statement | Source |
|---|---|---|
| Write-through | A mutation reaches the backing filesystem before the cache records it, in three phases: (1) mark the affected records unknown and insert the inodes into the durable `dirty` table, committed durably before the syscall; (2) the backing syscall, as the caller; (3) record the new state, then reply. | `docs/design.md`, "The write-through protocol" |
| Tri-state records | Every cache record that mirrors the backing filesystem is present, absent or unknown. A mutation sets it unknown before the syscall and present or absent after. Nothing is served from an unknown record. | `AGENTS.md`; "The schema" |
| Never serve what the backing does not have | Nothing served from the cache disagrees with the backing filesystem, also after a daemon crash, a kernel crash or a power loss. A crash may cost re-reading, never a wrong answer. | "Goals and non-goals" |
| Dirty set and recovery | A dirty row may be cleared only after a `syncfs` that began after its mutation's syscall returned. Startup recovery forgets what the dirty set names, terminates, and is idempotent. | "Sync points"; "Crashes, power loss and recovery" |
| Sync points | The periodic sync point and `FSYNC`/`FSYNCDIR` make the right things durable in the right order; the cache's durable point follows the backing sync. | "Sync points" |
| Crash consistency | The cache database comes back as a prefix of its commits, an acknowledged synced commit is never lost, and the backing filesystem comes back as its own regime allows. | "A power loss or kernel crash" |
| Identity | A `(nodeid, generation)` never stands for two objects; a handle reaches its object or `ESTALE`, never another object and never `EIO`. | "Identity model" |
| Credentials | Permission outcomes for callers equal the backing filesystem's; the kernel's `default_permissions` check reads cached attributes that must be current. | "Caller credentials" |
| Cancellation | A request the kernel can interrupt on the backing can be interrupted on dcfs, at interruption points just before a backing syscall and never between a mutation's syscall and its phase 3; cancellation never breaks the tri-state rule. | `AGENTS.md`; "Cancellation" |
| Bounded memory | A request's working memory is O(1) in the size of the files and directories it touches. | "Bounded memory" |
| Cost | Warm metadata operations touch no backing disk; costs grow no faster than the backing's. | "Goals and non-goals"; budgets below |
| File names are bytes | No name, link target or xattr name is treated as text. | "File names are bytes" |

## Which regime checks which invariant

Each row names the regimes that bear on an invariant and where they live. A
regime is listed only where it can fail for a violation of that invariant. The
sections after this table describe each regime, its bounds and its blind spots.

| Invariant | Regimes | Where |
|---|---|---|
| Write-through protocol (three phases, order) | TLA+ model with effect-point properties; trace validation of the code against the model; runtime invariant checks in the small/medium tiers; fault sweep by call site; ACE and mixed-fault sequences; mutation testing | `formal/dcfs.tla`; `formal/Trace.tla`; `dcfs/testonly/invariant_checker.cc`; `//dcfs:dir_cache_fs_fault_sites_test`; `//test/qemu:fault_ace_*`; `tools/mutation/` |
| Tri-state records | Model (`TriState`, `CacheNeverWrong`); invariant checker (`tri-state`); mutation testing of `metadata_cache.cc`; e2e tests that compare the served tree with the backing | `formal/MC_small.cfg`; `dcfs/testonly/invariant_checker.cc`; `test/qemu/guest/{create,rename,setattr,write}.sh` |
| Never serve what the backing lacks, also after crashes | Model (`CrashSafe`, `CrashRefines`, `CacheNeverWrong`) across three backing crash regimes; `crash_test`, `power_test`, power-cut tests (dm-flakey, and a real QEMU kill with a second boot); `sqlite_durability_test` for the cache database; ACE sequences; stress runs | `formal/MC_crash_*.cfg`; `test/qemu/guest/{crash,power,fault_power,sqlite_durability,fault_ace,stress}.sh` |
| Dirty set, recovery | Model (`DurableSetSound`, `RecoveryIdempotent`, `RecoveryTerminates`, `ClearOnlyAfterSync`, `RecoveryForgetsDirty`); invariant checker (`dirty-set`, `open-file`, `writable-open`); `power_test`; `fault_ace_mixed_test`; crash-during-recovery trace test | `formal/MC_recovery.cfg`; `//dcfs:dir_cache_fs_crash_during_recovery_test` |
| Sync points, fsync | Model (sync actions; `FsyncedCacheDurable`; the durability model); Ferrite litmus programs; `no_stronger` pinned non-guarantees; `fault_power_test` `lag`/`lagclean`; `fault_freeze_test` | `formal/MClitmus.tla`, `formal/durability.tla`, `formal/known_bugs/*fsync*` |
| Crash consistency of the cache database | `sqlite_durability_test` (dm-log-writes replays, write subsets, torn writes, forged WALs) against the model's abstraction; `fault_cache_test` | `test/qemu/guest/sqlite_durability.sh`, `test/qemu/crash_states.c` |
| Identity (nodeids, generations, handles) | `ident.tla` and its trace validator; `lifetime.tla` for nodeids and FORGET; `handles_test`; NFS test; the identity oracle in the mixed-fault sequences | `formal/ident.tla`, `formal/lifetime.tla`; `test/qemu/guest/{handles,nfs,fault_ace_mixed}.sh` |
| Credentials, permissions | `credentials_test` (two unprivileged users against the backing); pjdfstest; `reval.tla` for the cached mode the kernel's `default_permissions` check reads; xfstests | `test/qemu/guest/credentials.sh`; `formal/reval.tla` |
| Cancellation | Model (`InterruptBound`, `InterruptedReplies`, `InterruptActedOn`, `GuardsBalanced`, reachability of each interruption point); forged `FUSE_INTERRUPT` tests; interrupt sweep; `cancel_test` | `formal/MC_interrupt*.cfg`; `//dcfs:dir_cache_fs_interrupt_sweep_test`; `//test/qemu:cancel_test` |
| Bounded memory | `memory_test` (bytes per inode, a second tree adds no more than half); `readdir_boundary_test` (time linear in directory size); `names_test` | `test/qemu/guest/{memory,readdir_boundary}.sh`. The slope-at-zero allocation test is planned (26.41) |
| Cost: warm metadata touches no disk | Zero-sector assertions in most e2e tests; syscall, FUSE-request and SQL budgets; I/O budgets; slope tests | `test/qemu/guest/*budgets.txt`; `//dcfs:dir_cache_fs_slope_test` |
| Behave like the backing | pjdfstest and SibylFS differential on three filesystems; xfstests `generic`; native differential tests in the guest suites; `kernel_facts_test` | [Differential testing](#differential-testing-and-conformance) |
| File names are bytes | `names_test`, `names_random_test` (1,000 seeded random-byte names, compared byte for byte); the banned-symbol and raw-syscall checks | `test/qemu/guest/names*.sh`; see `docs/maintainability.md` |

## Test tiers and where they run

A test's Bazel `size` is its tier, and every QEMU test macro refuses a missing
`size` or `timeout` (`test/qemu/README.md`, "Test tiers"). `.bazelrc` maps
configs to tiers: `--config=fast` runs `small`, `--config=presubmit` runs
`small` and `medium` (`.bazelrc:68-69`), and plain `bazel test //...` runs
everything not tagged `manual`.

`bazel query 'tests(//...)'` finds 663 test targets:

| Tier | Targets | Examples |
|---|---:|---|
| small | 347 | unit tests; ext4 variant of each e2e test; most TLC configurations and all 61 hand-written trace logs |
| medium | 156 | xfs and btrfs variants; the forged-request trace harness; 80 TLC configurations |
| large | 64 | `nfs_test`, systemd guest, 22 TLC configurations, the four guest trace tests, `cancel_test` |
| enormous | 96 | pjdfstest (3 filesystems, shards), xfstests shards (see below) |

By kind: 327 `sh_test` (the QEMU macros expand to these), 204 TLC runs
(203 under `//formal`), 77 trace-validation tests (61 hand-written logs, 16
guest or harness runs), 48 Python tests (the gates' self-checks and tools), 7
flag/man tests. By location: 70 under `//dcfs`, 264 under `//test/qemu`, 264
under `//formal`, 46 under `//tools`, 15 under `//third_party`, 4 under
`//man`. Of the 663, 91 are tagged `manual` and run only when named (below).
The `kvm` and `e2e` tags mark the 307 targets that boot a guest.

| Where | What runs | Source |
|---|---|---|
| Developer loop | `bazel test --config=fast //...` (about a minute once built, with KVM); `--config=presubmit` before review | `AGENTS.md`; `docs/plan/process.md`, "Running tests" |
| CI `subjects` | commit subject format on the pushed range | `.github/ci/commit_subjects.sh` |
| CI `fast` | small tier | `ci.yml:126` |
| CI `presubmit` | small and medium; needs `fast` | `ci.yml:223` |
| CI `coverage` | `bazel coverage --config=presubmit`, the coverage gate | `ci.yml:314` |
| CI `reproducible` | two builds in two output bases, byte-identical | `ci.yml:426` |
| CI `mutation-changed` | mutants of the functions a push touches (at most 30) and, when `formal/` changes, of the model (at most 10); survivors are reported, not failed | `ci.yml:495` |
| CI `full` | large and enormous, three shards (`test.sh --sizes=large,enormous --shard=I/3`) | `ci.yml:591` |
| CI `xfstests` | xfstests shards, one runner per backing filesystem; does nothing without KVM | `ci.yml:710` |
| CI `asan`, `ubsan` | every tier under each sanitizer, three shards each | `ci.yml:796`, `:895` |
| CI `osv` | dependency vulnerability scan | `ci.yml:994` |
| Weekly `noisy` | the suite with the quiet kernel's determinism removed, `fault_ace_mixed_long_test_<fs>` seeded by run number, `kernel_facts_test_<fs>`; six runners; a report of findings, not a gate | `ci.yml:1161` |
| Weekly mutation | about 340 C++ mutants over six shards and 90 TLA+ mutants over three | `.github/workflows/mutation.yml` |
| By name only | the 91 `manual` targets: 24 slow xfstests shards, 18 native xfstests shards, 16 full SibylFS shards, 6 `xfstests_dev*`, `stress_random_test_<fs>`, the kernel-bug reproducers, `bench_write_test`, 14 TLC bisections of the state-merging `VIEW`, and the three weekly ones above (`kernel_facts`, `fault_ace_mixed_long`, `noisy_kernel_test`) | `bazel query 'attr(tags, manual, tests(//...))'` |

Failing a tier does not stop a lane's work, but the process is: the
orchestrator runs the tier russ expects for the step, CI must be green on a
merged commit from phase 5 on, and a red push is fixed before further queued
work (`docs/plan/process.md`, "Running tests" and the parallelism section).
After launch a push point becomes a sync point at which the candidate commit
gets CI's checks locally first; that is a stated plan, not a mechanism
(`docs/plan/process.md`).

Tests that run in no push job: the 24 slow xfstests shards, the native-run
xfstests shards, the full SibylFS run, `stress_random_test_*`, the kernel-bug
reproducers and `bench_write_test`. They exist to be run by hand or to guide
work (xfstests slow shards: `test/qemu/README.md`, "xfstests (Phase 17)").

## QEMU guests

**Why.** dcfs needs root, `open_by_handle_at`, `FS_IOC_GETFSUUID` and FUSE
passthrough, and many tests need control over the kernel (dm-flakey,
dm-log-writes, fsfreeze, power cuts). So every dcfs test, unit tests included,
boots a guest and runs there as root: `AGENTS.md`, "Tests"; `docs/design.md`,
"Everything runs in QEMU". No code path exists only so a test can run on the
host. Checks that need neither root nor the kernel (the model checker,
formatting, the gates' Python self-checks) run on the host.

**What boots.** The runner is `test/qemu/scripts/run-qemu.sh`. The guest uses
QEMU's `microvm` machine with qboot and direct PVH kernel boot, virtio-mmio
disks, a trimmed kernel, KVM and a small amount of memory; it reaches
userspace in about a second (`test/qemu/README.md`, "Fast boot"). The kernel
is Alpine's `linux-virt` of the pinned release branch (series 6.18), and QEMU,
busybox, the mkfs tools and the firmware are Alpine packages of the same
branch. Bazel fetches them and verifies Alpine's package signatures
(`third_party/alpine/README.md`, `//third_party/alpine:signature_test`); the
kernel's required options are checked against the package's config
(`//third_party/linux:kernel_config_test`). Nothing is a host tool.
`third_party/linux/README.md` says what a second kernel (to test the minimum
supported 6.9) would need; it does not exist yet.

**Filesystems under test.** Disks are made by Alpine's `mke2fs`, `mkfs.xfs`
and `mkfs.btrfs`, so every e2e test of the matrix macros runs on ext4, xfs and
btrfs (the ext4 variant is `small`, the others `medium`). RAM disks (step
26.17) let the budgeted tests avoid the host's disk. Tests that act on the
disk below the filesystem use device-mapper targets in the guest
(dm-flakey, dm-error, dm-delay, dm-log-writes).

**Test shapes.** `qemu_cc_test` wraps a GoogleTest binary in a per-test
initramfs (`test/qemu/qemu_cc_test.bzl`). `qemu_test` runs a guest shell
script against the real daemon (`test/qemu/guest/`, 98 shell files, libraries
among them, in
`lib.sh`). From step 25.34 new guest tests are C++: `test/qemu/guest/guest.h`
provides `DcfsMount::Start/Unmount/Crash/Log` (a mount awaited on the mount
table and the daemon's exit, never on a time), `InChild` (run a function in a
forked child, optionally as another uid/gid/groups, with its `Status` carried
back whole), `Snapshot`, `CompareTrees` and `ExpectSameAsBacking`. A new
`*.sh` under `test/qemu/` is refused by `tools/repo_shape.py` unless
`tools/repo_shape_guest_shell.txt` lists it. Three C++ guest tests exist
today (`guest_test.cc`, `removed_test.cc`, `kernel_facts_test.cc`); the
existing shell tests are being ported (plan M2).

**Which dcfs a guest runs.** `small` and `medium` tests boot
`//dcfs:main_static_checked`, the testonly build that installs the runtime
invariant checker (below); `large` and `enormous` boot the plain build that
ships. `memory_test` and `readdir_boundary_test` boot the plain build because
they measure what ships (`test/qemu/README.md`, "Which dcfs a guest runs").

**Verdicts are made of events, not time.** The project's rule (russ,
2026-10-17) is that a test's verdict counts events and never depends on host
load; time appears only as a hang guard. Concretely:

- A guest is stopped after `TEST_TIMEOUT` less 60 s; that allowance is
  the only timer in the harness besides xfstests' per-test limit
  (`test/qemu/README.md`, "Guest timeout", "xfstests (Phase 17)").
- `run-qemu.sh` refuses to boot a guest whose allowance is below three times
  the work its budgets allow, so a hang guard cannot be shorter than the
  measured work (`cost_wall_allowance`, `guest/strace_lib.sh`).
- A run that lost 20% or more of its wall time to host CPU pressure is marked
  `load-starved`; it is never failed for it, and is not held to CPU budgets
  (`test/qemu/README.md`, "Load starvation").
- The kernel is made quiet: writeback timers, expiry and the vCPU count are
  fixed so that a test's behaviour and coverage depend on nothing but the test
  (`test/qemu/README.md`, "A quiet kernel"; self-check
  `//test/qemu:quiet_kernel_test`). The weekly `noisy` job undoes exactly
  that, with one knob (`DCFS_NOISY=1`), to shake out the races the quiet
  kernel hides.
- `tools/repo_shape.py` allows no new bare `sleep` in guest shell beyond a
  shrinking list (`tools/repo_shape_sleeps.txt`); the project forbids timers
  and retry loops in production code (`docs/style.md` 1.11, 1.12).

**Gates have self-checks.** Every gate (a test or check whose job is to
reject something) ships a committed self-check that feeds it known-bad input
and requires failure, because several gates once passed vacuously (pjdfstest
ran while busybox's `tail -1` did not exist; an ASan benchmark passed while
dcfs was OOM-killed). The table in `test/qemu/README.md`, "Gates and their
self-checks" has about 65 rows; a gate without one is a review finding
(`docs/plan/process.md`, step 5).

## Unit tests, fakes and fault injection

**Counts.** 836 `TEST`, `TEST_F` and `TEST_P` invocations in 46 files under
`dcfs/`, `test/`, `bench/` and `tools/`; 38 `dcfs/*_test.cc` files, with the
large `dir_cache_fs_test.cc` split by `--gtest_filter` into the trace, fault
sweep, interrupt sweep and slope targets (`dcfs/BUILD.bazel:1200-1331`). They
run in guests as `qemu_cc_test` (44 targets in `dcfs/BUILD.bazel`).

**Fakes, not mocks.** `AGENTS.md` bans mocks. Dependencies are injected and
replaced by small fakes in `*_test.cc` or in `testonly/` directories whose
targets set `testonly = 1`; test-only code never lives in production files, and
no production comment may justify code by tests (`tools/repo_shape.py`,
`docs/style.md` section 8). Test seams are real features: the protocol
observer `Context::events` (`dcfs/protocol_events.h`) is a no-op in production,
and the daemon runs it. The harness drives libfuse with forged requests (no
kernel, no mount), which is how interleavings the single-threaded daemon
cannot produce today are exercised.

**Syscall fault injection** is link-time. Each fault test links with
`-Wl,--wrap=<symbol>` fakes in a target of its own so that the wrap affects no
other test. Wrapped today: `fsync`, `getxattr`, `listxattr`, `readlinkat`,
`ioctl`, `fstatfs64`, `fork`, `waitpid`, `dup2`, `flock`, `unlinkat`, `setsid`,
`fuse_reply_none`, `pidfd_open`, `poll`, and, for every `dir_cache_fs_*`
target, `name_to_handle_at`, `open_by_handle_at`, `statx`, `syncfs`, the
passthrough registration calls and a backstop over all 25 `*at`, xattr and
data-mutation calls through which `backing.cc` reaches the backing (so a
backing syscall added without its hook aborts the harness)
(`dcfs/BUILD.bazel:1138`; `docs/design.md`, "Runtime invariant checks").
This is a decision, not a gap: `docs/design.md` "Syscall fault injection stays
link-time (russ, 2026-10-16)". Step **25.41** (drafted, awaiting russ's
review, `docs/plan/phases/25-style-convergence.md:1733`) proposes replacing
the wraps with injected syscall interfaces; it is not built.

**Runtime invariant checks (step 26.2).** The testonly checking build and the
forged-request harness install `testonly::InvariantChecker`
(`dcfs/testonly/invariant_checker.{h,cc}`). It checks, at every backing
syscall, that no transaction or cursor is open and that every inode with a
mutation in flight has its dirty row; and, after every request for the rows it
changed, the invariants named `no-transaction-at-request-end`, `tri-state`,
`identity`, `dirty-set`, `open-file`, `writable-open`, `lookup-count`,
`held-fds` and `removed-record`; and over the whole database at `Startup`'s end
and after `DESTROY`. A violation aborts the daemon and `run-qemu.sh` fails the
run on the console line `DCFS-INVARIANT-VIOLATION` whatever the guest script
said. The checks also run after every injected failure of the fault sweep.
Each invariant has a death test that breaks it on purpose
(`DirCacheFSDeathTest.*`). Not checked by the checker, and said so in
`docs/design.md`: "the dirty set equals the unknown rows" is not an invariant
of this design, and nothing in the checker sees the order of a mutation's
phases (the trace validator does). The checks are not in the build that ships.

## The formal model

The formal model is the strongest evidence for the protocol and the one whose
limits are most precisely known. `formal/README.md` (about 2,900 lines,
written to assume no TLA+ background) is the reference; this section is the
summary a reader needs to judge it. The checker is TLC, run on the host on a
pinned JDK (`third_party/tlaplus/`); each Bazel test is one TLC run, built by
the `tlc_test` macro (`third_party/tlaplus/tlc.bzl`), host-only, so it is
excluded from sanitizer and coverage builds.

### What exists

| Module | Models | Lines |
|---|---|---:|
| `formal/dcfs.tla` (root `MC.tla`) | The write-through protocol for one directory D: three-phase mutations, fills and their guards (`CanFill`, `Mutation::Owns`, the completeness epoch), the durable dirty set and its fast path, sync points, crashes that keep any prefix of each disk's unsynced writes, `StartRun`/`RecoverDirty`/`FinishRun`, plus extensions: a regular file F (access times, step 23.8), born-dirty creates (23.11), interrupts and interruption points (22.5) | 2,592 |
| `formal/reval.tla` | What dcfs reuses across a change of the backing's state: shared descriptors and their access mode, the cached mode `default_permissions` reads, cached negative entries and complete listings | 501 |
| `formal/lifetime.tla` | Nodeids the kernel holds: lookup counts, `FORGET`/`FORGET_MULTI`, removed records, the written-file set and its held descriptors, `DESTROY`, startup sweep | 668 |
| `formal/ident.tla` | What a nodeid and generation stand for: minting, inode recycling, NFS handles, crashes, power loss, cache wipes, out-of-band changes, for today's identity and Phase 14's | 844 |
| `formal/durability.tla` | A clock on the cache database's durability: how old a commit a power loss can take | 244 |
| `formal/MClitmus.tla` | Ferrite-style litmus programs run through dcfs under three backing crash regimes (`seq`, `metaprefix`, `ext4`) | 111 |

All five protocol-level models are small relative to the code: the model of the
protocol is one directory, two names, a handful of requests.

**Properties checked** (names are the TLA+ definitions in the `.cfg` files):

- `dcfs.tla` invariants: `TypeOK`, `CacheNeverWrong`, `CompleteNeverHides`,
  `ServedFromCacheIsCurrent`, `TriState`, `CrashSafe`, `CrashRefines`,
  `DurableSetSound`, `CleanMeansNoDirty`, `GuardsBalanced`,
  `RecoveryIdempotent`, `InterruptBound`, `InterruptPrompt`. Action
  properties: `EffectAtSyscall`, `BackingAtSyscall`, `CacheLearnsAtCommit`,
  `ReplyObservable` (what each request replies, in SibylFS's call, effect,
  return terms: a mutation's effect is its backing syscall and nothing else;
  an answer is one the backing gave at some instant between arrival and reply;
  `EAGAIN`/`EINTR` only from a request whose syscall did not succeed),
  `QuietStepsUncounted`. Temporal: `RecoveryTerminates`, `InterruptedReplies`,
  `InterruptActedOn`. Born-dirty and file extensions add `BornDirty`,
  `LostRowProbed`, `DirtyBeforeChange`, `ClearOnlyAfterSync`,
  `RecoveryForgetsDirty`, `FileExact`.
- `reval.tla`: an open succeeds exactly when the backing would allow it at
  that moment; nothing dcfs holds grants more than the backing would (an open
  descriptor keeps the rights it was granted, as in POSIX); every write has a
  descriptor that can carry it (14 invariants, one temporal property
  `CachedDecisionsConverge`).
- `lifetime.tla`: a nodeid the kernel holds resolves to the object it was
  handed out for; a row or removed record goes only when nothing references it;
  dcfs's lookup count is the kernel's; after a crash the start sweeps every
  unnamed row (12 invariants).
- `ident.tla`: one `(nodeid, generation)` never stands for two objects; a
  handle reaches its object or `ESTALE`, never another object and never `EIO`;
  a row whose object is gone is caught at every entry point (7 invariants).
- `durability.tla`: `BoundAtRequestEnd`, `LostOnlySinceSync`,
  `FsyncedDurable`.

`CrashRefines` is a crash-refinement check in the style of Yggdrasil
(`formal/README.md`, "Properties"). A refinement mapping from the whole
protocol to an ideal filesystem specification (step 12.16) is **not** built.

### Configurations and state counts

There are 52 top-level configurations under `formal/`, 103 known-bug
configurations, 20 limitation configurations, and 204 TLC test targets (203
under `//formal`): 102 small, 80 medium, 22 large tier. State counts are TLC's
distinct states, as recorded in the README's tables from a full run of
`bazel test //formal/...` on 2026-10-18 at load 19 ("all 250 tests passed",
which includes the trace-log tests). These tables are hand-edited
(step 12.27 item 9 plans to generate them), and the README itself notes that
two of its tables disagree for the atime configurations, so treat them as
approximate evidence of scale, not as a checked artifact.

| Configuration | Bounds (README) | Distinct states | Tier |
|---|---|---:|---|
| `MC_small` | 2 names, 2 slots, 2 mutations, 1 crash, kernel lock, all request kinds | 1,869,733 | medium |
| `MC_recovery` | 1 name, 1 slot, 2 mutations, 2 crashes (one during recovery) | 42,479 | medium |
| `MC_liveness` | as small with 1 slot; `RecoveryTerminates` | 244,552 | medium |
| `MC_large` | 3 mutations, 2 crashes, `ReplyObservable` | 10,239,570 | large (about 20 min unloaded) |
| `MC_nolock` | as small without the kernel's directory lock | 8,224,822 | large |
| `MC_nolock_small` | no lock, no crash, the five guard-relevant request kinds | 350,330 | medium |
| `MC_interrupt` | `Interrupts`, 1 mutation | 757,278 | medium |
| `MC_interrupt_muts2`, `_nolock` | interrupts with a later mutation; without the lock | 1,755,235; 2,622,519 | large |
| `MC_interrupt_liveness`, `_slots` | `FairSpec`; 2 slots | 93,959; 196,629 | medium; large |
| `MC_atime*`, `MC_borndirty*` | the file F; born-dirty create | 3,288 to 444,106 | medium |
| `MC_crash_ext4`, `MC_crash_metaprefix` | weaker backing crash regimes | 402,796; 335,050 | medium |
| `MC_litmus_*` (7), `no_stronger/*` (3) | fixed fsync/replace/create programs | 761 to 1,874 | small |
| `MC_reval`, `_oob`, `_liveness` | revalidation | 69,104; 442,736; 67,632 | small to medium |
| `MC_lifetime*` (4) | nodeid lifetime | 23,082 to 317,992 | small to medium |
| `MC_ident*` (9) | identity | 8,961 to 721,557 | small to medium |
| `MC_durability` | durability clock | 764 | small |

Search depth is not recorded anywhere I could find; only distinct states and
elapsed time are. Bounds are small by necessity (every configuration is an
exhaustive search of its bounded space): the guarantee is "no violation within
these bounds", not for any number of names, mutations or crashes. The `View`
in `MC.tla` merges database states a crash may leave when recovery would make
the same cache of them; its soundness was itself checked (step 23.8b found it
was not a congruence for the file extension, and `formal/view_bisect` plus
fourteen manual targets bisect it by comparing state counts at one and four
workers). Liveness is checked without the `View`, because merging states can
hide or invent cycles.

Four configurations leave out the `CacheDurable` step to save states
(`MC_large`, `MC_nolock`, `MC_interrupt_nolock`, `MC_interrupt_muts2`) on an
argument recorded in `MC.tla` (`NoCacheDurable`). Step 12.27 item 10 plans a
parser-based test that checks the argument and fails closed; today it is
argued, not checked.

### The models check themselves

- **Known bugs.** `formal/known_bugs/` holds 69 variant modules and 103
  configurations. Each puts back one real or historical bug behind a `Bug*`
  constant that is `FALSE` in the real configurations, and its test (71 targets
  named `known_bug_*`) passes only if TLC reports the exact expected violation.
  Sources are the audits (crash F1 and F3, tri-state F1 and F4), gaps the model
  itself found (a sync point clearing the dirty rows of mutations in flight;
  Readdirplus listing after a suspension point; Rename trusting a source
  resolved before its phase 1), and bugs of Phases 12, 22 and 23 (identity
  checks, nodeid reuse, interruption points, born-dirty rows, access times).
  If a property stopped biting, its known-bug test would stop failing and
  the suite would go red.
- **Premises.** 31 `premise_*` targets check each property alone on a variant,
  so that every property is shown to bite in isolation
  (`formal/README.md`, "Premises: every property bites").
- **Limitations.** `formal/limitations/` pins what the design does not
  guarantee, each as a configuration whose counterexample is expected (20
  targets): the access time of a directory is the cache's; a power loss while
  a file is open may leave its atime behind; a cache wipe or a power loss that
  rolls back recent rows loses the handles of objects that still exist
  (`ESTALE`, never another object); a filesystem with neither generations nor
  birth times can take a recovered row over for a recycled inode;
  out-of-band changes (the revalidation model shows the cached mode and cached
  directory answers do not survive them); and, for fsync, what the backing
  does not guarantee. These are statements of what to expect, not open bugs.
- **Reachability.** `small_coverage_test` and `nolock_small_coverage_test`
  (large) compare TLC's `-coverage` report with committed reports in
  `formal/coverage/` and fail if an action that was taken no longer is. 13
  `interrupt_reach_<pc>_test` targets show that each interruption point is
  reached by an interrupted request.
- **Mutation testing of the model** (step 12.13; see
  [Mutation testing](#mutation-testing)).

### Findings of the model

Writing the model exposed three gaps that today's single thread and the
kernel's per-directory lock kept unreachable, all fixed (plan step R4) and each
now a known-bug variant: `sync_during_mutation`, `readdirplus_unlocked`,
`rename_stale_source`. The lifetime model found three more, fixed in step 12.4b
(a stub's nodeid handed out again; a removed object's row surviving a crash
between an unlink's syscall and phase 3; a `DESTROY` with an unlinked file
open for reading), and a fourth in 12.6b. `formal/findings/` is empty. This
is evidence the model finds real bugs; it is also evidence of how few it has
needed to: the gaps were ones the code could not yet reach.

### Trace validation: does the code do what the model says?

The model describes the code as it is (not as `docs/design.md` intends), so
the converse needs checking. Every step of the protocol the model has (a
mutation's phase 1, its syscall and its phase 3, a fill's read and commit, an
answer served from the cache, a sync point, a crash, recovery, shutdown) is
reported through `Context::events` (`dcfs/protocol_events.h`), a no-op in
production. Testonly builds link a recorder
(`dcfs/testonly/trace_recorder.cc`, `//dcfs:main_static_traced`) that writes
each step as a line of that directory's trace together with the cached state
after it. `formal/Trace.tla` takes one model step per event and requires the
model's state to match the recorded one; TLC must find a behaviour of the
model that matches the whole trace. A step the model forbids fails the
trace. Validators exist for the directory model (`Trace.tla`), `reval`,
`lifetime` and `ident` (`RevalTrace.tla`, `LifetimeTrace.tla`,
`IdentTrace.tla`).

This is enforced, in 77 test targets (step 12.2 and its follow-ups,
`formal/README.md`, "Trace validation"):

- 61 hand-written trace logs (`formal/trace_tests/`, small tier, host
  only), including deliberate violations that must be rejected.
- 3 `dir_cache_fs_trace_{a,b,c}_test` (medium): the forged-request harness
  run through the recorder without the kernel lock, producing the
  interleavings coroutines will, validated against the model without the lock
  assumption.
- 8 `trace_fault_<fault>_test` (medium): the code is deliberately corrupted at
  link time (a phase 1 that skips marking a name unknown, a syscall before
  phase 1, phase 3 before the syscall, a snapshot after `syncfs`, a swallowed
  syscall error, an ignored interruption point) and validation must reject the
  trace.
- 1 `trace_crash_during_recovery_test` and 4 guest tests
  (`trace_{crash,power,rename,create}_test`, large): the unchanged crash,
  power-loss, rename and create guest scripts run on the traced build with
  the kernel lock, and their root traces are validated.

Validation found places where the code is more conservative than the model
(recovery forgot more than the model's), now reflected in the model.
Limits (`formal/README.md` 1414-1463): cuts for syscall failures other than
`EEXIST`/`ENOENT`, verification of other inodes, and a child row recorded while
a mutation overlaps end a trace instead of being validated; three model
actions are never taken by any trace (`UnlinkFailed`, `RenameFailed`,
`RenameFailed2`); the guests' recorder does not write file traces for `reval`;
`MClitmus` and `durability` have no trace validator. A step the model does not
have (link, cross-directory rename, out-of-band change) ends that
directory's trace where it happens, and only such steps may.

### What the model does NOT cover

From `formal/README.md`, "What is modelled, and what is not" (lines 140-298):

- **One directory.** Only D's records are tracked. Child objects' attributes,
  xattr values, symlink targets, hard links, links and renames across
  directories, `RENAME_EXCHANGE`/`NOREPLACE` and the non-root `ParentOf` are
  out. The model therefore could not have found the stale-resolve gap
  `RemoveChild` had (review of R4, finding 2: found by reading); a unit test
  (`UnlinkMarksWhatItRemovesUnknown`) covers it.
- **Writable opens.** `open_for_write`, the last release that records
  attributes, and the sync point keeping the dirty row of an inode open for
  writing are not modelled: modelling them needs a second inode and multiplies
  every crash-state set. `dir_cache_fs_test` and `metadata_cache_test` check
  the release during a sync point's `syncfs` and the writable create's window;
  the runtime checker's `writable-open` rule and `crash_test` cover the rest.
- **Failures.** Failed cache writes (every `Commit` succeeds; step 11.4's
  `CreatedButNotCompleted` ends a trace), `ESTALE` handling and
  `InvalidateInode`, open-by-handle failures, and a `syncfs` that succeeds
  without making anything durable.
- **Out-of-band changes**, `ReconcileAttrs`, refused boundaries.
- **Time.** The periodic sync's timing (any request may be a sync point at any
  time); the durability model has the clock.
- **Concurrency is modelled; threads are not.** Requests interleave at every
  backing syscall as they will under coroutines; the real daemon is
  single-threaded and the kernel serialises each directory. A claim about
  real parallelism rests on the model alone.
- **The backing filesystem is a model.** Its crash behaviour is one of three
  regimes (`seq`: a prefix of operations, more than real journals allow;
  `metaprefix`; `ext4`: reorderings), checked against real ext4, xfs and btrfs
  only through the ACE and power-cut tests
  ([Crash testing](#crash-and-fault-testing))
  and the litmus tests.
- **The kernel.** The FUSE client's caching, entry and attribute timeouts and
  invalidation are outside the model (step 12.20, "kernel caches in the model",
  is queued). So is the code itself: TLA+ checks the model, and the code is
  tied to it only through trace validation, which covers the traces tests
  produce.
- **No refinement mapping.** There is no proof that the whole system refines
  an ideal filesystem; `CrashRefines` is a per-crash check, and step 12.16
  (an ideal-filesystem specification, to be verified against SibylFS) is
  planned.
- **Bounds.** Everything is within small bounds (2 names, 1 to 3 mutations,
  1 to 2 crashes).
- **Model-code name agreement** is not mechanically checked. The model's
  action and program-counter names are tied to the code by trace validation
  (a renamed action fails a trace test) and by `interrupt_reach_*`, but a
  mapped set such as `InterruptPcs` is not compared with the code's
  `InterruptionPoint` names. Step 12.27 item 7 plans it.

### Planned: unit tests of the specification (step 12.27)

Queued, not started
(`docs/plan/phases/12-formal-model-and-trace-validation.md:1003`):
every `.cfg` assigns every `CONSTANT` (this would have caught 22.5's
configurations lacking `LongNames`); frame completeness (every action primes
or lists `UNCHANGED` for every variable); no orphaned properties; vacuity
checks for `P => Q` properties; spec mutation testing as a test; every
disjunct reachable; model-code name agreement; symmetry soundness (after step
12.26); state counts generated from the last run; the `CacheDurable`
soundness check above. Other queued formal steps: 12.16 (ideal filesystem),
12.19 (no-stronger-than pinned non-guarantees for every operation), 12.20
(kernel caches), 12.21 (a register of environment assumptions and an
abstraction map), 12.26 (faster checking with symmetry).

## Differential testing and conformance

The specification is the backing filesystem, so the strongest tests run the
same operations with and without dcfs and compare.

**Native differential tests.** Most e2e tests check results "both through the
mount and directly on the backing filesystem" (`docs/design.md`, "End-to-end
suites and what each proves"); the ones named below run the same operation
on a directory dcfs never touches. `names_test` and `names_random_test` compare
trees byte for byte; `credentials_test` runs two unprivileged users and
compares ownership, setgid inheritance, ACL behaviour and the setuid/setgid
bits left by writes with the backing's answers, warm and after dropping the
kernel's caches; `atime_test` requires `stat` through dcfs to report exactly
the backing's access time; `copy_test` checks that `copy_file_range` shares
extents on btrfs and xfs as natively; `enospc_*` that a full backing
filesystem gives the backing's errors.

**pjdfstest** (`docs/conformance.md`). Pinned at commit `85a8aea9`. All 238
files, 8,827 checks, run twice per filesystem: through dcfs and directly on the
same filesystem, and the failure sets are diffed. A failure only through dcfs
fails the test; a failure that is on the backing only is listed as a note,
since passing more than the backing is also a divergence. Results: the backing
fails 28 checks on ext4, 30 on xfs and 32 on btrfs (pjdfstest TODOs for
documented Linux behaviour); dcfs fails the same, with zero dcfs-specific
failures; the expected-failure lists are empty
(`test/qemu/guest/pjdfstest.<fs>.expected_failures`). Enormous tier, three
guests per filesystem, in the `full` CI job; the xfs and btrfs runs are
skipped on runners without KVM.

**SibylFS** (step 12.18b, `docs/conformance.md`, "SibylFS, a second oracle").
pjdfstest checks single calls; SibylFS, the 2015 executable POSIX
specification, judges whole sequences of calls with several processes and
users. dcfs runs it differentially: each script runs once in a directory of the
dcfs mount and once on the backing filesystem, the checker judges both traces,
and the test fails only for a script the model allows on the backing and
rejects through dcfs. Measured on 2026-10-17: all 21,018 generated scripts on
ext4 get the same verdict through dcfs as on the backing (17,178 allowed on
both); the model itself rejects 18% on the backing (the 2015 model and
today's kernel disagree), which is why only differences count. The 51
hand-written scripts per filesystem are in CI tiers; a 1/40 sample runs
(`sibylfs_sample_test`); the full run is 16 `manual` guests
(`sibylfs_full_test`),
not in CI. One divergence is listed (`sibylfs.ext4.expected_divergences`):
`lseek(dirfd, 0, SEEK_END)` answers `EOVERFLOW` on ext4
(`ext4_dir_llseek`) but the kernel's FUSE client answers a directory's
`llseek` itself.

**xfstests** (step 17.1, `test/qemu/README.md`, "xfstests (Phase 17)"). The
generic tests of group `auto` run through dcfs on ext4, xfs and btrfs (six
`enormous` shards per filesystem, in the separate `xfstests` CI job), against
lists of the filesystem: expected failures with reasons (8 on ext4, 7 on xfs, 8
on btrfs; the reasons name the README sentence that documents the behaviour,
for example a stale `ctime` after a store through a shared mapping following
the last `close`), about 577 "not run" tests with xfstests' own reasons
(most need a block device or features FUSE does not have), and 6 excluded
plus 40 slow tests. The gate fails on an unlisted failure; on a listed test
that passes, does not run or times out; on a "not run" test not on the list
or with a changed reason (so a broken probe cannot hide a test); and when fewer
than an eighth of the tests ran. The slow shards (24, `manual`) and native
(without dcfs) shards (18, `manual`) are for the weekly run the plan names
(step 17.3) but no CI job runs them yet. **Not run:** xfstests under the
sanitizers (shards are incompatible with them; a curated subset is step
17.1c), and tests needing a raw block device.

**Kernel facts** (`test/qemu/guest/kernel_facts_test.cc`, seven tests, ext4,
xfs and btrfs, `manual`, run weekly by the `noisy` job). Each pins a fact about
the kernel that dcfs relies on, with its citation in `path:line (Linux
v7.3-rc7)` form, and fails when a kernel update changes it: an `O_DIRECT` open
of a FUSE passthrough file is an `O_DIRECT` open of the backing file
(`fs/fuse/passthrough.c:170`, `fs/backing-file.c:34-57`), `F_SETFL` clearing it
leaves the backing file direct, and inotify sees changes made through the
mount, a passthrough write raises `IN_MODIFY` on both, a backing change raises
no event on the mount. Each runs the same steps natively and through dcfs and
compares. `tools/kernel_bugs/` holds two standalone reproducers of Linux bugs
the suite found (btrfs failed inode read, ext4 casefold tune), each with no
dcfs in it.

## Crash and fault testing

Every item here runs in a guest. The oracle is nearly always the same: after
the event and a restart, everything dcfs serves must match the backing
filesystem, no data may be lost that the backing kept, and the checking build
must not have aborted.

| Regime | What it does | Targets |
|---|---|---|
| `crash_test` | `SIGKILL` of the daemon with passthrough writes unflushed; after restart sizes and mtimes match the backing files | 3 (per fs) |
| `power_test` | The state a power loss leaves, made deterministically: mutate, kill, undo each mutation on the backing, restart. Recovery must warn and every touched entry show the backing's truth; with recovery off the checks fail | 3 |
| `fault_backing_test`, `fault_cache_test` | Read and write errors on the backing disk or the cache disk through dm-flakey and dm-error; the error reaches the caller, phase 1 failure means no backing change, the dirty set survives a failed clear | 3 + 3 |
| `fault_recover_test` | Five failure modes over every operation class: the record reads unknown, the error returns, after recovery the same operation works | 3 (+1 unpinned btrfs) |
| `fault_shutdown_test` | An instant crash of the backing (`FS_IOC_SHUTDOWN` on ext4 and xfs, a dead disk on btrfs) under a running daemon | 3 |
| `fault_power_test`, `fault_power_kill_test` | Both disks drop writes at one instant, placed in a create's phases, in a sync point with `fsfreeze`, with atime and born-dirty scenarios; the kill variant has the host kill QEMU at the cut and a second boot check. The kill-mode cut keeps what was synced and loses the rest (self-checked) | 3 + 3 |
| `fault_freeze_test` | What blocks and what does not on a frozen backing | 3 |
| `enospc_backing_test`, `enospc_cache_test` | Out of space on the backing or the cache filesystem at each phase | 3 + 1 |
| ACE-style sequences | Bounded exhaustive sequences of operations and persistence points, then a cut: `fault_ace_a_test` (182 sequences), `fault_ace_b_test` (90), `fault_ace_fs_test_{xfs,btrfs}` (62 each). Oracles: the served tree equals the backing, and the backing holds exactly what it held at the persistence point. Fixtures check the checkers | 4 |
| Mixed-fault sequences (26.14e) | `fault_ace_mixed_test_<fs>`: 22 fixed sequences of an operation plus three events (crash, cut ahead, cut behind, failed phase 3, lookup, listing, sync, handle, drop-ahead). The identity oracle requires every pre-crash handle to open the same object or fail `ESTALE`. `fault_ace_mixed_long_test_<fs>` (120 seeded sequences, `manual`) runs weekly with the run number as seed. The expected-failure list is empty since step 23.11's born-dirty rows fixed the ten it once held | 3 + 3 |
| `sqlite_durability_test` | The cache database on dm-log-writes: replays of every FLUSH prefix, write subsets that keep every FUA write, torn multi-block writes and forged WAL files; recovery must match the model's abstraction (a normal commit may be lost, a synced one may not, survivors are a prefix) on ext4, xfs and btrfs | 3 (+1 short) |
| Fault sweep by call site (26.6) | `//dcfs:dir_cache_fs_fault_sites_test`: one short workload per operation type is run once recording the backing call sites it reaches, then again for each site with its first call failed (`EIO` always, and each errno the code branches on there), followed by the invariant checks, an unclean `Startup` and the checks again | 2 tests |
| Crash during recovery | `dir_cache_fs_crash_during_recovery_test` plus its trace-validated variant | 2 |
| Stress | `fsstress` and `fsx` from a pinned xfstests, tree digested right after, after `drop_caches`, after a restart, each compared with the backing; the checking daemon aborts on a broken invariant. `stress_short_test` (medium), `stress_long_test` (large, fixed seeds), `stress_random_test` (`manual`) | 3 + 3 + 3 |
| `fsck.dcfs` | Cache database checks, tested through the unit harness (with `--wrap` fakes of `fork`, `flock` and others) and the guests | `//dcfs:fsck_test` |

Honest bounds. A guest's page cache survives anything short of a reboot, so
`power_test` constructs the aftermath of a power loss instead of causing
one; the cuts that do cause one are dm-flakey drops (every cut is dm-flakey
plus a remount, never device-level reordering) and the QEMU kill, which
loses what the guest's block layer had not flushed. These cut points are
placed deliberately, not found by search. The ACE sequences are bounded
(length of two or three operations). Crash-consistency of the backing
filesystem itself (ext4's behaviour on an fsync of a directory alone, found by
11.2's ACE tests) is characterised, not assumed away
(`docs/design.md`, "Assumptions"; the litmus tests). The weekly `noisy` job
is the only place races between the daemon's threads and the kernel's
writeback are sought, and, as its own comments say, it had not yet run when
its time estimates were written.

## Cancellation and interruption

`AGENTS.md` requires any operation that can take more than about 100 ms on a
slow backing to check for interruption at points just before a backing syscall
and never between a mutation's syscall and its phase 3, and to have a
cancellation test. Built in Phase 22 (merged 2026-10-08; interruption points
and the model's `InterruptBound`, step 22.5, 2026-10-18). Evidence:

- **Unit harness** (`dcfs/dir_cache_fs_test.cc`, forged requests): 20
  `Interrupt*` tests (an interrupted lookup replies `EINTR` without touching
  the backing; an interrupted population records nothing; a mutation
  interrupted before its syscall leaves its records unknown; one interrupted
  after the syscall completes; a forged `FUSE_INTERRUPT` read by the real
  `SessionLoop`), and `CheckpointTest.InterruptedAtItsFirstCheckpoint`,
  parameterised over 12 request cases, each requiring `-EINTR` and exactly one
  interruption-point hit.
- **Interrupt sweep** (`//dcfs:dir_cache_fs_interrupt_sweep_test`): each
  scenario is interrupted at every backing call, and the calls after the
  interrupt must stay within the request kind's run, mirroring the model's
  `InterruptBound`.
- **Guest** (`cancel_test`, large, two vCPUs, on a dm-delay backing):
  an interrupted or killed `ls` of a cold 20,000-entry directory returns
  within 2 s of the signal, the daemon keeps serving, and the later full
  listing equals the backing's. Measured 0.8 s against 12.1 s with the
  interruption points disabled (`test/qemu/README.md`, gates table).
  `cancel_inventory_test` records timings only.
- **Model:** the `interrupt_*` configurations, `InterruptBound`,
  `InterruptedReplies`, `InterruptActedOn`, `GuardsBalanced`, and the
  known-bug variants `interrupt_after_syscall`, `interrupt_undo`,
  `interrupt_leaks_guard`, `interrupt_ignored`, `interrupt_not_replied`,
  `interruption_point_removed_{create,unlink}`; trace-fault builds
  `interruption_point_ignored*`.

Limits: kernel-blocked syscalls are uninterruptible until coroutines or
io_uring (documented, `docs/plan/README.md` row 22). `cancel_test`'s verdict is
a time bound (2 s), which the load-agnostic rule would replace by a count of
backing requests after the signal (planned, step 26.36 follow-up C). Step 22.7
(a lookup resolves one name instead of populating the directory, as the backing
does) is decided in `docs/design.md` but queued; until it lands the first
lookup in a directory populates it.

## Sanitizers

`--config=asan` (AddressSanitizer, `-O1`) and `--config=ubsan` (undefined
behaviour with `-fsanitize=vptr` and `-fno-sanitize-recover=all`) are defined
in `.bazelrc`, and each runs the whole suite, every tier, in three shards in
its own CI job (`ci.yml:796`, `:895`); the weekly `noisy` job also runs three
ASan shards. Both sanitizers have a self-check that the sanitized build
reports and dies (`//dcfs:asan_runtime_test`, `//dcfs:ubsan_runtime_test`; the
vptr check once failed to die until `-fsanitize=vptr` was named) and a check
that the guest's command line says which sanitizer ran
(`//dcfs:sanitizer_cmdline_test`), which leaves those runs out of the budgets.
The xfstests shards are incompatible with both (the musl image cannot load
the sanitizer runtime); a curated subset is step 17.1c, planned.
ThreadSanitizer,
MemorySanitizer and valgrind are not used. TSan would matter only once there
are threads to race, which the single-threaded daemon does not have.

## Coverage

`bazel coverage` builds our code (`dcfs/`, `bench/`, `tools/`; not tests, not
external code) with clang's source-based coverage and collects the profiles
from the guests (`test/qemu/README.md`, "Coverage"; `docs/coverage.md`).
Profiles are written in continuous mode so that a daemon killed mid-exit still
leaves a complete profile, and counters are atomic.

**The gate** (`tools/coverage_gate.sh`, CI job `coverage`): line and branch
coverage of `dcfs/*.cc` must not fall below the committed baseline
`dcfs/coverage_baseline.txt`:

| | Baseline |
|---|---:|
| lines | 97.27% |
| branches | 83.97% |

The baseline can only rise (russ, 2026-10-11: "You must not lower the coverage
baseline. That's why it exists. Only ever increase it."). A fall of 0.1 point
or more fails the job; a rise of 0.1 or more passes with a note naming the new
baseline. The gate is self-checked (`//tools:coverage_gate_self_check_test`) and
fails on any branch count of 2^31 or more, which is a tooling artifact, not a
count. The baseline began at 92.40% lines and 73.30% branches (step 7.2).
`bazel coverage` reports are also compared across two runs of one commit to
show the determinism the quiet kernel buys (`tools/coverage_diff.py`,
`docs/plan/notes/coverage-determinism-2026-10-08.md`).

**What the number covers.** The report is taken from the small and medium
tiers only (`--config=presubmit`); the large and enormous tests and the
trace-validation guest runs are not in it. Headers, `bench/` and `tools/`
are reported but not gated. A line the suite reaches in a large test only
counts as uncovered. Branch coverage of 83.97% means roughly one branch in six
is never taken by the covered tiers, which is the largest single number in this
document that falls short of the rule that new code is fully covered
(`AGENTS.md`).

**Gaps that cannot be covered** are listed with their reasons in
`docs/coverage.md`: a `flock` interrupted by a signal while blocked (no event
marks "blocked in it", and the project does not wait on timers), `flock` and
`fstatat` failures of the umount helper that no `--wrap` fake covers, a
`waitpid` failure other than `EINTR` that cannot happen, and a function that
returns in two processes. The same file records the artifacts of the tooling
(branch counts of 2^32-1 from lost updates and from forks; both found by
measurement, fixed, and guarded by `scripts/cov-lcov.sh`).

## Mutation testing

`tools/mutation/` generates mutants of the protocol code and asks whether the
suite notices (`tools/mutation/README.md`; plan steps 26.5, 26.5b-d, 26.28 and,
for the model, 12.13). It is "a tool, not a test or a gate", so a surviving
mutant is a finding about a missing test.

**Scope.** C++: all of `dcfs/metadata_cache.cc` and `dcfs/backing.cc`; the
functions of `dcfs/dir_cache_fs.cc` that call `Begin*`/`End*`/`Mark*` plus 13
named ones; the functions of `dcfs/fuse_ops.cc` that call `FinishSyncPoint`
(`tools/mutation/scope.txt`). TLA+: `formal/dcfs.tla`, `ident`, `lifetime`,
`reval` and the four trace modules (`tla_scope.txt`; `dcfs.tla` alone yields
about 1,350 candidates).

**Operators.** C++ (`operators.py`, eight): `negate` (a condition becomes its
negation), `relational` (`<`/`<=`, `>`/`>=`, `==`/`!=`), `logical`
(`&&`/`||`), `enum-swap` (`kFound`/`kNegative`, `kPresent`/`kAbsent`),
`constant` (an integer literal becomes N+1, N-1 or 0), `delete-statement` (an
effect call becomes `(void)0`; a `Begin*`/`End*`/`Mark*` call becomes
`OkStatus()`), `status-return` (an error return becomes OK and the reverse),
`swap-args` (two adjacent same-typed arguments exchanged). TLA+
(`tla_operators.py`, nine): `negate`, `drop-guard`/`drop-conjunct`,
`drop-disjunct`, `swap-junction`, `relational`, `constant`, `durability` (flip
the sync argument of `Commit`), `drop-step`, `swap-step`. Places where a
mutant would be meaningless (logging, `DCHECK`, `ToString`, TLA+ `VIEW`
definitions) are listed in `arid.txt` and `tla_arid.txt`.

**What kills a mutant.** C++: `bazel test --config=fast //dcfs/...` (the
small tier, including the invariant checker); survivors then face the
trace-validation tests (`//dcfs:dir_cache_fs_trace_test`). TLA+: the small
tier of `//formal/...`, then the medium tier (which includes the known-bug and
limitation tests). A killer is re-run without the test cache and a pass makes
the mutant `flaky`, not killed.

**Survivor policy.** A survivor is resolved by a new test, a deletion, or an
entry in `equivalent.txt` with a reason (11 entries today, all `End()` deletions
on a path that returns at once after a refused interruption point, each with a
reason that `mutate_test.py` enforces). A survivor does not fail CI: only a
tooling error (exit 2) fails either job, as `ci.yml` (line 495) and
`mutation.yml` say. (The repository `README.md` CI table, line 932, says the
per-push job "fails on a survivor"; the workflow files say it does not, and I
have followed them.) Making C++ survivors fail the per-push job is proposed
(step 25.13) and not built, so a survivor blocks a merge only if a reviewer
holds the step to it.

**When it runs.** Per push (`mutation-changed`): the functions the push
touches, at most 30 mutants and 120 minutes, and, when `formal/` is touched, at
most 10 TLA+ mutants. Weekly (`mutation.yml`, Sundays): about 340 C++ mutants
over six shards, `--per-function 2`, and 90 TLA+ mutants over three. The weekly
job publishes the `mutation-survivors` artifact and the badge in `README.md`
(`killed K/TOTAL`, total counting suppressed and invalid mutants).

**Recorded results** (all in `tools/mutation/README.md`; later weekly results
are not recorded in the repository and I could not verify them):

| Run | Result |
|---|---|
| C++, step 8.2c | 60-mutant sweep: 51 killed, 7 survived, 2 invalid; survivors killed by new tests (8.2c, 8.2e, 8.2f: 11 of 12 survivors) |
| C++, step 26.5d, 2026-10-08 | 354 mutants generated (343 to run, 11 suppressed); a sample of 30: 18 killed, 8 survived, 3 invalid, 1 flaky; survivors went to 8.2e |
| TLA+, step 12.13, 2026-10-09 | 20 mutants, small tier only: 11 killed, 9 survived, 0 invalid. Survivors include a `TypeOK` junction swap (checked only by medium and large configurations), two dropped `Next` disjuncts (`UnlinkSyscall`, `AttrChangeSyscall`), a flipped `S2` durability argument, and a dropped conjunct of `FileExactStrict`; no resolution is recorded |

The first full sweeps were samples. There is no recorded mutation score for
the current tree and no baseline file of survivors.

## Cost and I/O budgets

The aim is a mechanical bumper around performance regressions that does not
move with host load (russ, 2026-10-17). A test is held to what it costs, not
how long it took (`test/qemu/README.md`, "Budgets in three parts"). All
budget files are in `test/qemu/guest/` and are enforced by `run-qemu.sh`
through `strace_budget_compare` (`guest/strace_lib.sh`), each with a self-check
(`run_qemu_cost_test`, `strace_lib_test`, `cost_report_test`).

| Budget | File | Lines | What it holds | Tolerance |
|---|---|---:|---|---|
| Backing syscalls, procfd syscalls, syncs, SQL statements and transactions per operation | `syscall_budgets.txt`; `syscall_traces_test`, plus ordered goldens | 96 (non-comment) | counts from `strace` and the checking daemon's cost counter | none; below passes |
| FUSE requests by opcode per user-level operation | `request_budgets.txt`; `request_counts_test` | 24 budgets | e.g. `ls -l` of 100 entries | none |
| Disk I/O | `io_budgets.txt` | 89 budgets, 11 targets | reads, writes, sectors, flushes of the test's disks; the daemon's own I/O from the cgroup's `io.stat` | none; budget is the highest count seen plus the spread seen |
| CPU time of dcfs | `cpu_budgets.txt` | 12 budgets | the cgroup's `usage_usec` of the processes running dcfs's code | 100% (`COST_CPU_TOLERANCE_PCT`) |
| Slopes | `//dcfs:dir_cache_fs_slope_test` | 6 operation classes | statements, transactions, durable WAL fsyncs and backing syscalls at N = 100 and 1000 bounded by `a*N + b` (create: 79N+3 statements, 7N transactions) | the bound |

Reading the table honestly:

- Counts of syscalls, requests and statements are exact. The I/O budgets are
  counts of requests, which vary with writeback timing on the write side (ext4
  commits its journal by guest time) and were measured over 8 quiet and 7
  loaded runs per target; reads are mostly exact. `write_test_ext4` wrote 329 to
  336 requests against a budget of 325 at host load about 50 (step 26.36
  follow-up I, not resolved); a target whose writes move with the host's speed
  has only read-side lines (`xfstests_1_test_ext4`).
- A count below its budget passes, so a counter that read nothing would pass
  everything; `run-qemu.sh` fails a target whose `reads` budget is above zero if
  the guest counted none, and `boot_test` and `ram_disks_test` fail if a direct
  1 MiB write does not show in the counters.
- CPU time is time, not a count. It is not held under a sanitizer, coverage,
  emulation, the noisy kernel, or a load-starved run (20% host CPU pressure).
  A tolerance of 100% means it catches order-of-magnitude regressions, not
  small ones.
- Budgets are pinned to the Alpine 6.18 kernel; only a kernel bump may
  re-baseline them.

I could not find the term "interim disk-read headroom" in the repository.
What exists is the rule above: an I/O budget is the highest count observed plus
the observed spread, so a read-side regression smaller than that spread is not
caught.

**Planned.** Step 26.41 (queued) makes the primary bumpers exact counts by
backing-operation kind with zero tolerance, replaces the two-point slope check
with several geometrically spaced sizes that must be exactly collinear, adds
`EXPLAIN QUERY PLAN` checks that no SQL statement does an unallowed full-table
scan, and measures allocations at two sizes with the slope held at zero (the
bounded-memory check). Step 26.36 follow-up A would count user instructions
instead of CPU time. Neither is built.

## Stress, soak and benchmarks

- **Stress:** `fsstress` and `fsx` ([crash testing](#crash-and-fault-testing)).
- **Soak** (phase 19): a plan only (`docs/plan/phases/19-soak-test.md`). No
  soak target or workflow exists. It gates the "Ship" milestone M5.
- **Benchmarks:** `//bench:dcfs_bench` (google/benchmark) runs `Stat`,
  `OpenClose`, `SmallRead`, `Lookup`, `Readdir` on the backing, dcfs with
  default timeouts, and dcfs with zero timeouts, plus the same over a slow
  dm-delay backing; `Startup`, `Recovery`, `Memory`. `bench_smoke_test` runs in
  the small and medium tiers, `bench_full_test` in `full`. No timing is gated,
  so benchmarks guard that they run, not that they stay fast; the speed
  guard is the budgets above.
- **Real hardware:** the spare-disk trial (step 21.1, russ, 2026-10-11) ran on
  the production server: standby held under metadata work, a read woke the
  disk, and 175k files were listed over NFS with the disk asleep; russ's
  verdict was success. The final hardware check (21.2) is not done.

## Supply chain

- **Pinned, hermetic dependencies.** Third-party code is fetched and built by
  Bazel (`MODULE.bazel`, `third_party/*/README.md` say how to update each pin);
  the C++ toolchain (LLVM 22.1.8 and a Debian sysroot) takes nothing from the
  host but glibc's runtime (`//tools:toolchain_hermetic_test` and its
  self-check).
- **Reproducible builds.** `bazel run //tools:reproducible_build` (CI job
  `reproducible`) builds `//dcfs:main`, `//dcfs:main_static` and the man page
  from two copies of the repository in two output bases with no disk cache and
  fails unless the outputs are byte-identical. Only one host has been measured.
- **Vulnerability scanning.** CI job `osv` (weekly, and on every push)
  scans each shipped dependency by upstream commit with osv-scanner; findings
  can be ignored only in `osv-scanner.toml`, each with a reason and an expiry
  date, and an expired ignore fails (`tools/sbom/`, `//tools/sbom:sbom_test`).
  Test-only pins are scanned informationally.
- **Alpine packages** (kernel, QEMU, busybox, mkfs tools) are pinned to a
  stable branch, not a hash, and verified by Alpine's signatures
  (`third_party/alpine/README.md`). Consequence: a rebuild inside the branch can
  change the test kernel without a commit; the kernel-facts tests and the
  per-kernel budgets exist partly to notice.

## Process guarantees

These are rules, enforced partly by tooling and partly by review, from
`AGENTS.md`, `docs/style.md` and `docs/plan/process.md`.

- **Test first.** Every bug fix and behaviour change starts with a test that
  fails on the current code, quoted in the commit message under "Failing first"
  (`docs/style.md` section 7). Where a bug cannot be reproduced
  deterministically (coroutine-only interleavings, a real power loss), the
  closest honest test is written and its limits stated (`docs/design.md`,
  "Test-first"). This is a rule checked by review; no tool verifies that a
  test failed first.
- **Never weaken a test** to make it faster or to make it pass (`AGENTS.md`).
  Gates that could rot into vacuous passes carry self-checks (above).
- **Review before merge.** Every step's full diff is reviewed by a style
  reviewer and, for protocol, identity and oracle steps, by a second reviewer
  of a stronger model, before the orchestrator merges it
  (`docs/plan/process.md`, "A step, start to finish", step 5; the checklist
  includes test first, behave-like-the-backing, the invariants, coverage of new
  code, the TLA+ model updated for protocol changes, and a self-check for every
  gate). Findings are appended to `docs/plan/review-findings.tsv`: 173 rows
  from 2026-10-17 on, of which 9 blockers, 64 should-fix and 93 nits; 143 are
  recorded `fixed/merged`, 17 `decided`, 8 open. The ledger records the
  reviewers' own findings, including 12 test-gap and 14 test-shape findings, so
  it also shows where tests were weak. It started on 2026-10-17; earlier
  reviews are in `docs/plan/audits/` and `docs/plan/log.md`.
- **The model is kept in step with the code.** A protocol change updates the
  TLA+ model in the same change, and trace validation fails if the code does
  something the model does not allow (`AGENTS.md`).
- **CI.** Red CI on a merged commit is fixed before further queued work (a
  "red push" is dispatched first, `docs/plan/process.md`, "Parallelism and
  budget"); large-tier failures are caught by CI rather than before merge
  while development is fast, and the plan says push points become green-only
  after launch (milestone M5: "red CI then gates merges"). Nothing here claims
  CI is green now: I did not check.

## Known gaps and open risks

This section is the reason to be cautious. Each item names what is untested
or unmodelled and the planned step that would address it, if there is one.

**Model scope.**

- The protocol is modelled for one directory under small bounds; children,
  hard links, cross-directory renames, writable opens, out-of-band changes
  and failed cache writes are not (see
  [What the model does NOT cover](#what-the-model-does-not-cover)). Unit tests
  and the runtime checks cover some of these, not by exhaustive search.
  Closes: no step plans a multi-object protocol model; 12.21 (an abstraction
  map) would at least register what is abstracted.
- There is no refinement proof against an ideal filesystem (12.16, planned).
- The kernel's FUSE caching is not modelled (12.20, queued). The identity and
  durability claims that depend on kernel behaviour rest on the
  `kernel_facts` tests and the guests.
- The backing filesystem's crash behaviour is one of three modelled regimes.
  Whether real ext4, xfs and btrfs stay inside them is tested by ACE sequences
  and litmus programs, not proved.
- Model-code name agreement and the specification's own consistency (frame
  completeness, orphaned properties, vacuity, reachability of every disjunct,
  the argument for omitting `CacheDurable`) are not mechanically checked (12.27,
  queued). Known-bug variants and the TLA+ mutation tool partly stand in; the
  first TLA+ mutation run had nine survivors of twenty, unresolved in the
  record.
- Trace validation covers the traces the tests produce; whole classes of
  step end a trace instead of being validated (listed above).
- State counts in the README are hand-edited and in places inconsistent
  (12.27 item 9).

**Concurrency and platform.**

- The daemon is single-threaded and requests are never concurrent in the real
  code; the concurrent behaviours (coroutines, io_uring) exist only in the
  model and the forged-request harness. No race detector runs.
- Tests run on one kernel series, 6.18. The minimum supported kernel, 6.9, is
  not tested (a second kernel and a Bazel flag are needed; `ci.yml` header,
  `third_party/linux/README.md`, "Kernel matrix").
- Power loss is simulated (dm-flakey drops, QEMU kill), not real; the
  hardware trial (21.1) was a manual run on one server, and the final
  hardware check (21.2) and the soak test (19) are not done. dcfs has not seen
  production use (`README.md`, "Status").
- Network behaviour is tested only by a loopback NFSv4 re-export (`nfs_test`,
  large tier); the 21.1 trial exercised a real NFS client once, by hand.

**Coverage and mutation.**

- Branch coverage is 83.97% and line coverage 97.27% over the small and medium
  tiers, against a rule that new code is fully covered. Large-tier-only paths
  count as uncovered; documented unreachable gaps are few and listed. Closing
  this is phase 8 ("coverage to 100"), still in progress, and milestone M4
  names a coverage target.
- Mutation testing is a sample, not a sweep: the recorded C++ sweeps ran 30
  and 60 mutants; there is no recorded current score or survivor baseline;
  survivors do not fail CI (step 25.13 proposes that they do).

**Flaky and load-sensitive tests, as documented.**

- `cancel_test` uses a 2-second bound (step 26.36 follow-up C would count
  backing requests instead). The wall-clock hang guards are deliberately
  generous.
- I/O write counts of long-running tests move with writeback timing (above).
  CPU budgets are time-based with a 100% tolerance and are not held on a
  loaded host. Step 26.34 triaged the load-sensitive tests of 2026-10-17;
  follow-ups remain open in `docs/plan/phases/26-bumpers.md`.
- The weekly `noisy` run is a search for races whose findings do not gate
  anything, and its first run had not happened when its estimates were
  written; I did not verify whether it has run since.
- ASan and non-KVM runs differ from the default: some tests are
  incompatible with sanitizers, and without KVM the xfstests and the xfs and
  btrfs pjdfstest runs are skipped by CI.

**Differential testing.**

- xfstests: about 577 tests per filesystem are "not run" by their own reasons
  (no block device, no features FUSE has); the 40 slow tests and the native
  comparison are not run in CI; the shards do not run under sanitizers.
- The full SibylFS run is manual.
- Behaviour that differs from the backing today: a lookup populates its
  directory (decided against, step 22.7 queued); kernel-blocked syscalls are
  uninterruptible; remount `ro` is ignored and `dcfs.ro` must be used
  (`xfstests.ext4.expected_failures`, generic/294); atime of directories and
  symlinks is the cache's; mtime and ctime after a store through a shared
  mapping following the last close are late (README "Limitations"). These are
  documented and pinned by expected-failure lists or limitation
  configurations, but they are departures from the stated specification.

**Process.**

- Test-first, the review checklist and the rule that surviving mutants block
  a merge are enforced by people and agents, not tools. The review findings
  ledger
  begins 2026-10-17.
- Coverage of the planned dependency-injection step (25.41) and the shell-to-C++
  port of guest tests (plan M2) are open; until they land, many guest tests
  are shell scripts of the older style.

## How to check these claims yourself

Run each from a checkout with `/dev/kvm` writable (without it guests fall back
to emulation, 2-9x slower and some jobs skip). Plain `bazel` from the
checkout.

```bash
# Test counts by tier (663 total at the time of writing)
for s in small medium large enormous; do
  bazel query "attr(size, $s, tests(//...))" | wc -l; done
bazel query 'attr(tags, manual, tests(//...))' | wc -l          # 91
bazel query 'kind(_tlc_test, //...)' | wc -l                     # 204
bazel query 'kind("_tla_trace.*", //...)' | wc -l                # 77

# The developer tiers
bazel test --config=fast //...          # about a minute with KVM, once built
bazel test --config=presubmit //...     # small + medium

# The formal model (about 20 minutes; no KVM needed for TLC itself)
bazel test //formal/...
bazel test //formal:small_test
bazel test //formal:known_bug_crash_f1_phase1_not_durable_test
# Trace validation (needs KVM)
bazel test //dcfs:dir_cache_fs_trace_test //test/qemu:trace_crash_test

# Conformance and differential tests
bazel test //test/qemu:pjdfstest_test_ext4 //test/qemu:sibylfs_test_ext4
bazel test //test/qemu:kernel_facts_test_ext4 \
           //test/qemu:kernel_facts_test_xfs //test/qemu:kernel_facts_test_btrfs
.github/ci/test.sh --tag=xfstests-ext4      # slow; CI runs it per backing fs

# Crash and fault tests
bazel test //test/qemu:fault_ace_a_test //test/qemu:fault_power_kill_test_ext4 \
           //test/qemu:sqlite_durability_test_ext4 \
           //dcfs:dir_cache_fs_fault_sites_test

# Sanitizers
bazel test --config=asan //dcfs:all
bazel test --config=ubsan //dcfs:all

# Coverage and its gate (the same steps as CI's coverage job)
bazel coverage --config=presubmit //...
cat dcfs/coverage_baseline.txt                                   # 97.27 / 83.97
.github/ci/coverage.sh --config=presubmit

# Mutation testing (see tools/mutation/README.md for the full workflow)
bazel run //tools/mutation:mutate -- --help
cat tools/mutation/scope.txt tools/mutation/equivalent.txt

# Reproducible build and the gates' self-checks
bazel run //tools:reproducible_build
bazel test //tools/... //test/qemu:run_qemu_verdict_test \
           //test/qemu:run_qemu_cost_test

# What CI runs, and its schedule
less .github/workflows/ci.yml .github/workflows/mutation.yml
less test/qemu/README.md     # tiers, budgets, gates and their self-checks
less formal/README.md        # the model, its tables and its limits
less docs/plan/review-findings.tsv
```

To check that a regime bites, break something and watch it fail: remove a
`MarkUnknown` call in `dcfs/metadata_cache.cc` and run
`bazel test --config=fast //dcfs/...` (the mutation tool automates this); put
`BugEffectBeforeSyscall`-style behaviour back by running a known-bug target and
read its expected counterexample in `bazel-testlogs/formal/<test>/test.log`.
