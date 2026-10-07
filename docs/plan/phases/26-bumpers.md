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
  checker uses (`Context::checks`-style: one increment per step and per
  transaction, nothing formatted, zero cost shipped). Then: (1) FUSE
  requests per user-level operation: how many READDIRPLUS/GETATTR/LOOKUP/
  OPEN the kernel sends for `ls -l` of N entries, `find`, `stat` of a path
  N deep, `cat` of a file, counted from the daemon's request accounting
  (same hook), budgeted like 26.4; (2) slope tests for every operation
  class, not only readdir: create N, unlink N, rename N, mkdir N, cold
  lookup N, setattr N, each bounded a*N + b in SQLite steps, transactions
  and backing syscalls at N = 100 and 1000 in the harness (the create
  path, 119 steps / 10 transactions / 2 fsyncs, is the first target).
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
