# Phase 26 — Bumpers: narrow checks around generated code

**Decision (russ, 2026-10-07).** Narrow, mechanical checks that constrain
the production code the agents write, beyond coverage and review. Each
step is small; the order favours the failure modes already seen (three
gates passed vacuously in one week: pjdfstest's `tail -1`, the missing
`usleep`, ASan `bench_smoke` passing while dcfs was OOM-killed).

- 26.1 Self-checks for every gate. Each enforcement target ships a
  known-bad fixture it must reject, as its own `small` test: the format
  test over an unformatted file (with 7.6), the oops detector over an
  oops log (exists: run_qemu_verdict_test), the raw-syscall test over a
  `::mkdir`, the coverage gate over a dropped line (with 7.2/8), the
  pjdfstest wrapper over a sabotaged expected-failures list, the
  `require_commands` check over a missing applet, `tools_test` over a
  wrapper outside its tree (exists). A list in `test/qemu/README.md`
  "Gates and their self-checks"; a gate without one is a review finding.
  Owner: dcfs-mechanical.
- 26.2 Runtime invariant checks in test builds. A testonly build
  (`--//dcfs:check_invariants`, like the trace-recorder build) in which
  dcfs asserts at the end of every request what the review checklist says
  in prose: no database transaction is open when a backing `syscalls::`
  call is entered (a guard in `Context` the wrappers consult: this one
  first); every row is in a legal tri-state; the dirty set equals the set
  of unknown rows (modulo the sync snapshot); lookup counts non-negative;
  `held_fds_` equals the held entries of `written_`. A violation aborts
  with the request and the invariant named. The harness and the guest
  tests run against this build in the fast and presubmit tiers (the plain
  build stays what ships and what the large tier runs). Test first: each
  check gets a fault test that breaks the invariant on purpose. Owner:
  dcfs-protocol. Deviations as built (2026-10-07): the small and medium
  tiers boot `:initramfs_checked` by tier selection in `qemu_test`
  instead of a `--//dcfs:check_invariants` flag (a Starlark flag would
  reconfigure every target and drop the analysis cache per switch); the
  guard is a `Context::checks` hook called at each backing call site
  (plus a harness `--wrap` fake that holds the hooks in place) rather
  than inside the wrappers; "the dirty set equals the unknown rows" is
  false as written (populations and invalidations leave unknown rows
  that are not dirty) and is checked one-way, as is "no held fd beside a
  writable shared fd" (Release keeps a held fd across a later writable
  open). No existing test violated any check.
- 26.3 Golden backing-syscall traces per operation, with strace (russ:
  observe reality, not our accounting). Alpine's `strace` in the guest
  (alpine_package); a helper `strace_op` in lib.sh quiesces the daemon,
  attaches `strace -f -y -e trace=%file,%desc,%fstat ...` to it, runs one
  operation, detaches, and reduces the trace to the backing mount's calls
  (the `-y` paths separate backing from the cache database) as
  `syscall(path-kind)` lines; a checked-in golden per operation and cache
  state: a cached LOOKUP = nothing, a cached READDIR = nothing, unlink =
  `unlinkat` + the phase-3 `fstatat`, create, rename, write-through,
  FORGET of a written file = `statx` + `close` through the held fd (the
  text first said "nothing"; design.md's FORGET hook does a statx), ...
  A diff fails the test, with the trace printed. Owner: dcfs-implementer.
  Done 2026-10-07 (44da625): Alpine strace (+3.8 MB per e2e initramfs,
  dynamic with musl's loader), `strace_lib.sh`, goldens for warm lookup/
  stat/readdir (empty), cold lookup (13 backing calls), create (26),
  unlink (13), mkdir (12), rename (15), write-through (12: open/release
  only, passthrough), fsync (14), chmod (24), forget-written (2); stable
  over 3 runs; self-check; `--sync_interval_sec` raised in the test so
  the periodic syncfs stays out. Findings: design.md says the generation
  is read before `name_to_handle_at`, the code does it after (doc or
  identity question for 12.5/14); the per-op counts (create 26, chmod 24
  incl. `security.capability` / `posix_acl_access` reads) are the first
  exact cost statement and worth an investigator's look once 26.4 pins
  them.
- 26.4 Ratchets on deterministic counts only (russ: wary of brittleness).
  From 26.3's traces and a counting SQLite hook: backing syscalls per
  operation, SQLite statements and transactions per operation, fsyncs per
  operation. A checked-in budget file; the test fails when a count rises;
  raising a budget is an explicit edit whose commit says why. No
  time-based or size-based ratchets; memory peaks are reported (MEM
  lines), not gated. Owner: dcfs-implementer, with 26.3.
- 26.4b Cost accounting as tests, extended (russ, 2026-10-08). First
  replace the step counting through `VLOG(2)` lines (`--v=2`, counting
  `sqlite3_step:` text: it formats every statement, costs memory under
  ASan, slows the instrumented daemon, and depends on a log format) with
  a counter behind the no-op production hook interface the invariant
  checker uses, and that is the protocol-event recorder's (russ,
  2026-10-08: reuse the trace-validation instrument): the recorder
  already sees every request, phase, fill, sync point, FORGET, RELEASE,
  open and lifetime step, so request-level accounting is a reduction of
  the trace a test already produces; SQLite steps (below the protocol,
  hundreds per op) become a counter method on the same hook object, not
  trace lines; and the two testonly hooks that now exist (protocol
  events, `Context::checks`) merge into one observer interface with one
  no-op in the plain build. Then: (1) FUSE requests per user-level
  operation: how many READDIRPLUS/GETATTR/LOOKUP/OPEN the kernel sends
  for `ls -l` of N entries, `find`, `stat` of a path N deep, `cat` of a
  file, counted from the trace's request events, budgeted like 26.4;
  (2) slope tests for every operation
  class, not only readdir: create N, unlink N, rename N, mkdir N, cold
  lookup N, setattr N, each bounded a*N + b in SQLite steps, transactions
  and backing syscalls at N = 100 and 1000 in the harness (the create
  path, 119 steps / 10 transactions / 2 fsyncs, is the first target: the
  slope test must count fsyncs per create in ONE directory across N
  creates; destroy_test's 5 ms per create on the runner says they are per
  create, which the design's durable phase 1 may or may not require).
  Not adopted: allocations per operation (maybe later; cheap), recovery
  and shutdown budgets (ad hoc measurements suffice), workload budgets
  over realistic sequences (no).
- 26.5 Limited mutation testing (russ: limited). Scope: `metadata_cache.cc`
  and the protocol paths of `dir_cache_fs.cc`; operators: negate a
  condition, delete a call to a phase function (Begin*/End*/Mark*),
  swap present/absent; killed by the small tier plus trace validation.
  `mull` on the pinned clang (after 7.1/7.2) or a small clang-rewriter
  mutator if mull does not fit. Runs on the schedule (nightly/weekly) for
  the whole scope and, per push, only for functions the push touched. A
  surviving mutant is a missing test, filed as a step. Owner:
  dcfs-investigator for the tooling, dcfs-protocol for the survivors.
- 26.6 (russ, 2026-10-07: "sure, we can try", still sceptical; the
  step's report MUST state the wall time it adds to the tier, and the
  step is dropped if that is not seconds) In the
  forged-request harness, bounded by CALL SITE, not by dynamic call:
  short targeted workloads (one per operation type, about ten
  operations each); every backing call site reached by the workload is
  failed once (fresh in-memory cache database per iteration, the
  `--wrap` hooks failing that site's first call), with one errno (EIO)
  except where the code branches on the errno (ENOENT, EEXIST, ENOSPC,
  ENAMETOOLONG: those too); 26.2's invariants and recovery checked
  after each. About a hundred sites x 2-3 errnos: a few hundred
  iterations at milliseconds each, one guest boot, small tier. Never
  "every N of a long workload". After 26.2.
- 26.7 Bazel visibility as the layering rule (russ yes, 2026-10-07): split
  `//dcfs:syscalls` into the backing-reaching wrappers (visible only to
  `//dcfs:backing` and its tests) and the process-local ones (visible to
  all), so the relaxed syscalls rule is enforced by the build graph.
  Trivial; with 26.1.
- 26.8 Banned symbols in the shipped binary (yes): a small test runs `nm`
  over `//dcfs:main_static` against a deny list: `realpath`, `getcwd`,
  `std::filesystem::*`, `nftw`, `fts_*`, `glob` (no paths after startup);
  `system`, `popen`, `exec*`, `dlopen`; `pthread_create` (single-threaded
  by design; exactly one when the notifier thread arrives); `strerror`;
  the `printf` family. A banned symbol names its rule in the failure.
  With 26.1.
- 26.9 Dependency golden (yes): `bazel query deps(//dcfs:main_static)`
  reduced to external repositories, compared with a committed list; a new
  dependency in the shipped binary is a deliberate edit. Feeds the SBOM.
- 26.10 Injected clock (yes; russ: "why do we need the current time?"):
  two call sites in dir_cache_fs.cc: the periodic sync point (`last_sync_`
  / `sync_interval_sec`) and relatime at read-open (a raw `clock_gettime`,
  which 25.1b's rule also catches). Use Abseil's own: `absl::Clock`
  (`absl/time/clock_interface.h`) carried in `Context`, the real clock in
  production and `absl::SimulatedClock` (`absl/time/simulated_clock.h`,
  `AdvanceTime`) in tests; no clock type of our own (russ, 2026-10-07).
  Production code never reads the time otherwise (a 7.5b matcher / 26.8
  symbol). Makes the sync interval testable in the
  harness (the destroy_test time-dependence). Before Phase 11.
- 26.11 Strong types (yes; after 25.2, it changes core signatures):
  `BackingFd` (minted only by backing.cc), `CacheFd`, `Nodeid`,
  `Generation`, a bytes `Name` type instead of `std::string`, phase tags;
  a wrong-kind argument no longer compiles.
- 26.12 Reproducible build (yes; after 7.1): two builds in different
  output bases give byte-identical `main_static` and `dcfs.8`; flushes
  embedded paths/timestamps and host leaks of the `uuid.h` kind.
- 26.13 Repository-shape tests (yes; with 26.1): every `third_party/<name>/`
  has a README; every guest script is used by a `qemu_test`; every
  `DISABLED_` check is named in README's limitations; every commit
  subject on a CI push range starts with a plan step (`N.M:` or a listed
  prefix: plan, style, warnings, notes).
- Not adopted: schema golden + migration round-trip (russ: no users yet,
  migrations do not matter until launch); a bespoke in-memory reference filesystem for differential
  testing (pjdfstest, xfstests and fsstress already test against the
  kernel; TLA+ test generation, 12.10, is the random driver).

Order: 26.1 (+26.7, 26.8, 26.13) and 26.2 as lanes free up (no new
tooling); 26.3 + 26.4 and 26.9 next; 26.10 before Phase 11; 26.5 after
7.2; 26.12 after 7.1; 26.11 after 25.2; 26.6 after 26.2 (report its added runtime).

## 26.5d Expand mutation testing with our own tooling (russ, 2026-10-08)

Keep `tools/mutation/mutate.py` (clang AST-JSON, Bazel-run killers); no
third-party framework (Mull lags our LLVM pin and runs outside Bazel;
Dextool needs a D toolchain). Borrow the ideas from Google's mutation
testing papers (Petrović & Ivanković):
- Operators, each a small named class with a unit test on a fixture
  file: relational replacement (`<`↔`<=`, `>`↔`>=`, `==`↔`!=`), logical
  replacement (`&&`↔`||`), the existing negations, constant nudges
  (`+1`/`-1`/`0` on integer literals in conditions and arithmetic),
  statement deletion of a void call or an expression statement (the
  write-through calls especially: `MarkDirty`, `ClearDirty`, `syncfs`,
  phase-3 record calls, `End()`), return-value replacement for
  `absl::Status`/`StatusOr` functions (an error path returning OK, an OK
  path returning an error), and argument swap for two same-typed
  arguments.
- Arid nodes: no mutants inside logging and diagnostics (`LOG`, `VLOG`,
  `PLOG`, `CHECK` message streams and their conditions' message parts,
  `AbslStringify`, debug-string functions, `DCHECK`), inside testonly
  hooks, or in `ToString`/`DebugString`; the list lives in a data file
  with reasons and has a test.
- Sampling: at most one mutant per source line and a bounded, seeded
  sample per function (the seed printed and settable), so the weekly job's
  cost stays flat as operators grow; the per-push `changed` mode keeps a
  mutant budget per push (deterministic selection, surplus reported as
  "not run").
- Equivalent-mutant memory: a data file (`tools/mutation/equivalent.txt`
  or similar) of suppressed mutants keyed by a stable id (file, function,
  operator, before/after text, not line numbers) with a reason each;
  `tools/mutation/README.md`'s prose list moves there; a test rejects an
  entry without a reason.
- Reporting: survivors grouped by function with the operator and the
  one-token diff; the weekly job's summary carries killed/survived/invalid
  per operator and mutants per hour, so the next sweep can be judged.
- Done when: the operator set and suppressions are tested, one weekly-style
  sweep over `dcfs/dir_cache_fs.cc` and `dcfs/backing.cc` has run (report
  its survivors per operator as the next 8.2x list, do not fix them in this
  step), and `changed` mode over a recent range stays under its budget.

## 26.14 Quiet kernel in the guests; coverage determinism (russ, 2026-10-08)

Goal: a test's behaviour and its coverage depend on nothing but the test.
Under KVM the guest clock is wall time, so the kernel's spontaneous
activity (writeback, reclaim, FORGET under pressure, timer-driven work)
is the remaining source of run-to-run variation. Take the spontaneity
away rather than the clock:
- `guest/init` sets, before any test runs: `vm.dirty_writeback_centisecs=0`
  and a large `vm.dirty_expire_centisecs` (writeback only on our `sync`/
  `fsync`/sync points), `vm.laptop_mode=0`, `vm.vfs_cache_pressure` low
  enough that dentries and inodes are reclaimed only by `drop_caches`
  (reclaim is already asserted absent by `require_no_reclaim`),
  `kernel.randomize_va_space` as it is (dcfs is PIE: say whether ASLR
  affects anything observable), one vCPU for every non-stress test (the
  matrix macros gain the knob; stress and cancel tests keep theirs); each
  setting with a comment saying which kernel event it removes and which
  explicit trigger replaces it (`drop_caches_quiesced`, `sync`, `fsfreeze`).
- Measurement: run `bazel coverage --config=presubmit` twice on one
  commit (CI reruns of the coverage job, or two local runs) and diff the
  per-file line and branch counts; zero difference is the target; every
  difference names a test with a kernel-timing dependence to pin to an
  explicit event (or to document in `docs/coverage.md`). Record the
  before/after diff in `notes/`.
- Back pocket (russ): QEMU `-icount` with record/replay gives a fully
  deterministic guest clock and device inputs but needs TCG (10-30x
  slower, one vCPU); if ever needed for a replay tier, the tests
  themselves have speed headroom to make up for it. Not now.
Owner: dcfs-investigator; after the current dcfs/ branches land.

Status 2026-10-09: merged b0d57de (lane-2, seven commits; rebased cleanly,
fast 229 + 2, quiet_kernel/mount_dcfs/atime targets green under the new init). As built: `dirty_writeback_centisecs=0`,
`dirty_expire_centisecs=8640000` (a day; larger overflows the kernel's
centisecond product), `laptop_mode=0`; `vfs_cache_pressure` stays at 100
(deviation: at 1 the slab shrinkers' counts are scaled down so `drop_caches`
dropped 2 of 1091 dentries and request_counts/syscall_traces/write_test lost
their FORGETs; `quiet_kernel_test` checks drop_caches still drops); ASLR left
at 2 (the only observable effect: Abseil's hash seed changes the iteration
order of `DirCacheFS::Destroy` and `MountFds::Fds`; no test asserts it);
`cpus` on qemu_test/qemu_test_matrix/tla_trace_test, default 1, stress,
cancel, pjdfstest and bench keep 2 via CONCURRENT_CPUS. Fast tier wall
1701 s to 1282 s with `--nocache_test_results` (host load 11-13: noisy).
Two presubmit coverage runs of one commit: the combined report differs in
two branches only; per test 65 of 141 differ (2280 line/branch
differences): `tools/coverage_diff.py` + `coverage_diff_test` is the check,
`notes/coverage-determinism-2026-10-08.md` the list. Follow-ups, each its
own step, none fixed in 26.14:
- 26.14b (harness): the daemon's clean-shutdown path (`FinishRun`,
  `main.cc` after the session loop, `mount_fds.cc`, the sqlite close) runs
  in one run and not the other for seven tests (146-220 lines each): the
  agent's hypothesis, inferred from the scripts and not observed, is the
  cleanup's `umount; kill` racing the daemon's post-loop shutdown. Make the
  cleanup wait for the daemon's exit after the unmount (or kill it before,
  deterministically), then rerun the diff. After 6.5 merges (same scripts).
- 26.14c (coverage): `dir_cache_fs.cc:817` branch 1.2 (`DropLookups`) reads
  4294967295 in one run of `bench_smoke_test_btrfs`: a counter underflow or
  a profile-merge bug; find which before trusting branch counts there.
  Done 2026-10-09, merged 8f0d3c2: a negative difference of region counters.
  The daemon's profile counters were plain increments shared by its threads
  (and by the four daemons bench_smoke starts, one continuous-mode profile
  per binary); lost updates left the entry counter at 12217 against 12220
  runs, and llvm-cov's branch count 12216 - 12217 printed as 2^32-1. Runs:
  1 of 3 plain runs at 2 vCPUs showed it (17 functions negative), 1 vCPU
  lowers the rate (1 function), `-fprofile-update=atomic` removed every
  negative count in 5 of 5. Fix: `coverage --copt=-fprofile-update=atomic`
  in .bazelrc; the gate fails on any branch count >= 2^31, naming it, with
  two self-check fixtures (failing first); `docs/coverage.md` created with
  "Known coverage artifacts"; run-qemu.sh gains DCFS_KEEP_PROFRAW and
  DCFS_FORCE_CPUS (off by default). Likely also part of 26.14's per-test
  differences: 26.14b's rerun will tell.
- 26.14d (flake): `dir_cache_fs_test` `ARowGoneDuringPhase3IsNotLoggedAsAFailure`
  failed once with the WARNING `supplementary groups unreadable ... pid 0`
  and passed 5 of 5 reruns; cause not established (a request with pid 0 is
  the kernel's own, e.g. a FORGET or writeback; the test's expectation on
  the WARNING count may be racing one).
  Done 2026-10-09, merged dae95e1: a fixture bug, not a daemon bug. The
  harness forged every request with pid 0, so every mutation hit
  `FuseRequest::Caller`'s "supplementary groups unreadable" WARNING, which
  is `LOG_EVERY_N_SEC(WARNING, 60)` per process: whichever WARNING-counting
  test ran first after a 60 s window saw it (1 of 40 runs, a different
  test). Fix: forged requests carry the test's pid (FORGET, BATCH_FORGET,
  INIT and INTERRUPT keep 0, as the kernel sends them); two new
  fuse_request_channel tests pin the groups path and the pid-0 WARNING
  (which stays: a real caller outside the daemon's pid namespace loses its
  groups, and the administrator can fix that); design.md "Caller
  credentials" says which requests read groups. 20 of 20 reruns, fast 229 +
  2, pjdfstest three shards green.
- FORGET vs BATCH_FORGET after `drop_caches` and the session loop's
  read-ahead/`-EINTR` paths differ between runs: document in
  `docs/coverage.md` or pin; part of 26.14b's rerun.

## 26.15 Kernel bug reports: reproducers and drafts (2026-10-09)

The suite found two kernel bugs that russ wants reported upstream (log.md
"Needs russ"); russ sends the mail, the step prepares everything else:
- ext4: `EXT4_IOC_SET_TUNE_SB_PARAM` (6.18+) switches the casefold feature
  on under a mounted filesystem without loading `sb->s_encoding`; `chattr +F`
  checks only the feature bit; the next readdir of that directory
  dereferences NULL in `utf8byte` (`ext4fs_dirhash <- ext4_readdir`).
  Reproducer in the tree: `guest/casefold_tune_oops.sh`
  (`casefold_tune_oops_test`, a DISABLED_ check with `kernel_failure =
  "expected"`).
- btrfs: a failing inode read (dm-flakey under the device) WARNs in
  `btrfs_destroy_inode`; found by 11.3's fault sweep, pinned around by the
  btrfs variant (`fault_recover_btrfs_unpinned_test`, `kernel_failure =
  "expected"`, run-qemu.sh tolerates exactly that WARNING when the guest
  reports it).
For each: a standalone reproducer with no dcfs in it (a shell script that
needs only a scratch block device or a loop file, the pinned kernel's
version and config fragment); the oops/WARNING text and the call chain from
the serial log; the code path in the upstream source (read-only; the
current mainline, to say whether the bug is still there and where); a
search of lore.kernel.org for an existing report or fix (do not draft a
duplicate: link it instead and say whether the pinned kernel lacks the fix);
a draft report per bug in `notes/kernel-bugs-2026-10-09.md` in the form
the lists expect (subject with the subsystem prefix, kernel version, steps,
the oops, the analysis, a suggested fix in prose, the `#regzbot` line only
if it is a regression with a known-good version), with the maintainers and
lists from the pinned tree's MAINTAINERS. No patches: russ decides whether
to write one. Owner: dcfs-investigator, lane-5.
Done 2026-10-09, merged badacf3 (lane-5, three commits; the first agent was
blocked by the auto-mode check, a fresh one was not). Reproducers under
`tools/kernel_bugs/{ext4_casefold_tune,btrfs_failed_inode_read}/`
(POSIX sh + a static helper, no dcfs), installed into the initramfs under
/kernel_bugs/ and proven by two manual guest targets (`kernel_bug_*_test`,
`kernel_failure = "expected"`). btrfs cause proven: relatime from a listing
updates the directory's inode item only in memory, `btrfs_fill_inode`'s
fast path leaves `index_cnt = -1`, a failed on-disk read then goes through
`iget_failed` -> `make_bad_inode` (S_IFREG), and `btrfs_destroy_inode`
reads `index_cnt` as `csum_bytes` (shared storage since d9891ae28b0d,
v6.11): spurious; an unanswered syzbot report (2024-09-12, 6.11-rc6) exists,
so the draft is a reply. ext4 present in mainline af32da41b032 (7.3-rc6);
no prior report found (lore 403 to the tool; mirror + web searched).
Drafts with To/Cc from the pinned MAINTAINERS in
`notes/kernel-bugs-2026-10-09.md`. NEEDS RUSS to send: search lore by hand
(two queries in the note), fill in the syzbot Message-ID, decide on
today's mainline reviewers, attach reproduce.sh and the helper.

## 26.14e Noisy run (russ, 2026-10-09: "Yes to noisy job")

Also in this job (russ, 2026-10-09, "I agree"): mixed-fault sequences.
The ACE generator today runs pairs of one fault kind; the 23.11 bug needs
a daemon crash (or a failed phase 3 through the fault points), a lookup,
then a power loss before the backing commits. Extend the generator to
seeded sequences of three events drawn from {daemon crash, power loss
ahead, power loss behind, failed phase 3, lookup, listing, sync, handle
taken}, judged by 11.7's identity oracle plus the path oracle; the long
tail runs in this weekly job, a small fixed sample in the large tier.

Status 2026-10-09: built (lane-5, three commits on 2c8a25c; Opus review
running, focused on whether the generator can reach 23.11's state and on
the identity oracle's soundness). As built: one knob, `--test_env=DCFS_NOISY=1`,
which run-qemu.sh turns into the cmdline word `dcfs_noisy=1` (init skips
the sysctl block) and at least two vCPUs (DCFS_FORCE_CPUS still wins); a
`DCFS_SEED` knob for the weekly draw; `quiet_kernel_test` fails under the
knob and the new manual `noisy_kernel_test` fails without it, both from a
shared `kernel_mode_lib.sh` with a host-side canned-/proc test; the
`noisy` job (weekly and workflow_dispatch, six runners: plain shards with
small+medium x3 runs, large+enormous once and the mixed long tail per
backing; ASan shards small+medium x3; noisy_kernel_test first; findings to
the summary via tools/noisy_report.py, exit 0 on test failures; 95-145
min per runner estimated, unmeasured, 300-minute limit). Mixed faults:
`fault_ace.sh` kind `mixed` draws one operation plus three events from
{crash, crash3 (kill with a create held in phase 3 by freezing both
disks), cutahead (a create elsewhere first, so the cache is ahead, then
cut), cutbehind, fail3 (ENOSPC on the cache), lookup, listing, sync,
handle}; path and identity oracles after every restart and at the end
(the identity oracle in its minimal form in fault_lib.sh: each recorded
handle opens to the same inode and type with a name under the mount the
backing also has, or ESTALE; a `stat` subcommand added to fhtest; 11.7's
helper should replace it); `fault_ace_mixed_test` (large, 20 sequences
per backing, ~150 s) and `fault_ace_mixed_long_test_<fs>` (manual, 120)
for the tail; self-checks: a tampered handle record rejected, an
`identity_oracle_test` over a fake fhtest with eight violations. No true
positive on main in 150 + 70 + 39 targeted sequences; the agent's own
caveat: in `create:crash3:handle:cutahead` the cutahead lost the create
on the backing and the handle correctly gave ESTALE, so its timing may
not match 23.11's (backing writes dropped from the freeze onward); the
review decides. `fd_blocked` (a pre-existing 0.2 s poll) reused for "held
by a freeze". Fast 236 + 2.

Opus review 2026-10-09: the knob, its self-checks and the job are sound
and could merge alone; the mixed-fault half needs another round because
the generator CANNOT reach 23.11's state for any seed or count, so the
clean runs were a false negative (main still has the bug: lane-1's `born`
scenario fails on unchanged code). Why: after a mid-sequence restart the
oracle's own content check (lib.sh `snapshot`, md5 of every file through
dcfs) sends a cold read-only OPEN that marks the ghost-to-be atime-dirty;
cutahead's kSync create makes the mark durable; RecoverDirty then
invalidates the row and the handle's LOOKUP(".") correctly gets ESTALE:
the check masks the bug. `fail3`'s trailing global `sync` commits the
create on the backing and closes path B by construction. The grammar
cannot say "backing drops writes from before the create" (every cut drops
both disks at once). Required: metadata-only checks after mid-sequence
restarts; `syncfs` of the cache only in fail3; a `dropahead` event;
handles taken automatically before every cut and crash (today ~73% of
sequences check no identity); the two 23.11 sequences pinned in the large
tier and shown failing on pre-fix code; a real-ghost guest fixture for the
oracle's "does not have" branch; event preconditions asserted (c3 held,
EEXIST); the identity comparison strengthened from inode+type to a backing
handle recorded at take time (recycled inode numbers defeat it) with the
check run before any walk; `fhtest stat` moved to testutil (fhtest.c is a
hand-synced third-party copy; merge with 23.11's handle-save/handle-stat);
cutahead's own directory; noisy_report's artifact name and the missing
`--runs_per_test_detects_flakes`. Verified: `fhtest stat` uses O_PATH, so
its answer is LOOKUP(".") -> EntryFor, the bug's symptom path, not an
open; crash3's hold is faithful (the backing create is not durable at the
kill); the job cannot go red except by its own breakage; `-quiet-only` is
last-wins if anything else sets `--test_tag_filters`. Sent back
2026-10-09.

Second round 2026-10-09: the generator reaches the state. Mid-sequence
checks are metadata-only (identity oracle first, then `fd_snapshot` after
`mixed_drop`; md5 only at the end); `fail3` syncs the cache only; a
`dropahead` event (backing drop-writes until the next cut, re-applied
after a freeze as `born` does); handles taken before every crash and cut
(22 of 22 sequences checked identity, 411 handles, 10-30 each); the large
tier is an explicit list of 22 sequences drawn once from seed 2610. On
unfixed code TEN of the 22 fail, identically on ext4, xfs and btrfs, each
with the ghost ("the handle of t/c3 opened inode 20, but the backing
filesystem's own handle of it answers ESTALE: an answer for something
gone"): the two pinned ones and eight that need only crash3/fail3, a
listing and the final cut (`replace listing crash3 listing`, `unlink
listing crash3 listing`, ...), so the bug's reach on main is wider than
the model's two paths; listed in `fault_ace_mixed.expected_failures` with
reason 23.11, strictly (a listed sequence that holds fails), so 23.11's
merge must empty the file. Also: the real-ghost fixture (rm behind the
daemon; the old `tamper` edited the wrong field and rejected nothing, now
fixed); preconditions asserted; the oracle compares the recorded inode and
type AND requires the backing handle taken at the same time to still open
to the same object, answers collected before any read, ESTALE for a
surviving object noted and counted; fhtest.c restored, `handle-save`/
`handle-stat` in testutil (prints inode and mode too; 23.11 adapts);
cutahead's own directory t/z; noisy_report's artifact name and
`--runs_per_test_detects_flakes`; the L items except the wchan check.
Merged 2026-10-10, 628af12 (four commits; clean rebase over 6.5; fast 236
+ 2, the three mixed targets with their ten expected failures, the
kernel-mode pair both ways, the host tests). The ten entries in
`fault_ace_mixed.expected_failures` are 23.11's to empty.

26.14 made the default guests deterministic (writeback off, one vCPU),
which also removed the noise that shakes out races: timer-driven writeback
landing mid-operation, real parallelism between the daemon's threads and
the kernel. A scheduled CI job puts it back on purpose:
- `noisy`, weekly (and on demand through `workflow_dispatch`): the plain
  full suite plus the ASan small/medium tiers, with the quiet-kernel
  sysctls left at the kernel's defaults, every guest at two vCPUs, and
  `--runs_per_test=3` for the small and medium tiers (what the time allows;
  sharded like the full suite). One knob in the harness selects it (a
  `cmdline` word that `guest/init` reads, or an environment variable
  run-qemu.sh passes through; one abstraction, no per-test flags), so the
  same test targets run in both modes and the test-result cache stays
  sound (the knob is part of the action key).
- A failure there is a finding, not a red push: the job is not required
  for the push gate, it reports like mutation-changed, and each failure
  becomes a deterministic test (pin the interleaving with the harness's
  holds or a fault point) before the fix, test first.
- Self-check: a target that fails when the knob is on and passes when it
  is off (the quiet_kernel_test inverted), so the mode is known to take
  effect on the runner.
Owner: dcfs-implementer, when a lane frees. Not to be confused with the
soak test (Phase 19, manual).

## 26.17 RAM-backed guest disks, three-part budgets, load-starvation flag (russ, 2026-10-09, approved)

1. RAM-backed virtual disks for the fault tests. The fault, ACE, freeze,
   power-cut and recover tests get their failure semantics from
   device-mapper (dm-flakey, dm-log-writes, dm-delay) and fsfreeze, not
   from the physical disk; the host disk contributes only latency, which
   is the noise we don't want. 12.14 already runs its log, replay and
   snapshot devices as loop devices over sparse tmpfs inside the guest and
   is sound. Add a harness option that backs a test's virtual disks with
   guest RAM (brd or a tmpfs-backed loop device) with the dm target and
   filesystem on top; default it on for the fault and ACE tests after
   measuring their memory (expect 256 MB guests to grow toward 768 MB to
   1 GB); keep dm-delay available where a test wants a known latency.
   Only the manual real-hardware phase (21.1) keeps a real disk.
2. Budgets in three deterministic parts, not wall time: daemon CPU
   seconds with a stated tolerance, I/O operations by kind (reads,
   writes, sectors, fsyncs and FLUSH/FUA from `/proc/<pid>/io`,
   `/sys/block/vdX/stat` and the strace budgets), and the existing request
   and syscall counts. I/O-count budgets are pinned per kernel pin like
   the strace goldens: a kernel bump may re-baseline them in its own
   commit with before/after, nothing else may.
3. Wall time stays only as the hang guard, set generously from CPU plus
   counted I/O times a pessimistic latency.
4. A load-starvation flag: when a test's wall time exceeds its CPU time
   by more than a few times, the harness marks the run "load-starved" in
   the summary (diagnostic only), so a timeout can be told apart from a
   hang.
5. 17.3 uses this: attribute the slow xfstests cases by daemon CPU
   seconds and fsync/write counts against the native run, not wall time.

Owner: dcfs-investigator for the measurement and the disk option,
dcfs-implementer for the budget files; after the current fix rounds
(15.6b, 17.1, 6.5) merge, since all three touch the same guest harness
files. The Gantt is updated when it is dispatched.

Merged 2026-10-11:
- 26.17a RAM-backed guest disks (3e0c778): loop over tmpfs, 13 fault targets,
  guest RAM sized from measurements, fault_recover silent pass found.
- 26.17b deterministic budgets (7b713f6, five commits a565c32..e073bc0):
  CPU time of dcfs per test held to cpu_budgets.txt with 100% tolerance;
  I/O operations of the disks and dcfs held to io_budgets.txt; wall time
  hang guard refused in run-qemu.sh if allowance is shorter than formula;
  load-starvation flag in summary when wall time stretches under load;
  non-SMT core pinning in run-qemu.sh (taskset -c <cpulist> parsed from
  sysfs thread_siblings_list, DCFS_PIN_NON_SMT=0 escape hatch, self-checks);
  xfstests cost lines and side-by-side native comparison (scripts/xfstests_costs.py,
  xfstests_costs_test, xfstests_notrun_test); xfstests_1_test_ext4 shard budgets
  calibrated. Full suites green: fast (299 pass), presubmit (444 pass), tools (38 pass).
- 26.17 non-SMT pinning off by default (927e8ca): Finding 6 from trial-2026-10-11
  applied; non-SMT core pinning is off by default and enabled only for budget-measuring
  runs (DCFS_PIN_NON_SMT=1); run-qemu.sh header, test/qemu/README.md, and
  run_qemu_cost_test self-check updated.
- 26.17c interim headroom for disk count budgets (russ 2026-10-18: "until that's
  done, add a small headroom right now so I can push and get CI green again");
  in progress, on branch fix-disk-read-headroom, not merged; removed by 26.41's
  deterministic counts (26.41 (6)).

## 26.18 End-to-end tests: assertions and oracles in C++ (russ, 2026-10-09, approved; start after the weekly quota resets, Friday 2026-10-16)

Premise: the unit tests are already googletest C++ binaries run in the
guests; the shell layer is the end-to-end scripts (mount through the
kernel, dmsetup, power cuts, systemd). This week's findings against shell
there: every poll that had to become an event (no poll()/inotify/pidfd in
shell; 6.5 fixed it by adding them to testutil in C); the identity
oracle's weakness and 12.14's awk variable bug (typed comparisons and
matchers in C++); busybox ash, musl getopt, awk's rand() across pins, an
unquoted `?` glob; and russ's crash-versus-assertion rule (25.9), which
C++ gives for free. Shell stays right for command glue (the systemd guest,
xfstests, pjdfstest, the wrapper tests).
1. A guest-side e2e library in `testonly/`: process spawning (from
   bench/process.cc), mount/unmount, dm targets, fsfreeze, power cut, the
   event waits, and the oracles (tree digest, identity by handle, dirty-set
   reads through `testutil sql`), reporting through googletest.
2. Port the two oracles and one fault scenario (the ACE mixed test) as the
   pattern; measure time and lines against the shell version.
3. Rule (style.md tests section): new end-to-end tests with comparisons,
   parsing or waits are C++; shell only for command glue with
   succeeded-or-not checks; lib.sh helpers migrate into the library as
   they are touched, as 6.5 did.
4. Decide on porting the rest from the measurement, not up front.
5. Structured results (russ): googletest writes a machine-readable report
   (`--gtest_output=xml:<path>` or `json:<path>`; Bazel already consumes
   the XML as `test.xml`): the harness passes the flag into the guest,
   copies the file out with the serial log, and the host side merges it
   into the test's `test.xml` so Bazel, the CI summary tools and 26.17's
   budgets see one check per testcase with its own time; the shell
   scenarios' `TEST x PASS/FAIL` lines get the same treatment (run-qemu.sh
   turns them into testcases) so breakdowns aggregate across both kinds.
Owner: dcfs-implementer for the library and the structured output,
dcfs-protocol for the oracles' port; after 26.17; not before the quota
reset.
Widened (russ, 2026-10-11, "let's minimize [shell's] use"): 26.18 is
the vehicle for converting the test guests' shell (113 scripts, most of
the tree's 26k shell lines) to C++ test binaries, and gains a host-side
counterpart: the `sh_test`s under tools/ and test/qemu (the toolchain,
run-qemu and gate self-checks) become `cc_test`s or, where they only
compare text, `py_test`s; `.github/ci/*.sh` stay as thin Bazel wrappers;
`test/qemu/guest/init` stays shell up to the point where a C++ binary
can run. Each conversion is a separate step by area, test-equivalence
shown (the same checks, the same failures on the same known-bad
fixtures), and the shell file deleted in the same commit.


## 26.16 Where CI time goes (2026-10-09)

No cold-run profile exists: `notes/build-speed-2026-10-07.md` predates the
pinned LLVM toolchain, and the per-job wall times show nothing inside the
Test step. Every CI job writes Bazel's `--profile` JSON (and `--execution_log`
if it is small enough) and uploads it as an artifact; a tiny host-side
tool (`tools/ci_profile.py`, unit-tested) sums the profile into fetch,
`@dcfs_llvm` extraction, third-party builds, our compile, test execution,
per job, and prints the table into the job summary. Then one deliberately
cold run (`workflow_dispatch` input that skips the cache restores) for the
full picture, recorded in a note. Check on the way whether the extracted
`@dcfs_llvm` (the output base) is among the restored cache paths; if not,
every job re-extracts it. Owner: dcfs-investigator, when a lane frees;
data arrives with the next push.
Done 2026-10-09, merged 86a3775: seven jobs plus osv record profiles
(`bazel-profile-<job>[-shard]` artifacts, with the compact execution log,
0.9 MB warm), `.github/ci/profile.sh` + `tools/ci_profile.py` put the table
(fetches, @dcfs_llvm, third-party builds, our compile, tests, critical
path) in each job summary; the `cold` input already existed (26.5) and
skips restores and saves. Answer to the cache question: NO, the extracted
@dcfs_llvm (`--repo_contents_cache` at ~/.cache/bazel-repo-contents, and
the output base's external/) is in no restored path, so every Bazel job
re-extracts it: about 14 extractions per push; locally 31.6 min under load
18-37 for a re-extraction from the repository cache, 54 min for the first
fetch. Local fast tier: 486 s wall, repository fetches 82 s, third-party
builds 148 s, our compile 76 s, tests 153 s, critical path 138 s
(dir_cache_fs_test 121 s). The first runner numbers arrive with the push.
- 26.16b Cache the extracted toolchain in CI (from 26.16): save and restore
  `~/.cache/bazel-repo-contents` (or only its dcfs_llvm entry) keyed on
  `MODULE.bazel.lock`, `third_party/llvm/*` and `.bazelversion`, saved on a
  miss only; entries are named by a hash of the rule's inputs, so a stale
  entry never matches. Cost: about 1 GB compressed (estimate) of the 10 GB
  quota. Decide with the push's profile numbers. Owner: dcfs-implementer.
  Done 2026-10-09, merged 4e21a50 (lane-2, one commit). The premise was
  wrong: Bazel's repo contents cache holds only http_archive/http_file
  repositories; `@dcfs_llvm`, the alpine_* repos, kernel_image and qemu
  are real directories in the output base. Cached instead: the output
  base's `external/+llvm_distribution+dcfs_llvm` plus its marker (the
  smallest unit, LLVM only; Bazel reuses a restored directory only when the
  marker's inputs match, so a stale entry costs a refetch, never a wrong
  build; verified locally with a sentinel file in a fresh output base).
  Key: hash of MODULE.bazel, MODULE.bazel.lock, .bazelversion and
  third_party/llvm/*, no restore-keys, saved on a miss, skipped under
  `cold`; prepare.sh exports `DCFS_BAZEL_EXTERNAL` (output base from
  md5 of the workspace path). Measured from the bc7eee7 profiles: the
  extraction is 199 s in fast, 456 s in presubmit, 263 s in coverage, 276 s
  in each reproducible build, and about 253 s hidden in each shard's
  `cquery` (unprofiled): about an hour of runner time per push, 7.6 min
  on the critical path plus about 4 min per shard. Size 0.58 GB zstd.
  `reproducible` (own output bases) and `osv` left out. Caveat: Alpine pins
  by branch, so an upstream change to a package the extraction uses makes
  the marker mismatch and the stale entry stays (slow, not wrong); a date
  component in the key if the profiles show it. Check on the second push
  after the merge: the `@dcfs_llvm` row near 0 in fast/presubmit/coverage,
  shard cquery seconds instead of 253 s, restore hit, save about 0.58 GB;
  a "Path Validation Error" would mean the `+`/`@` in the paths broke
  actions/cache's globbing (the one thing not verifiable locally).

## 26.14f Coverage artifacts in the daemonisation path (CI run 37973594236, 2026-10-09)

The first coverage run after 26.14c's gate flagged six branch counts as
artifacts and failed the job: dcfs/main.cc:674, 685, 686, 690 (counts
4294967562, 4294967430, 4294967421, 4294967546: 2^32 + a few hundred) and
dcfs/startup_channel.cc:110, 111 (8589934575, 8589934574: 2*2^32 + a few
hundred). Not the -1 underflow 26.14c saw: these are a 32-bit wrap plus a
real count, in the code that forks and daemonises (15.2), under
continuous-mode profiling with `-runtime-counter-relocation` and atomic
counters. Hypotheses: the parent and the forked child both own the mmapped
profile and one re-initialises the counter relocation bias; the helper
that execs /bin/mount in the private mount namespace writes the same
profile; a profile written by a process that exits through `_exit` after
fork without the runtime's own flush; two processes of one binary
merging with inconsistent region counters. Investigate as 26.14c was
(reproduce on `mount_dcfs_test` under coverage with DCFS_KEEP_PROFRAW,
per-process profiles with `%p` to see which process contributes the wrap),
fix in the harness or the profile naming, never by widening the gate.
Until then the coverage job is red on every push that runs mount_dcfs under
coverage; the push rule (fast dev) tolerates it, the gate did its job.
Owner: dcfs-investigator, next free lane after 15.6b.
Done 2026-10-09, merged 6d74ef5 (lane-5, one commit). Cause: `ForkDaemon`
returned in both the wrapper and the daemon; under continuous-mode
profiling the two processes share one mapped profile, so the code after
the fork counted twice per entry (function count 22, post-fork blocks
44) and llvm-cov's flow-conservation derivation of a branch count went
negative (2^32 - n); Bazel's lcov merger then added other tests' real
counts on top, hence 2^32 + a few hundred, and 2*2^32 where two tests
carried it. A 25-line fork program reproduces it. Not a corrupt profile:
every raw file reads cleanly; `%p` cannot split a fork child in
continuous mode (the mapping is inherited). Fix: `dcfs/fork_split.h`
`ForkSplit(child, parent)`, the one function that returns twice, marked
`no_profile_instrument_function`; `ForkDaemon` returns only in the child
and the parent exits inside `AwaitReportAndExit`; behaviour unchanged.
Guards: `scripts/cov-lcov.sh` fails a single test whose lcov has a count
>= 2^31 (so it no longer hides in the sum), `coverage_pipeline_test` runs
a fork fixture both ways (twice refused, split accepted with each side at
1, failing first), `startup_channel_fault_test` forks a wrapper per case.
Cost: nothing (profile +352 B). Left: `backing_capture.cc`'s two forks and
`bench/process.cc` under-count the parent leg of their `child == 0`
branch (never negative); a follow-up can use the same primitive.


## 26.20 CI triage of run 38018425705 (merged 2026-10-10, 9fd3ff2)

Three shard failures on f616181, all test or harness bugs, no dcfs bug
(investigator, lane-1, three commits, fast suite green, 16-run soaks of
each fixed target):
- `destroy_test` under asan: `uptime_ms` built milliseconds as a string
  (`0.93` became `0930`, invalid octal under busybox arithmetic) and the
  asan daemon read it while the guest was under a second old. Now one
  `uptime_ms` in `lib.sh`, decimal arithmetic, five fixed-value cases in
  `boot.sh` that failed first.
- `nfs_test` under asan: not a stale cached size. The held descriptor's
  read got ESTALE because `restart_daemon` ran `exportfs -f` as soon as
  the mount appeared in the table, before the asan daemon served its
  first request (0.7 s later); the restarted daemon never saw the file.
  A `stat` of the mount, which blocks until dcfs replies, precedes the
  flush (an event, not a timer). 4 of 16 runs failed before, 0 of 16 after.
- `fault_ace_fs_test_xfs` ace-dsplit (plain and asan): since 23.11 took
  the WAL fsync out of a create, xfs sometimes committed its log on its
  own between the persistence point and the cut, so the operation after
  the point (or a create with no point) was on the disk: the fixture's
  premise, not a dcfs property, failed. Bisect: 0 of 8 at 5dcec18, 3 of
  12 at 61f6a02. After each persistence point the backing now drops its
  writes (dm-flakey `drop-writes`, as the mixed sequences' `dropahead`
  already does), and the negative fixture drops before its create. The
  cut, restore and comparison are unchanged. 0 of 16 after. Open: which
  xfs path forces the log was not identified (`/proc/fs/xfs/stat` force
  counts did not separate the runs); the fix does not depend on it.

## 26.21 CI triage of run 38025358346 (merged 2026-10-10, ca65a1c)

Two shard failures on 8f528d8, both harness, no dcfs bug (investigator,
lane-1, one commit):
- `fault_power_kill_test_ext4` atime check: the snapshot helper read every
  file with md5sum and assumed the backing snapshot's read made the one
  relatime update. A power cut's second boot starts its clock from the
  RTC's whole second and can be behind the ctime of files the first boot
  made, so relatime moved the atime at every read, including both
  snapshots' (CI: the served tree read 35 ms after the backing tree). The
  snapshot now reads with O_NOATIME through testutil (`readnoatime`
  became `catnoatime`, one mechanism), so no snapshot moves an atime, for
  all four snapshot users; a self-check puts the clock back and takes two
  snapshots, which failed first. The natural failure was never reproduced
  locally (0 of 8 plus three instrumented batches); CI's faster guest
  reached the snapshot at 0.8 s, which is the inferred margin.
- `stress_short_test_btrfs` under coverage, `no-reclaim`: the coverage
  initramfs is 54 MiB against 38 plain (instrumented binaries), the page
  cache the same 108 MiB of file data in both, so a 216 MiB guest sat at
  175 MiB cached near kswapd's watermark; 872 pages scanned once in CI,
  0 of 4 locally. `mem = 320` for stress_short with the measurement in a
  BUILD comment; lowest MemAvailable then 160 MiB, 8 of 8 clean.

## 26.17a merged 2026-10-10 (3e0c778): RAM-backed guest disks

Part 1 of 26.17 (investigator, ten commits, Opus review plus one fix
round). `ram_disks = True` on `qemu_test`/`qemu_test_matrix`: the
guest makes each disk as a sparse file in a capped tmpfs, formats the
file (not the device: formatting the loop device left ext4's inode tables
un-zeroed and ext4lazyinit became a spontaneous writer on every mount),
attaches it as a loop device marked rotational (so btrfs and ext4 keep
the allocator the virtio disks had), and mknods `/dev/vdN` as a second
node. The pinned kernel has no brd. On for 13 fault targets (selftest,
backing, cache, power, shutdown, ace a/b/fs/mixed/mixed_long, freeze,
recover, recover_btrfs_unpinned); `mem=` set from measured peaks
(384-576 MiB plain, 512-832 asan; mixed_long's a flagged guess). Plan
drift, recorded: `fault_power_kill_test` stays on host images (the host
kills QEMU and a second boot reads the disks; the macro refuses RAM
disks with power_cut, boots, rootfs, systemd_image, mke2fs options, a
disk named vda, or no explicit mem); the enospc tests stay on images
(they fill the disk); `DCFS_RAM_DISKS=0` (validated, like DCFS_SEED)
runs any converted test on host disks; run-qemu.sh prints a time line
(guest wall, host CPU around QEMU only) for 26.17 part 4. Measured: the
I/O-heavy ACE targets 25-30% faster on a quiet host, and under host disk
load fault_ace_b 72.6 s → 37.6 s; freeze and recover unchanged (their
waits dominate). One mechanism: sqlite_durability shares the attach
helpers; `device_sectors` replaces two size helpers; `sectors_read`
resolves by node and stops loudly when the counters are absent, which
exposed a silent pass in fault_recover.sh (`DM_NAME` was always empty
without udev, so every fill_fails pair compared empty and every
answered-from-cache lookup was a SKIP; now real PASSes). Not run:
`bazel coverage` with RAM disks (the coverage CI job is the check). The
reviewer's "does a loop device over tmpfs change what the tests prove"
answer was no: every cut is dm-flakey plus a remount, never device
durability.

## 26.22 CI triage of run 38030966315 (merged 2026-10-10, 010ba9a)

Investigator, lane-3, three commits. (A) coverage: not the harness
changes I guessed; dcfs/fsck.cc (15.5, 478 lines) was 291/372 lines
covered, deterministically, because the systemd mount test that reaches
findmnt/blkid/fsck(8) is large and the coverage job runs small and
medium; plus error paths with no test. Fix: fsck_test gains stand-in
findmnt/blkid/fsck.fakefs programs asserting their arguments, corrupt
database fixtures (orphan pages, dropped tables and columns, an
exclusively locked WAL, a symlink), and --wrap fakes of fork/waitpid/
dup2/flock/unlinkat; one production simplification (Inspect reads the
dirty set in one pass; the old count and clean conjuncts could not fail
once earlier queries succeeded, 1.10a). fsck.cc now 380/380 lines,
184/196 branches; the baseline should land near 96.29/80.83 (the CI run
decides; baseline file untouched). Also the collect-logs cp failed on
157 files, not two: `cp --parents` re-protects each directory to the
read-only source mode after the first file; `--no-preserve=mode`,
with a test. (B, C) the four xfstests cases all failed at the mount
after an umount with "cache database is in use by another dcfs
process": the xfstests guest never mounted fusectl, so
umount.fuse.dcfs did not wait for the old daemon and the new one lost
the database flock; the idle 17.1 runs won the race, CI's loaded
runners lost it (3 of 12 shard runs under load before, 24 of 24 and 12
of 12 after mounting fusectl in the guest; a guest check fails without
it). No list entries. Design question for russ: a mount right after a
plain umount still fails on a machine without fusectl or without
umount.fuse, because the new daemon takes the database lock before
HoldDaemonLock's earlier-daemon wait. (D) btrfs shard 5's hour: during
generic/650 (fsstress with CPU hotplug on a two-vCPU guest) the daemon
sat in D state inside the backing btrfs's own commit
(write_all_supers ← btrfs_commit_transaction ← syncfs) waiting for
block I/O, the watchdog killed the test, and the next batch's mkfs on
the other disk hung too: the guest's block I/O stopped, not a dcfs
request. Not reproduced (650 alone 48 of 48; the shard 12 of 12). No
change, no list entry (excluding 650 would be weakening). Follow-ups
for russ: bound the scratch reset as the watchdog bounds tests, and
dump the guest's blocked-task state when the watchdog fires; both
involve a timer, so they wait for him. Lane-3 has a leftover
`.scratch/` tree (377 MB, a 0555 directory inside) that the classifier
would not let the agent remove.

## 26.23 Blocked-task diagnostics in the guests (russ, 2026-10-11; B of the btrfs-hang follow-ups)

From 26.22's finding D (the btrfs shard's hour: block I/O stopped under
generic/650's CPU hotplug; not reproduced in 60 runs). russ: do B, not
A as submitted code. (1) When xfstests.sh's watchdog fires, dump the
blocked tasks' kernel stacks into the serial log (sysrq-w via
/proc/sysrq-trigger, and /proc/PID/stack for every D-state task) before
killing the test; the same at the host-side kill if the guest is still
answering. (2) Turn on the kernel's hung-task detector in the quiet
guest kernel (hung_task_timeout_secs 120, panic off) so a stall prints
its stack by itself; check the quiet-kernel idle test still shows no
spontaneous activity. (3) A failing-first check for (1): a fixture that
parks a process in D state is hard; a fixture that puts a task in S with
a known stack and asserts the dump contains it is the test. The agent
may bound the scratch reset or add any timer while reproducing or
debugging (style 1.11's working-tree clause), but no such code is
submitted; generic/650 is not excluded. Owner: dcfs-investigator, after
the current 25.23 round, within the pace.

## 15.10 fusectl in the guests that remount; one test without it (russ, 2026-10-11)

russ's decision on the mount-after-unmount question: keep the behaviour,
document it (README, done 2026-10-11), mount fusectl in every test guest
that unmounts and mounts the same cache in sequence (xfstests done in
26.22; audit the fault, freeze, recover, power, nfs and lifecycle guests
and mount it where a remount follows an unmount, in guest/init or the
script), and keep at least one test that proves dcfs works without
fusectl (mount_dcfs.sh's no-fusectl case: name it in the README and the
gates table so it is not removed). The limitation is removed by kernel
patch 6 (synchronous FUSE_DESTROY), noted in kernel-patches.md. Owner:
dcfs-mechanical.

## 26.24 CI triage of run 38069307659 (merged 2026-10-11, 67652b7)

`ubsan (1)`: `fault_ace_mixed_test_xfs` failed the remount after the cut
in 7.7 s. Not memory (ubsan got the 640 MiB asan allowance; no OOM), not
the RAM disks (a deterministic variant fails on host disks too), not
dcfs: a harness race specific to xfs. xfsaild passes about every 50 ms
and forces the log for an item the operation changed; with `dropahead`
the disk drops that log write while xfs counts it done; the healthy
freeze window that follows then writes metadata stamped with the dropped
record's LSN, and after the cut xfs refuses the mount ("Metadata has LSN
ahead of current LSN", EINVAL). A real power cut cannot leave that state,
so the harness was constructing an impossible disk. Mitigation merged:
`mixed_quiesce_backing` (a healthy freeze and thaw before the operation)
empties xfs's AIL so xfsaild has nothing to force in the window;
measured with temporary ftrace in the working tree: 10-11 of 12 runs
failed before, 0 of 12 after; 16 of 16 under a 4x oversubscribed host
(1 of 16 failed before). Not a full cure (parked, oracle soundness,
26.25): a `sync` event before `dropahead` or xfs's 30 s log worker can
still put a log write in the window; the lasting fix for xfs is an image
copy at `dropahead` restored at the cut instead of drop_writes plus a
healthy freeze (freezing on the dropping disk was tried and fails:
drop_caches evicts clean buffers that never reached disk). Also observed,
not fixed (26.25): "dropahead then sync" sequences fail the identity
oracle on ext4 and btrfs with ESTALE, because the dropping disk
acknowledges fsync without keeping it (a disk that lies about fsync, so
not a dcfs bug; the manual long test can draw them). Diagnosability
(26.23): a dmesg tail when `fd_mount_backing` fails would have named the
cause in CI, whose serial log had no kernel lines. Coverage baseline
bumped to 96.26 / 80.85 as the gate printed.

## 26.25 The ACE harness's dropahead window on xfs (parked; oracle soundness)

From 26.24: for xfs, replace drop_writes plus a healthy freeze with an
image copy taken at `dropahead` and restored at the cut; and decide what
"dropahead then sync" means (a disk that lies about fsync is outside the
model's premise; either the generator never draws it, or the oracle
treats the sequence as a disk fault rather than a dcfs one). Protocol
agent (it is the oracle's premise). After the trial.

## 26.27 Coverage gate on regions, not lines (russ, 2026-10-11: "coverage counts lines? It doesn't count C++ statements?")

The gate sums lcov lines, which formatting changes (the 7.6a reformat
moved the figure 0.22 points with no logic touched). llvm-cov's
source-based coverage counts regions (statements, expressions,
conditional arms) and reports them directly. The gate's two gated
numbers become regions and branches, read from `llvm-cov report`/`export`
rather than the lcov line export; lines stay printed as information; the
baseline file holds regions and branches, seeded from the first run
after the change; `docs/coverage.md` and the gate's self-check test
updated; the 26.14 coverage_diff tooling checked to work on regions.
After this a reformat cannot move the gate.
Finding (2026-10-11, the mechanical agent stopped without changes,
rightly): only lcov leaves the guest (run-qemu.sh's cov-lcov.sh converts
each test's profiles in the guest), and lcov carries no regions; a region
figure needs one profdata merged across all tests on the host, then
`llvm-cov export`, since per-test JSON totals would double-count shared
regions. So this is a pipeline change: per-test profraw or profdata as
test outputs, `llvm-profdata merge` in .github/ci/coverage.sh, JSON
export, totals.regions and totals.branches for the gate; lcov kept for
the per-line reports and coverage_diff. Investigator, not mechanical.
Until then the lcov gate stays (lines and branches) with the reformat
case documented in docs/coverage.md: a formatting-only change that moves
the line figure is re-baselined with the 26.26 alignment check as the
evidence. Parked in the Queue's item 6 unless russ pulls it forward.

## 26.28 README badges from our own numbers (russ, 2026-10-11: "Do it"; dispatched, mechanical, lane-4)

The two workflow badges and three static ones are in the README
(7742cae; the man page extracts named sections, so none reach it). This
step: shields.io endpoint badges fed by JSON files that CI writes to a
`badges` branch of this repository (russ approved CI pushing to that
branch; nothing else in the project pushes): coverage (lines and
branches from the gate's line; regions when 26.27 lands), reproducible
(bit-identical or not, from the reproducible job), OSV (known
vulnerabilities and ignores with their expiry, from the osv job),
mutation score (killed of total, from the weekly job's merged report),
and TLC (states explored and violations from the weekly large
model-checking run). Each job writes its JSON as a step; one final job
per workflow commits the changed files to `badges` with the workflow's
token (`contents: write` on that job only, least privilege elsewhere),
skipping the commit when nothing changed; the README references
`https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/<owner>/<repo>/badges/<name>.json`.
A test for the JSON writers (fixtures of each job's output line, the
shields endpoint schema: schemaVersion 1, label, message, color). No
badge for a number we do not already compute.

## 26.33 names_random.sh has never run past line 118; `sh -n` for guest scripts (from 26.31) (merged 2026-10-17, 33e90ba)

26.31 found that test/qemu/guest/names_random.sh ends direction 2 with
`exit "$FAILED"` followed by `fi` and a second copy of the mounted
section: a parse error after the exit, so busybox sh stops there and
everything after it never ran. Nothing checks guest scripts' syntax.

1. A host-side gate: every `test/qemu/guest/*.sh` (and any other shell the
   guests run) passes `sh -n` under the guest's shell (busybox ash from the
   pinned Alpine packages, if it can run on the host through Bazel; else
   dash or bash --posix, saying why), as a small test. Show it failing on
   names_random.sh first.
2. Fix names_random.sh: decide from git history what the script was meant
   to run (the duplicated section is probably a bad merge), keep one copy,
   and run names_random_test on ext4, xfs and btrfs. Anything the newly
   reached code finds is a real result: report it, do not weaken it.
3. Fix any other script the gate flags.

Implementer.

## 26.34 Load-sensitive tests (triage, from the 2026-10-17 log) (merged 2026-10-17, 6aa8df5)

Tests seen failing under host load and passing on rerun, each before the
change that hit it: fault_backing_test (I/O budget), enospc_cache_test
(`sync-listing`, flagged load-starved, twice), request_counts_test (disk
writes 62 to 70 once, then 52, 55, 54), backing_test (CPU budget at 51%
CPU pressure). For each: reproduce with a targeted `--runs_per_test` under
competing load, find whether the test measures something load really
changes (wall time, scheduling) or something it should not (a count that
depends on writeback timing or a race in the test), and fix the cause:
count what is deterministic, or make the budget robust by measuring the
right quantity. Never widen a budget or add a retry to make it pass
(AGENTS.md; no retry loops). A real dcfs race found this way is a result,
not a nuisance. Investigator.

Outcome: enospc_cache lists with `testutil readdir-ino` (busybox `ls`
hid a getdents ENOSPC as an empty listing); `reads` and `writes` in
`io_budgets.txt` count requests issued (completed plus merged), and the
re-measured counts no longer move with load. Follow-ups:
- xfstests_1_test_ext4's budget lines await a whole re-measure; its
  `reads` and `writes` are still completed counts.
- backing_test's CPU budget is not trustworthy on a load-starved host
  (100 runs: 0.26 to 1.0 s against 1.02 s). Options are with russ: do not
  hold CPU budgets on runs `run-qemu.sh` flags as starved (recommended),
  or calibrate in the guest.

## 26.35 CI run 39000396656 (58a1f17): causes, and the mutation job's missing base (merged 2026-10-17, 8b46484)

The first CI run after the 2026-10-16 trailer rewrite and force-push failed
five ways. Three were already fixed on main when this step started
(the failures are in the run's log; each test passes on main, run here):

- `mount_dcfs_systemd_test` (full, asan, ubsan): the systemd fixture's
  nested bind line named SOURCE=/srv/raw, which has the raw mounts below it,
  and a grep for the old ro message. 15.11's capture deletion (95dafad)
  introduced both; fixed by 8184e510. Passes on main (31 s here).
- Every `xfstests_*` shard: xfstests passed the device as SOURCE
  (`openat(-100, /dev/vdb): Not a directory`); 15.11 made SOURCE a directory.
  Fixed by 2abffe7a (26.30: native mounts, a directory served from each).
  `xfstests_1_test_ext4` passes on main (171 s here).
- The coverage gate (lines 94.71, baseline 96.50): 25.9's uncovered poisoned-
  backing guards and 26.7's plain failures; fixed by 50645bbc (26.30), which
  raised the baseline to 96.95 / 82.90 (since 17.2: 83.25).

The fourth, the `mutation-changed` job, was the push itself: `github.event.before`
was d215c09, the old tip, which the force-push replaced (it exists only in
`backup/pre-trailer-rewrite`), so `git diff d215c09..58a1f17` in the clone
exits 128 even with `fetch-depth: 0`. `mutate.py changed` now treats any base
the checkout does not have as the tip against its parent, which subsumes the
all-zero base of a new branch (one mechanism, not two). Test first:
`mutate_test` failed with `CalledProcessError ... 'd215c09...HEAD' ... exit
status 128`.

Why presubmit and the merge did not catch the first three: `mount_dcfs_systemd_test`
and the xfstests shards are `large`/`enormous` (159 of the 597 tests are),
which only `bazel test //...` in CI's `full` job runs after the push;
`--config=presubmit` (the merge gate) is small and medium. The coverage gate
runs in presubmit's tier but only in CI (`coverage.sh`). A merge that touches
what a large test depends on is therefore first tested on origin. A
pre-push run of the large tests a change reaches was proposed and declined
(russ, 2026-10-17: "No. Until we ship, a red CI is not a problem."): CI is
where the large tier runs, and its failures are fixed forward.

## 26.36 Load-agnostic tests: CPU budgets, a PMU, and an audit (russ, 2026-10-17) (merged 2026-10-17, 816f150)

russ, on 26.34's backing_test CPU budget: "yes. But in general for all our
tests I'm interested in them being load-agnostic." The first half is the
rest of 26.34; the second is a rule and an audit.

1. **backing_test's CPU budget (26.34's open item, option 1).** A CPU
   budget is not held on a run `run-qemu.sh` flags as load-starved (host
   pressure 20% or more): the number is printed with the reason, as
   `budget_unheld_reason` does for sanitizer, TCG and coverage runs. Test
   first: a canned starved run whose CPU exceeds the budget fails today.
2. **Instructions instead of CPU time (investigate, then decide).** Can a
   guest see a virtual PMU (KVM, `-cpu host`), here and on CI's runners?
   If so, measure dcfs's instructions retired (user and kernel,
   `perf_event_open` in a small testutil reader, or `perf stat`) for the
   targets in `cpu_budgets.txt`: their spread across quiet and loaded
   runs (load 20+) against CPU time's. If instructions are steady, propose
   switching the CPU budgets to them, falling back to unheld where no PMU
   is visible; the switch itself is a follow-up step. For CI, add a probe
   that reports whether the PMU is visible (red CI is fine until we ship,
   russ 2026-10-17), rather than guessing.
3. **The audit.** Every place a test's verdict depends on wall time or host
   speed, in `test/qemu/` (guest scripts, `run-qemu.sh`, budgets), `dcfs/`
   unit tests, `bench/` and `tools/`: `sleep`s, `timeout N`, time bounds
   asserted (e.g. cancel_test's 2 s KVM / 8 s TCG), time-based budgets,
   allowances, retries on slowness. Classify each: (a) a hang guard only
   (generous, scaled by `run-qemu.sh`'s allowance; acceptable), (b) a
   verdict on time (must become a count of events, or move behind (a)),
   (c) a sleep standing in for an event (a 1.11 violation; e.g. 26.34's
   `sleep 3` against `sync_interval_sec=2`). Write the list to
   `docs/plan/notes/load-agnostic-tests-2026-10-17.md` with a proposed fix
   per (b) and (c) item; fix the cheap ones here, test first, and brief
   the rest as numbered follow-ups in this phase.
4. **The rule.** Add to `docs/style.md` section 2 (Tests), with russ's
   words: a test's verdict does not depend on host load or speed; it
   counts events (syscalls, requests, I/O, instructions), waits on
   events, and uses time only as a hang guard sized by the harness.

Owner: dcfs-investigator. Never widen a budget or tolerance to pass;
never weaken a test (AGENTS.md).

**As built (26.36, 2026-10-17).**
1. A run `run-qemu.sh` marks load-starved is not held to its CPU budget (the
   cost line says `not held to the budgets: host load-starved`); the I/O
   budgets stay held. `run_qemu_cost_test` now reads a fixed pressure file in
   every run: with the host's own pressure the flag now decides a verdict, and
   the test failed once on a real host's pressure before that.
2. The PMU: the guest sees a vPMU here with the existing `-cpu host`. User
   instructions retired spread 0.01 to 0.8% on a quiet host and 0.01 to 2.1%
   loaded, against CPU time's 12 to 28% and 49 to 326% (README, "Instructions
   instead of CPU time"). Probe: `testutil pmu`, `//test/qemu:pmu_test`, CI step
   "Report the PMU" (`.github/ci/pmu.sh`, `//tools:pmu_report_test`).
   Recommendation in the README; the switch is follow-up A below.
3. The audit: `docs/plan/notes/load-agnostic-tests-2026-10-17.md`. 63 listed
   sleeps: 1 (a), 5 (b), 46 (c), 11 (d); plus 6 (b) verdicts on time outside the
   list. 13 of the (c) fixed (`tools/repo_shape_sleeps.txt` 63 to 50), with
   `testutil waitexit`, `testutil waitpath` and `lib.sh` `wait_exit`, which replaced a python pidfd
   wait of `mount_dcfs_systemd.sh`.
4. `docs/style.md` section 2: the rule, russ's words, the hang-guard ruling
   ("Keep it until we replace with non-load-sensitive tests.") and the
   re-measure ruling ("Agreed").

**Proposed follow-ups (from 26.36).**
- A. Now in 26.43 (3), with B to H: `instr_user` CPU budgets.
- B. Now in 26.43 (3): sync points on demand for the sync-interval sleeps.
- C. Now in 26.43 (3): `cancel.sh` by counts of backing requests.
- D. Now in 26.43 (3): `readdir_boundary.sh` by instructions.
- E. Now in 26.43 (3): the fault windows by events.
- F. Now in 26.43 (3): probe, settle and quiesce waits by counters.
- G. Now in 26.43 (3): `nfs.sh` and `init` waits.
- H. Now in 26.43 (3): `xfstests.sh`'s `LIMIT` from the allowance.
- I. Now in 26.41 (6): the disks' I/O budgets become deterministic counts.

### Follow-ups from the 25.34 fix round (clerk batch 12, 2026-10-18)

- **`write_test_ext4`'s I/O budget is load-sensitive.** Now in 26.43 (2).
- **Suspected cause: the wall-clock sync interval.** Now in 26.43 (2), with
  25.28c's CLOCK_MONOTONIC interval as the link.
- **`pmu.sh` in the shell list.** `test/qemu/guest/pmu.sh` (from 26.36) was
  added to `tools/repo_shape_guest_shell.txt`, a list that should only
  shrink: port it to C++. Owner: dcfs-implementer, after 25.34 merges.

## 26.38 The cache database's permissions are the administrator's (russ, 2026-10-17; queued)

russ, on the Gemini finding that dcfs "tightens" a stricter 0400 database
up to 0600: "I changed my mind about this. If we create the file, make it
0600 (via our umask). But if the administrator opens it up, don't step on
their choice. That should mean that you can just delete all the file
permissions checking code."

- dcfs creates the cache database (and SQLite its `-wal`/`-shm`) 0600 and
  its directory 0700, through the umask it sets before creating them.
- Deleted: the fchmod to 0600 and its WARNING, the refusal of a database
  (or `-wal`/`-shm`) that grants more than the source's root directory,
  and the warning for a group- or world-accessible directory, with their
  tests, README rows (`dcfs.cache_db`, the log table) and design.md text
  ("OpenHardenedCacheFile").
- Also deleted (russ: "Gone. The questionable security is not worth the
  code."): refusing a symlink or a non-regular file (`O_NOFOLLOW`, the
  regular-file check). WAL mode's check stays (it is correctness).
- Test first: a database the administrator made 0644 or 0400 is left as it
  is (fails today: it is changed or refused); a new one is 0600 and its
  directory 0700.

Owner: dcfs-implementer, effort low.

## 26.39 Duration flags, and the sync interval from the kernel's writeback (russ, 2026-10-17; queued after 25.28b)

russ: "Abseil flags support an absl::Duration type, meaning we can have
`--attr_timeout=1h` instead of `--attr_timeout_sec=3600`. Please make that
change." And: "Is 3600/3600/5 the right defaults? Maybe for
--sync_interval_sec it should be set to the kernel's writeback time at
start, or something?"

- `sync_interval` becomes `ABSL_FLAG(absl::Duration, ...)` (`attr_timeout`
  and `entry_timeout` go away in 26.40 instead) (mount options `dcfs.attr_timeout=1h`
  and so on); the `_sec` names go (nothing has shipped). 25.28b's refusals
  (non-positive, NaN, infinite) carry over to what a Duration can say
  (zero, negative, `inf`). README's option table, the man page, the mount
  helper's pass-through, the guest scripts and fstab fixtures that set the
  old names, and their tests.
- Defaults: `sync_interval` from the kernel, approved (russ: "Do it.").
  Attr and entry, pending an inventory (the dashboard's questions table):
  stay 1h (all changes go through the mount, so the kernel's caches stay
  right; shorter only costs round trips). `sync_interval` defaults to the
  kernel's `vm.dirty_expire_centisecs` read at start (30 s on a stock
  kernel), the age at which the kernel itself writes dirty data back, as
  dcfs already reads `vm.dirtytime_expire_seconds` for access times; the
  flag overrides it. A sync point then never forces the backing's
  writeback (syncfs) much sooner than the kernel would have.
- **Following the sysctls while running** (russ, 2026-10-17: "Yes"): an
  inotify watch on `/proc/sys/vm/dirty_expire_centisecs` and
  `/proc/sys/vm/dirtytime_expire_seconds`, in the session loop beside
  25.10's signalfd and 25.28c's timerfd; on `IN_MODIFY` dcfs rereads the
  value and uses it from then on (unless the flag set the interval
  explicitly). First, the failing-first guest test verifies the premise:
  `sysctl -w` (a `write(2)` through `vfs_write`) raises `IN_MODIFY` on
  procfs. If it does not, report it and keep the startup reading only.
  A watch that cannot be added is logged once at WARNING (russ,
  2026-10-17), and dcfs keeps the startup value.
- Test first: `--attr_timeout=90m` parses and reaches the FUSE replies;
  the old names are refused as unknown; the default sync interval follows
  a changed `vm.dirty_expire_centisecs` in the guest.

Owner: dcfs-implementer, effort medium (the default touches 25.28b's
bound and its model's Interval; check the model's comment still holds).

## 26.42 Warn about FUSE request timeouts (russ, 2026-10-18; queued, M2)

russ: "Maybe also LOG(WARNING) at startup if it's set."

- dcfs never sets a FUSE request timeout. A timeout is a timer that aborts
  the connection when a request runs long, so a disk spin-up would abort
  the mount. dcfs keeps it that way.
- Read the system-wide sysctls at startup: `/proc/sys/fs/fuse/
  default_request_timeout` and `/proc/sys/fs/fuse/max_request_timeout`.
  Verified in the kernel tree (v7.3-rc4-607-gd530980dfbb7): both are in
  `fs/fuse/sysctl.c:23-50`, registered under `fs/fuse` by
  `fuse_sysctl_register` (`fs/fuse/inode.c:2089`), range 0 to 65535, with
  the default 0 (`fs/fuse/req_timeout.c:21`, `:29`). They first appear in
  the v6.19 tag (earliest tag containing them; the adding commit was not
  pinned, and the step's author should pin it).
- If either is non-zero, LOG(WARNING) once at startup, naming the value and
  the risk: a timeout shorter than the disks' worst-case response (spin-up
  included) aborts dcfs mounts.
- Start-only: the check runs at startup and nowhere else. No inotify watch
  and no reread (russ, 2026-10-18: "It's not worth an inotify watch just
  to log a warning."); the README says the warning is checked at start.
- README: warn that a value shorter than the disks' worst-case response
  (spin-up included) aborts dcfs mounts; recommend leaving both unset.
- Test first: a guest test with the sysctl set gets the WARNING, and a
  guest test with both at 0 gets none.

Owner: dcfs-implementer, effort low.

## 26.37 CI run 39046531637 (3441f89): the asan shards and xfstests_1 (2026-10-17) (merged 2026-10-17, 233baa3)

The first CI run after 12.18b, 26.30 and 26.34 failed in two places.

1. **All three asan shards: 55 guests and `//dcfs:backing_test`.** 55 guests
   died with `Initramfs unpacking failed: write error` and then `No space left
   on device` for every file `guest/init` and the scripts wrote. Nothing was
   full on the runner or in the cache or backing disks: the kernel unpacks the
   archives into the rootfs, a tmpfs capped at half of the RAM free when it
   is mounted, `MemTotal` less the compressed archives, which are still in RAM
   then. At the default `E2E_ASAN_MEM` of 384 that cap is 145,800 kB; the
   shared asan initramfs and a btrfs modules archive unpacked to about 138,000
   kB before 12.18b and to 147,400 after (its SibylFS binaries and suite are
   9.6 MB in every e2e initramfs), and `peak_shmem` stopped at 143,300. Only
   asan: the e2e initramfs unpacks to 46 MiB plain and 105 MiB under ubsan
   (both passed in this run) and 134 MiB under asan. 12.18b sized its own
   guests (asan_mem 1152) and not the others it grew. Reproduced locally:
   `bazel test --config=asan //test/qemu:rename_test_btrfs` died the same
   way ("-m 384 MiB is probably too little for the initramfs"). Fix:
   `E2E_ASAN_MEM` 512 (cap 209,400 kB against 155,700 for the largest modules
   set), and the rule in test/qemu/README.md, "Guest memory". The unit guests
   (`qemu_cc_test`, per-test initramfs) are unaffected.

   `//dcfs:backing_test` was another failure: 1.027 s of CPU against a budget of
   0.509 s with the 100% tolerance, in a run flagged load-starved. The budgets
   are not held for a sanitizer run (`budget_unheld_reason` reads
   `dcfs_sanitizer=` from the guest's command line), but `qemu_cc_test` never
   passed `--cmdline dcfs_sanitizer=...` as `qemu_test` does, so an asan or
   ubsan unit test was held to the plain build's budget since 26.17, and
   passes only while the host is fast. `qemu_cc_test` now passes `sanitizer_args()`
   (one helper for both macros); `//dcfs:sanitizer_cmdline_test` fails
   (`/proc/cmdline` is `console=ttyS0 ... dcfs_accel=kvm`, no
   `dcfs_sanitizer`) without it, under asan and ubsan. 26.36's item 1 (not
   holding CPU budgets on load-starved runs) is still open and is the other
   half of this test's fragility.

2. **`//test/qemu:xfstests_1_test_ext4`: a budget failure, not a test failure**
   (25 passed, 98 notrun, 0 failed; the verdict `XFSTESTS PASSED`). `writes`
   57,097 against 21,437, `flushes` 15,620 against 8,950, `dcfs_writes`
   14,896 against 11,906. The cause is not 26.34's change of what `writes`
   means, and no re-measure of the same kind can fix it: the shard runs
   generic/364, `timeout 10s` of a two-thread direct-write-and-fsync loop, so
   its requests are the host's rate. Per-test lines in the CI log: 364 alone
   issued 45,012 of the 57,097 writes and 14,404 of the 15,620 flushes; three
   local runs at a load average of 36 issued 853 to 1,060 and 402 to 489 of
   them, and the shard 13,644 to 13,965 writes in all. 26.34 measured nine runs
   at 19,595 at most, and nothing about the budget was wrong for that host.
   The write-side lines (`writes`, `sectors_written`, `flushes`,
   `dcfs_writes`, `dcfs_write_bytes`) are dropped for this target with the
   evidence in `io_budgets.txt` and the README; the read side (`reads`,
   `sectors_read`, `dcfs_reads`, `dcfs_read_bytes`) was measured again over
   all four runs (3 local, 1 CI), 2,116 to 2,153 reads on both hosts, as the
   highest count plus the spread seen. The CPU line was left (17.1 to 19.2 s
   here under load, 8.3 s on CI, against 20.8 s with its tolerance); a
   re-measure takes a quiet host. `run_qemu_cost_test` holds both hosts'
   COST lines to the checked-in budgets and failed on CI's with the three
   messages of the log. 037 (a free-running setter loop against 1,000 reads)
   is the same in kind and belongs in 26.36's audit, with every other
   xfstests test that counts a duration.

   For russ: dropping the write-side lines leaves xfstests with no check of
   what it writes. If it should have one, the timed tests have to leave the
   shard that carries it (a new shard split, or a `xfstests.timed` list
   run apart), which is a change to which tests run where.

## 26.37b Style fixes for 26.37 (post-merge Flash review, 2026-10-17; merged 2026-10-17, d0c51c8, Gemini Flash)

- `run_qemu_cost_test.sh:397-401`: the new check passes on the old budgets
  too; make it fail without 26.37's io_budgets change (e.g. assert the
  dropped write lines are absent, or that CI's line fails the old file).
- `dcfs/sanitizer_cmdline_test.cc:34`: a matcher, `EXPECT_THAT(words,
  Contains(kBuiltWith))`, instead of `EXPECT_TRUE` on a search.
- History in comments and docs (1.9): `qemu_test.bzl:39-41`,
  `dcfs/BUILD.bazel:1902-1903`, `test/qemu/README.md:510, 726, 1455`,
  `io_budgets.txt:173`, `run_qemu_cost_test.sh:387-388` state the rule and
  the current numbers; the before/after goes to the commit and the log.
- The dropped write-side lines are the dashboard's provisional ruling
  (restore a write budget over the shard's non-timed tests), not this step.

## 26.40 The two stale kernel-cache cases (from the 2026-10-17 inventory; queued)

`docs/plan/notes/kernel-cache-invalidation-2026-10-17.md`: two places the
kernel's cached attributes go stale with no help from its own FUSE code.
russ (2026-10-17) asked for tests and a model that fail today, and prefers
synchronous invalidation before the reply to a deferred one (racy).

- **chattr (`FS_IOC_SETFLAGS`, `FS_IOC_FSSETXATTR`):** after the backing
  ioctl succeeds and before the reply, `fuse_lowlevel_notify_inval_inode`
  with a negative offset (attributes only). That path takes only spinlocks
  (`fuse_reverse_inval_inode`, fs/fuse/inode.c:548), so it cannot deadlock
  against the inode lock `vfs_fileattr_set` holds, and its `attr_version`
  bump keeps an in-flight GETATTR from restoring the old ctime.
  docs/design.md's "dcfs never notifies" paragraph is corrected: entry
  invalidation deadlocks in related operations (it locks the parent);
  attribute-only inode invalidation does not.
- **No deferred invalidation without russ** (russ, 2026-10-17: "If there's
  any cases you think deferred invalidation is needed, bring them to me."):
  invalidations are synchronous before the reply; a case that seems to
  need one after the reply (an entry invalidation in a related operation)
  stops and goes to russ.
- **Stores through a passthrough `MAP_SHARED` mapping after close:** dcfs
  refreshes the row only at FORGET; design the fix (e.g. the row's
  attributes unknown while the kernel may still hold a writable shared
  mapping, refreshed at the next getattr) and invalidate as above.
- **Failing first:** guest tests (C++, 25.34's helpers) that chattr through
  dcfs and stat (the ctime differs from the backing's today, for up to the
  attr timeout), and that write through a mapping after close and stat
  (stale until eviction today); 12.20's model shows both as
  counterexamples before the fix, with a known-bug variant.
- Then the attr and entry timeouts become effectively infinite (russ,
  2026-10-17: "I want invalidation to be part of the formal model, but
  after that you can make attr/entry timeouts effectively infinite"),
  once 12.20 models the invalidations: (1) the largest value the kernel
  accepts without overflow (FUSE's `entry_valid`/`attr_valid` are a u64 of
  seconds plus nanoseconds, turned into jiffies by the kernel; find the
  real ceiling in fs/fuse and libfuse's double conversion, or use a value
  above it the kernel clamps, such as INT_MAX), with a test that the reply
  carries it; (2) `attr_timeout` and `entry_timeout` stop being flags
  ("we're now doing the Right Thing and don't need to ask the user to
  figure it out"): removed from main.cc, the mount helper, README and the
  man page; 26.39 converts only `sync_interval` to a Duration flag. The
  zero timeout for files open for writing stays.

Owner: dcfs-protocol (kernel cache coherence), after 12.20 or with it.

## 26.41 Performance bumpers that do not move with load (russ, 2026-10-17)

russ: "The whole point of both the CPU and disk I/O budgets are to try to
provide mechanical 'bumpers' around performance regressions, but our host
can get heavily loaded ... Is what we're doing the best way to achieve
that goal?" The orchestrator's proposal, agreed if the investigation
bears it out ("Investigate, then if it pans out, I agree with your
proposal"):

1. Primary bumpers are exact counts of what dcfs's design controls, with
   zero or near-zero tolerance: FUSE requests per operation, backing
   operations by kind, fsync/syncfs, SQL statements and transactions
   (`backing_budgets.txt`, `request_budgets.txt`).
2. Complexity tests (russ: "How does this measure the complexity? It's
   two points on some unknown curve."): today's 26.4b slopes check a
   linear envelope at N = 100 and 1000 only. Instead, because the counts
   are exact, each operation class runs at several geometrically spaced
   sizes (10, 100, 1,000, 10,000 or more, above realistic directory
   sizes) and the points must be exactly collinear with today's per-item
   cost a and constant b both pinned (a = 0 for operations that should be
   constant per call). Three or more exactly collinear points rule out a
   quadratic (more points, higher degrees) and a log factor that shows in
   the counts across the range; a cost that appears only beyond the
   largest size is the stated limit.
3. CPU work is instructions retired (user and kernel, 26.36's PMU
   findings) in a few dedicated benchmark scenarios, unheld without a PMU.
1a. Costs are vectors by kind, not a sum of "1 per syscall" (russ,
   2026-10-17: "Is cost 1 really the right number? shutdown() or
   connect() has a very different cost than gettimeofday()"): each count
   is per backing-operation kind, and each kind is split by its costly
   fields: a statx with STATX_DIOALIGN counts separately from a plain
   statx (as the backing/procfd/sync split in `syscall_budgets.txt`
   already starts), so an fsync never trades against a statx. The line at
   the kernel is drawn because a call's kernel-side cost depends on state
   dcfs does not control (the backing filesystem, its caches, the
   hardware, the kernel version), not because the kernel is opaque: its
   source is here, and where dcfs's design depends on a kernel cost (what
   syncfs flushes, getdents64's batch size) that cost is reasoned from the
   source. The A/B benchmark measures the total, kernel included.
1c. **Rename: syscall budgets become backing-operation budgets** (russ,
   2026-10-17: "which aren't just syscalls anymore after this"). The
   budget counts what dcfs does to the backing, and that is no longer only
   syscalls (FUSE_STATX and its costly fields, io_uring operations).
   `test/qemu/guest/syscall_budgets.txt` becomes
   `test/qemu/guest/backing_budgets.txt`; its targets in
   `test/qemu/BUILD.bazel`, the scripts that read it (`strace_lib.sh`,
   `syscall_traces.sh`, `mkinitramfs.sh`), their tests and
   `test/qemu/README.md` change in this step, and so do the messages that
   name the file.
1b. **Query-plan check** (russ: "Yes"): a test runs `EXPLAIN QUERY PLAN`
   on every SQL statement in dcfs against the real schema and fails on a
   full-table SCAN not allowed with a reason, so each statement's cost is
   O(log n) per row returned. Small; Gemini.
2a. Each operation gets a worst-case input family next to its typical one
   (readdir with every attribute unknown, lookup among many negative
   entries, recovery with N dirty rows, rename of a directory with N
   descendants, unlink of a file with N links), listed in the test file;
   12.22 searches for the ones nobody listed.
3a. Internal work, exactly (russ, 2026-10-17: "Agreed to adding (1) and
   (2), with the adjustments you mentioned for deterministic execution"):
   (1) the slope scenarios also run under the coverage build and record
   per-function execution counts (`-fprofile-instr-generate`), held to the
   same exact collinearity at each size, so work done and discarded shows
   up in the function that did it; (2) SQLite's `sqlite3_stmt_status`
   counters per statement (VM steps, full-scan steps, sorts, automatic
   indexes), held the same way. Both depend on one request at a time
   (the daemon is single-threaded); the io_uring phase keeps that possible
   with a deterministic test executor (README future work).
3b. Allocation budgets (russ, 2026-10-17, bounded memory:
   `docs/design.md`, "Bounded memory"): allocations, or peak bytes, per
   operation are measured at two sizes (e.g. 10 and 100,000 entries) and
   must have zero slope: a request's working memory does not grow with the
   directory or file it touches. Counts, not time, so load does not move
   them; an exception is a recorded decision.
4. Wall-clock speed is an A/B benchmark: main and the candidate
   interleaved on the same host, compared as a ratio; on demand or
   nightly.
5. The per-test block-layer I/O budgets (`io_budgets.txt`) and per-test
   CPU budgets (`cpu_budgets.txt`) go; the wall allowance stays only as a
   hang guard.
6. **Count something deterministic** (russ, 2026-10-18: "Count something
   deterministic"; neither a longer commit interval nor skipping load-starved
   runs). Replace every disk count budget with a count of what dcfs causes:
   the cgroup `io.stat` counters (`dcfs_reads`, `dcfs_writes` and siblings)
   and the `/sys/block` counters (`reads`, `writes`, `sectors_read`,
   `sectors_written`), counted at the syscall layer, so a verdict never
   depends on page-cache, readahead or journal-commit timing. Then remove
   26.17c's interim +2% headroom (fault_ace_mixed_test_ext4 `dcfs_reads` 753
   against 752, CI run 39157612253, pinned in 25.28b from too narrow a
   sample). The full run 9d6468d3 that went red was this budget; its test
   body passed. Part of 26.43 (the load-agnostic-to-zero step, which owns the
   rest of the load-sensitivity work).

**Investigation first:** from `docs/plan/log.md`, the phase files, git
history and CI history, every regression or finding the CPU, I/O,
syscall and request budgets caught, and for each whether an exact count,
a slope test or an A/B run would have caught it. Any regression only a
block-layer or CPU-time budget caught changes the design and goes to
russ. Report in `docs/plan/notes/`. Supersedes the dashboard questions
on instruction budgets, disk I/O drift and the xfstests write lines.

Owner: the investigation by Gemini (whole-history reading), verified by
the orchestrator; the change by dcfs-implementer (medium).

**Investigation result (2026-10-17,
`notes/performance-budgets-history-2026-10-17.md`): it pans out.** Eight
real regressions or findings, every one caught by an exact count (syscall
and SQL budgets, FUSE request counts), a slope test, or a cost comparison
against native; none caught only by a block-layer I/O or CPU-time
budget. Eight budget failures were noise (load, block-layer merging, the
journal timer, timed tests, a missing sanitizer flag), all from CPU-time,
wall-time or block-layer budgets. So the proposal proceeds as russ agreed.

## 26.43 Load-agnostic to zero (russ, 2026-10-18; queued, not started, M2)

Gathers the load-sensitivity work that was scattered as un-numbered
follow-ups, so that one step owns it. Not started before the 2026-10-23
quota reset. Owners: dcfs-investigator first (the measurement and the
audit), then dcfs-implementer (the changes).

1. The disk count budgets become deterministic counts: **26.41 (6)** owns
   this (russ, 2026-10-18: "Count something deterministic"), with 26.17c's
   headroom removed in the same change.
2. `write_test_ext4`'s I/O budget and its suspected cause: at host load ~50
   it counted 335 writes against 325 and failed ("load-starved"); alone it
   passes (24.8 s on main); it also failed in 17.6's first presubmit run.
   At 87 to 91% host pressure the disks' writes and flushes moved (329 to
   336 against 325, 144 to 150 against 145), and `fault_ace_b_test` was 0.2
   to 0.6% over. The suspicion: the wall-clock sync interval, so the number
   of sync points (each a syncfs and a WAL fsync) varies with load. Find what
   makes the count depend on timing (writeback, journal commits, the sync
   interval, which 25.28c moves to CLOCK_MONOTONIC) and make the budget count
   only what dcfs causes; test first. The 25.28b fix round saw it again
   (writes 325 to 328 against 325).
3. 26.36's follow-ups A to H (the owner and scope are unchanged): `instr_user`
   CPU budgets across `cpu_budgets.txt` with 10% tolerance, re-measured
   together over one run count, including `fault_ace_a_test`,
   `fault_ace_mixed_test_ext4` and `xfstests_1_test_ext4`; sync points on
   demand for the sync-interval sleeps; `cancel.sh` by counts of backing
   requests after the signal; `readdir_boundary.sh` by instructions; the
   fault windows (`fault_recover.sh`, `fault_selftest.sh`) by events (11.3c);
   `probe`, `expect_answered`, `fd_blocked` and `quiesce_daemon` by counters,
   with the fourteen settles after them; `nfs.sh`'s rpc readiness and mount
   retry and `init`'s coverage-run waits; `xfstests.sh`'s per-test `LIMIT`
   from the harness's allowance.
4. The remaining entries of `tools/repo_shape_sleeps.txt`, each a verdict on
   time or a sleep standing in for an event (1.11).

Done when `tools/repo_shape_sleeps.txt` and 26.17c's headroom list are empty
and no budget line counts I/O that the guest kernel schedules by time.
Test first for each change; never widen a budget or tolerance to pass, and
never weaken a test (AGENTS.md).

## 26.44 A TCG boot test: one guest under `DCFS_FORCE_TCG=1` (clerk batch 27, 2026-10-18; queued, not started)

Every guest test falls back to TCG when `/dev/kvm` is unusable or
`DCFS_FORCE_TCG` is set (`test/qemu/scripts/run-qemu.sh:736-757`, since 5.1,
e7e0714e), but nothing boots a guest that way: `run_qemu_cost_test.sh:158`
only fakes the run. Add a test that boots one guest with `DCFS_FORCE_TCG=1`,
asserts that it passes, and asserts that its log says `using tcg`
(`run-qemu.sh:743`), so the fallback stays real. Test first; a C++ guest
test under `test/qemu/guest/` (no new shell). Owner: dcfs-mechanical.

## 26.45 Delete or use the `requires-kvm` tag (clerk batch 27, 2026-10-18; queued, not started)

The tag is added at `test/qemu/qemu_test.bzl:330` and
`test/qemu/qemu_cc_test.bzl:218`, and nothing reads it. KVM is optional for
every guest test (26.44), so either delete the tag, per the
no-unnecessary-code rule, or make CI select on it (CI's `full` job skips
xfstests and the xfs and btrfs pjdfstest runs without KVM, `test.sh:66-76`
and `test.sh:79-81`). Owner: dcfs-mechanical.
