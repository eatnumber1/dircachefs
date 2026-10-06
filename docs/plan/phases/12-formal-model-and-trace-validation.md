# Phase 12 — Formal model (TLA+) and trace validation

**Decisions (russ, 2026-10-05).**
- Model dcfs's write-through protocol in TLA+ and check it with the TLC
  model checker.
- Guard against the model drifting from the code with trace validation:
  recorded traces of real runs must be behaviors the model allows.
- These checks need neither root nor kernel control, so they run on the
  host (the QEMU rule covers only tests that need those). The traces come
  from tests that do run in QEMU.

Order: after Phase 11 (its crash and stress tests produce the traces) and
before Phase 13, so the later protocol changes (identity, the wrapper)
are made against a checked model.

## 12.1 The model

- Where: `formal/` at the repository root: `dcfs.tla` (the model),
  `MC.tla` and `*.cfg` (model-checking configurations), and a README
  written for a TLA+ newcomer (russ has not used it): what each variable
  and action means, which `docs/design.md` section and which dcfs
  function each action corresponds to, and how to run TLC and read a
  counterexample.
- State: the backing filesystem (names in one directory -> objects), the
  cache (dentry state present/absent/unknown, the directory's
  completeness flag and epoch, the dirty set), each split into what is
  durable on its disk and what is still volatile; in-flight requests
  (fills with their fill guards and epoch, mutations by phase).
- Actions: mutation phase 1 (mark unknown, add to the dirty set, durable
  commit), the backing syscall, phase 3 (commit present/absent), fill
  start and fill finish (compare-and-set on the epoch), serving a lookup
  or readdir from the cache, a sync point (syncfs, then empty the dirty
  set), a crash (each disk independently keeps some prefix of its
  unsynced writes), and startup recovery.
- Properties: every answer served from the cache matches the backing
  filesystem at that moment ("no cache ahead", including after any crash
  and recovery); the tri-state rules (a record mirroring the backing
  filesystem is never present or absent while a mutation of it is in
  flight); completeness never claims a name is absent that the backing
  filesystem has; recovery always terminates.
- Bounds: two names, one directory, up to two concurrent requests, a few
  crashes. Small configuration in the `medium` tier (target: under a
  minute); larger bounds in the `slow` tier.
- Test first, for the model itself: variants of the model with known
  historical bugs re-introduced must produce counterexamples. From
  `audits/`: the parent not marked unknown on create (crash F3), the
  restore-completeness lost update (tri-state F4), unguarded fills
  (tri-state F1), and phase 1 not durable before the backing syscall
  (crash F1). A model that cannot find these is too coarse.
- Tooling, pinned (Phase 4 rules, under `third_party/tlaplus/`): TLC's
  `tla2tools.jar` release fetched by Bazel (`http_file`, sha256), the TLA+ CommunityModules jar (for
  reading JSON traces), and a hermetic JDK through `rules_java`'s remote
  JDK. Bazel targets run TLC as ordinary tests.

## 12.2 Trace validation

**Pulled forward (russ, 2026-10-06):** runs right after R4.2, before
cancellation (Phase 22) and the identity/wrapper phases, so the C++ is
checked against the model before those change the protocol. Until Phase
11's crash and stress suites exist, the traces come from the existing
guest tests (crash, power, rename, create, write, readdir_boundary) and
the forged-request unit harness (`dcfs:dir_cache_fs_test`), which can drive
exact interleavings. A `dcfs-reviewer` pass checks every event call site
against the model action it claims (an event in the wrong place validates
a lie). The action-coverage report lists model actions no trace reached.


- An event interface in dcfs (`dcfs/protocol_events.h`): one method per
  model action (mutation phase 1/3, backing syscall done, fill
  start/finish, served answer, sync point, recovery steps). Production
  code calls a no-op implementation; the call sites are the only
  production change.
- A recording implementation lives in `testonly/` and writes one JSON
  line per event to a scratch disk. A `testonly = 1` build of dcfs links
  the recorder instead of the no-op (link-time selection, like the
  existing `-Wl,--wrap` fakes), so production binaries contain no test
  code.
- The Phase 11 crash, stress and failure tests and the fsstress runs also
  run with the recording build (in QEMU) and export their traces.
- A trace-validation spec (`formal/Trace.tla`) constrains the model to
  the recorded events, following the published method for validating
  program traces against TLA+ specifications (Cirstea, Kuppe, Merz et al.,
  2024; used by Microsoft's CCF and by MongoDB). TLC must find a
  behavior of the model that matches each trace; if not, the test fails
  and reports the first event the model cannot explain.
- The other direction: across all traces, report which model actions
  never appeared (TLC's action coverage). Every action should appear in
  at least one trace; gaps are listed in `formal/README.md` until a test
  covers them.
- Test first: a `testonly` fault-injected build that skips marking a name
  unknown in phase 1 produces a trace that validation rejects.

Owner: Opus (protocol invariants);
russ reviews the model's README. AGENTS.md gains the rule that a change
to the protocol updates the model in the same change.

KLEE was planned as a one-function trial and dropped (russ, 2026-10-05):
it supports only LLVM 16 (partially up to 19), far behind the pinned
toolchain.
