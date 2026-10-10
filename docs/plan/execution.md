# Execution plan

How phases 2-21 are scheduled: which steps run in parallel, on which
model, and where everything must stop and synchronize. `process.md`
describes a single step's life cycle (worktree, test first, review,
merge); this file orders the steps.

## Ground rules

- **Lanes.** The dev machine has 8 cores and 62 GB since the reboot of
  2026-10-10 (4 CPUs, 11 GB before). Tokens are the limit now
  (`process.md`, budget paragraph: about three lanes), not the machine:
  every lane's `user.bazelrc` caps it at `--jobs=4`, `--local_test_jobs=4`
  and 20 GB of build memory, which three lanes share comfortably; raise
  the caps before the lane count. Non-building lanes (reviews, the TLA+
  model, docs) cost nothing here. Watch for guest-test timeouts caused by contention; if they
  appear, pause the newest lane rather than raise timeouts. Phases may
  start out of the listed order when they depend on nothing in flight.
  Where a third building lane would help, it is marked "(lane 3 if the
  machine allows)".
- **Lanes own files.** Two lanes in a wave never edit the same file. The
  "Owns" column lists what a lane may touch; anything else needed from
  another lane's files is a small separate commit the orchestrator lands
  first.
- **Sync points** (S1, S2, ...) are hard stops: every lane of the wave is
  merged to `main`, the listed checks pass, and the orchestrator updates
  `README.md`'s status table and `log.md` before the next wave starts.
  A lane that finishes early may start the next wave's lane only if that
  lane does not depend on the unfinished one; the orchestrator says so
  explicitly.
- **Agent types** (model and effort; definitions in `.claude/agents/`,
  table in `/CLAUDE.md`; escalate one row after two failed reviews). The
  "Model" column below names the model; the type follows from it:
  - **Haiku** -> `dcfs-mechanical` (low effort): precise, pattern-following
    batches.
  - **Sonnet** -> `dcfs-implementer` (medium) by default, or
    `dcfs-investigator` (high) for measurement and investigation: Phase
    5.1 emulation speed, the Bazel-built kernel, QEMU and busybox, and
    any puzzling failure.
  - **Opus** -> `dcfs-protocol` (high): the write-through protocol,
    identity, crash and failure handling, namespaces and mounts, the TLA+
    model. "Opus review" means `dcfs-reviewer` (xhigh, read-only).
  - **Orchestrator**: the strongest available model; reviews, merges,
    runs suites, talks to russ.
- **Kernel config once.** Phase 3's kernel config fragment includes every
  option any later phase needs (device-mapper targets, `UNICODE`,
  `FS_ENCRYPTION`, systemd's requirements, quotas), so later lanes never
  edit it concurrently. A missing option found later is a one-line
  orchestrator commit.
- **Tests per sync point.** Until Phase 6: the full `bazel test //...`
  matrix and `--config=asan` (the ASan suite runs in the background after
  merges, `process.md`, and must be green at the sync point). From Phase 6: `--config=presubmit` per merge
  and the full suite at every sync point. From Phase 5: CI green on the
  merged commit.

## Before wave 1 (russ)

- Review `/AGENTS.md` (done 2026-10-05).
- Optional, any time: the LKML reply.

## Waves

### Wave 1: first fixes and the pinned kernel

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 2: cache database permissions (test first) | Sonnet | `dcfs/main.cc` (database creation), `dcfs/sqlite.*` if needed, a new guest test |
| 2 | Phase 3, part a: Bazel fetches and builds the pinned upstream kernel with the minimal config fragment (`http_archive` plus a build rule in `third_party/linux/`). Added as a second kernel target; the patched kernel stays the default for now | Sonnet, Opus review of the build rule | `third_party/linux/`, kernel lines of `MODULE.bazel`, the kernel part of `test/qemu/kernel.bzl` |

**S1:** both merged; the suite is green on the default (patched) kernel;
the stock kernel boots the guest's boot test.

### Wave 2: stock kernel and pinned tools

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 3, part b: drop the kernel patch (test first: the suite on the stock kernel fails today), stock libfuse, docs | Sonnet | `third_party/libfuse/`, libfuse lines of `MODULE.bazel`, `dcfs/fuse_request.*`, `DirCacheFS::Init`, `dcfs/libfuse_version_test.cc`, docs mentioning the patch |
| 2 | Phase 4: Bazel-built QEMU and busybox, then the Debian image through `rules_distroless` | Sonnet, Opus review of the QEMU build | `third_party/{qemu,busybox,debian}/`, their `MODULE.bazel` lines, `test/qemu/scripts/{run-qemu.sh,mkinitramfs.sh}`, removal of `mkrootfs-debian.sh`, the tool parts of `kernel.bzl` |

The one-line switch of the default kernel to the stock one is landed by
the orchestrator after lane 1 merges.

**S2:** the full matrix and ASan are green on the stock kernel with stock
libfuse and pinned QEMU, busybox and Debian image; the old patched-kernel
path and the out-of-tree kernel repository rule are deleted. Phases 2, 3 and 4 are done.

### Wave 3: emulation speed and test tiers

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 5.1: why emulation (TCG) is slow; measure every test, fix causes, set timeouts from measurements | Sonnet | `run-qemu.sh` (timeouts, accelerator flags), guest `init` |
| 2 | Phase 6.1: test sizes as tiers, real resource tags, `--config=fast` / `presubmit`, the check that every test has a tier | Sonnet | all `BUILD.bazel` test rules, `test/qemu/qemu_test.bzl`, `qemu_cc_test.bzl`, `.bazelrc` test configs |

Lane 2 needs measured times; it uses KVM timings and takes lane 1's TCG
numbers when they land (timeouts are lane 1's, sizes are lane 2's).

**S3:** every test has a tier; `fast` runs in about a minute with KVM;
each test under TCG is within about 10x its KVM time (or russ has been
told why not).

### Wave 4: CI and the first speed pass

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 5: the CI workflow (kernel and image caching, full matrix, ASan, oldest and newest kernel, logs on failure), developed and run locally with pinned `act` before publishing; the full suite under `act` doubles as a check for host dependencies | Sonnet (investigator) | `.github/workflows/`, `third_party/act/` |
| 2 | Phase 6.2, first pass: replace sleeps with event waits, shard long tests, build images once | Sonnet | `test/qemu/guest/*.sh`, `guest/lib.sh`, shard settings in test `BUILD.bazel` rules |

If hosted runners lack KVM, lane 1 stops and reports (russ decides).

**S4:** CI green on `main`, including the kernel matrix. Phases 5 and 6.1
done; 6.2 continues as fill-in work in later waves.

### Wave 5: the pinned toolchain (gate, then two lanes)

**Gate 7.1:** switch every build to the pinned clang (Sonnet, Opus
review). It touches the whole tree, so nothing else runs.

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | 7.2: coverage (instrumented builds, `.profraw` out of the guest, lcov, CI report) | Sonnet | `run-qemu.sh` coverage parts, coverage config in `.bazelrc`, CI coverage job |
| 2 | 7.3: `-Weverything -Werror` with a commented deny-list; fixes in batches by directory | Sonnet; Haiku for mechanical batches | warning flags in `.bazelrc`; source files batch by batch (`dcfs/`, `tools/`, tests) |

Then:

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | 7.4: UBSan over the whole suite; each finding test first | Sonnet; Opus if a finding is in the protocol code | `ubsan` config; fixes in the files of each finding |
| 2 | 7.5: clang-tidy aspect and `.clang-tidy`; fixes by directory | Sonnet; Haiku batches | `.clang-tidy`, the aspect, CI job; fixes by directory |

Lanes 1 and 2 of the second half agree up front on directory ownership for
fixes (7.4 fixes are few and land first).

**S5:** clean build with all warnings as errors, UBSan and ASan suites
green, clang-tidy clean, the first coverage report published. **russ
checkpoint:** review the coverage report together (which gaps matter).

### Wave 6: coverage close to 100%

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 8: the cache checker (run after every test) | Sonnet, Opus review | `testonly/` checker, teardown hook in `guest/lib.sh` |
| 2 | Phase 8: the syscall failure-sweep harness (link-time fakes) | Sonnet, Opus review | `testonly/` sweep library, its BUILD rules |

Then (lane 3 if the machine allows: the SQLite failure VFS shim, otherwise it
follows lane 2):

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Closing gaps in the protocol code: `backing`, `metadata_cache`, `dir_cache_fs` | Opus | tests for those files; fixes there |
| 2 | Closing gaps elsewhere: `sqlite`, `syscalls`, `file_handle`, `device_id`, `main`, `tools/` | Sonnet; Haiku for straightforward tests | tests for those files; fixes there |

**S6:** coverage at the agreed target, every remaining gap listed in
`docs/coverage.md` and agreed by russ; the CI ratchet is on.

### Wave 7: file names and benchmarks

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 9: names are bytes (audit, escaping, the corpus and random tests) | Sonnet, Opus review | escaping functions, log call sites, new guest tests |
| 2 | Phase 10: benchmarks, memory test, idle test (test first) | Sonnet | `bench/` (google/benchmark), new guest tests, dm-delay setup |

Fill-in when a lane frees up early: Phase 15.6, the systemd-booting
Debian guest (infrastructure only, no dcfs changes; Sonnet; owns
`third_party/debian/` systemd variant and its harness).

**S7:** both merged; baseline benchmark numbers recorded in `log.md`;
the idle test passes (or its failure is a fixed bug).

### Wave 8: crash, stress and failure testing

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | 11.1: dm-log-writes power-loss replay and our replay tool | Opus | `tools/replay-log/`, its guest tests |
| 2 | 11.2: fsstress and fsx as external test dependencies, seeded runs | Sonnet | `third_party/xfstests/` (build of fsstress/fsx), guest tests |

Then:

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | 11.3 I/O error injection, then 11.4 out of space | Opus | their guest tests; fixes in the code they expose |
| 2 | 11.6 fsfreeze, then 11.5 instant backing crash (Opus review) | Sonnet | their guest tests; fixes |

Bugs found here are fixed test first in the lane that found them; if two
lanes need the same file, the orchestrator serializes those fixes.

**S8, the trial point:** Phase 11 done, full suite and CI green. russ
may run the spare-disk trial (21.1) whenever he likes; it does not block
wave 9. A spin-up he finds becomes a failing QEMU test, then a fix,
scheduled ahead of the current wave's next step.

### Wave 9: the model and connected fds

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | 12.1: the TLA+ model, its newcomer README, the known-bug variants, pinned TLC | Opus | `formal/`, `third_party/tlaplus/` |
| 2 | Phase 13: connected backing fds (test first) | Sonnet, Opus review | `OpenNode` and helpers in `dcfs/backing.*`, new guest tests |

Then **gate 12.2**, trace validation (Opus): event call sites go into
`backing.cc` and the cache code, so it runs alone after Phase 13 merges.

**S9:** the model finds every re-introduced historical bug; traces from
the Phase 11 tests validate; the action-coverage report is in
`formal/README.md`. **russ checkpoint:** read the model's README.

### Wave 10: identity and the wrapper core

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 14: identity from the backing filesystem, steps 14.1-14.6 in order | Opus | `dcfs/metadata_cache.*`, `dcfs/schema.sql`, migration removal, new `dcfs/handle_identity.*`, identity paths of `backing.*` and `dir_cache_fs.*` |
| 2 | 15.2: the `mount.dcfs` wrapper and daemonization (capture in a throwaway namespace, option split, forms, readiness pipe, syslog), removing `--source`; then 15.3 instance identity, cache path, fsid | Opus for 15.2, Sonnet for 15.3 | `dcfs/main.cc`, new `dcfs/mount_helper/*`, `guest/lib.sh` start-up function, every test's dcfs invocation |

Two Opus lanes at once is the most expensive wave; if the budget is
tight, run 15.2 first and Phase 14 after it. The lanes meet in
`dir_cache_fs.cc` only at `Init` and startup; lane 2 avoids identity code
and lane 1 avoids start-up code.

**S10:** both merged; NFS handles survive a cache wipe; every test starts
dcfs through the wrapper; the model and trace validation still pass
(identity changes may need model updates in lane 1).

### Wave 11: finishing the wrapper

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | 15.4 stubs (needs Phase 14's reserved range), then Phase 16 (reject case-insensitive and encrypted directories) | Opus for 15.4, Sonnet for 16 | stub code in `backing.*`/`dir_cache_fs.*`, their tests |
| 2 | 15.5 `fsck.dcfs` and `dcfs exports`, then the systemd-guest tests of 15.1 (the guest itself is from wave 7's fill-in) | Sonnet | new helper sources, systemd-guest tests |

Then **15.7 docs** (README, man page, design.md; Sonnet) and the
remaining Phase 15 tests.

**S11:** Phase 15 and 16 done: fstab trees with and without systemd,
stubs, exports, man page; full suite and CI green.

### Wave 12: conformance and statistics

| Lane | Work | Model | Owns |
|---|---|---|---|
| 1 | Phase 17: the xfstests subset and expected-failure lists; findings fixed test first (Opus for protocol findings) | Sonnet | `third_party/xfstests/` config, the lists, the guest test |
| 2 | Phase 20: statistics, only if russ has agreed a design | Sonnet, Opus review | the statistics code and its exposure |

**S12:** xfstests subset green against its lists.

### Wave 13: the last automated phases

Phase 18 (EDQUOT; Sonnet), then Phase 19 (soak test; Sonnet). One lane.
The soak test is run by hand, for hours, and its result recorded in
`log.md`.

**S13:** every phase done, every tier green, a soak run passed. russ
runs the final hardware check (21.2) after upgrading the server's kernel
to at least 6.9; only then is dcfs deployed on the server.

## Side track: review fixes (review-fixes.md)

R1 and R2 run in a third, capped building lane (`lane-3`: `--jobs=2`,
one test at a time) alongside wave 3, since russ prefers more parallel
agents and these touch files neither wave-3 lane owns; R3 follows Phase
5.1 in lane-1. All three must be merged before sync point S3.

## Side track: ASan speed (Phase 6.3)

Started 2026-10-06 in whichever lane is idle (investigation; Sonnet,
investigator type after the restart). The investigation and any fix to
`.bazelrc` or `third_party/` BUILD files can run alongside Wave 2's harness
wiring; changes to `test/qemu/*.bzl` wait until that merges.

## Continuous work

- 6.2 speed passes as fill-in whenever a lane is free (measured, never
  weakening a test).
- The coverage ratchet and full coverage of new code at every merge
  (from S6).
- Model updates with any protocol change (from S9).
- `log.md` and the status table at every merge; failures and questions
  for russ under "Needs russ".

## Where russ is needed

| When | What |
|---|---|
| Wave 4, if hosted runners lack KVM | TCG or a self-hosted runner |
| S5 | Coverage report review |
| S6 | Agree the remaining coverage gaps |
| S8 (optional, any time after) | Spare-disk trial (21.1) |
| S9 | Read the model's README |
| Before wave 12 | Statistics design (Phase 20) |
| S13 | Final hardware check (21.2), then deployment |
