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
