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
  dcfs-protocol.
- 26.3 Golden backing-syscall traces per operation, with strace (russ:
  observe reality, not our accounting). Alpine's `strace` in the guest
  (alpine_package); a helper `strace_op` in lib.sh quiesces the daemon,
  attaches `strace -f -y -e trace=%file,%desc,%fstat ...` to it, runs one
  operation, detaches, and reduces the trace to the backing mount's calls
  (the `-y` paths separate backing from the cache database) as
  `syscall(path-kind)` lines; a checked-in golden per operation and cache
  state: a cached LOOKUP = nothing, a cached READDIR = nothing, unlink =
  `unlinkat` + the phase-3 `fstatat`, create, rename, write-through,
  FORGET of a written file = nothing (held fd), ... A diff fails the
  test, with the trace printed. Owner: dcfs-implementer.
- 26.4 Ratchets on deterministic counts only (russ: wary of brittleness).
  From 26.3's traces and a counting SQLite hook: backing syscalls per
  operation, SQLite statements and transactions per operation, fsyncs per
  operation. A checked-in budget file; the test fails when a count rises;
  raising a budget is an explicit edit whose commit says why. No
  time-based or size-based ratchets; memory peaks are reported (MEM
  lines), not gated. Owner: dcfs-implementer, with 26.3.
- 26.5 Limited mutation testing (russ: limited). Scope: `metadata_cache.cc`
  and the protocol paths of `dir_cache_fs.cc`; operators: negate a
  condition, delete a call to a phase function (Begin*/End*/Mark*),
  swap present/absent; killed by the small tier plus trace validation.
  `mull` on the pinned clang (after 7.1/7.2) or a small clang-rewriter
  mutator if mull does not fit. Runs on the schedule (nightly/weekly) for
  the whole scope and, per push, only for functions the push touched. A
  surviving mutant is a missing test, filed as a step. Owner:
  dcfs-investigator for the tooling, dcfs-protocol for the survivors.
- 26.6 (needs russ's yes; proposed as the fast form of fault
  enumeration) In the forged-request harness: run a workload once
  recording its K backing calls, then iterate N = 1..K in-process (fresh
  in-memory cache database, the `--wrap` hooks failing the Nth call),
  checking 26.2's invariants and recovery after each. One guest boot,
  thousands of iterations at milliseconds each, small tier. After 26.2.
- Not adopted: a bespoke in-memory reference filesystem for differential
  testing (pjdfstest, xfstests and fsstress already test against the
  kernel; TLA+ test generation, 12.10, is the random driver).

Order: 26.1 and 26.2 as lanes free up (no new tooling); 26.3 + 26.4 next
(Alpine's strace is one rule call away); 26.5 after 7.2; 26.6 if russ
says yes, after 26.2.
