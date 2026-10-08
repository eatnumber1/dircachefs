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


## 12.3 Shared backing fd model (russ, 2026-10-07)

Phase 23 found that after `chattr +i` a writable open still wrote through
a shared backing fd opened before the flag. The main model abstracts one
directory's cache records and has no fds or permission state, so this
class (a shared fd outliving a permission change: immutable or
append-only flags, chmod removing write, an ACL change) is outside it.
Add a small second module, `formal/fds.tla`: per object, a backing
permission state (writable or not, mutable through a flag change or
chmod), dcfs's shared backing fd with its access mode, OPEN/RELEASE with
modes, WRITE through an open. Invariant: a dcfs OPEN for writing succeeds
exactly when the backing would open for writing at that moment, and an
already-open descriptor keeps its rights (POSIX's own rule), so no write
ever happens through dcfs that the backing would have refused at that
open. Known-bug variant: the pre-fix behaviour (reuse the shared fd's
mode). Trace validation: map the existing OPEN, RELEASE and IOCTL events
(and WRITE's wakeups are not events: use the harness's forged requests)
onto it. Owner dcfs-protocol; after Phase 23 merges.

## 12.3 widened (russ, 2026-10-07): a revalidation model, and further models

Generalise 12.3 from fds to the class the chattr +i and casefold findings
share: dcfs reuses something (a shared backing fd, a cached mode or ACL
used by default_permissions, a cached negative entry or complete listing,
a cached decision) across a change of the backing's state that should
have invalidated it. Invariant: nothing dcfs holds grants more than the
backing would grant at the moment of use, except where POSIX grandfathers
(already-open descriptors). `formal/reval.tla`: objects with permission
state (mode/flags/casefold), dcfs's held fds and cached decisions,
changes through dcfs and out of band (the latter only to show the
documented limitation), opens/uses. Known-bug variants: the pre-fix
shared-fd reuse, SETFLAGS changing casefold under a complete listing.
Trace validation from the start (OPEN/RELEASE/IOCTL/SETATTR events).

Further models, each with trace validation in the same step:
- 12.4 Inode lifetime: rows retired at the last release, in-memory records
  for removed objects, stub rows, lookup counts and FORGET/BATCH_FORGET,
  DESTROY; invariant: no reply refers to an object the kernel no longer
  references, and no row or record is dropped while referenced.
- 12.5 Identity (before Phase 14): node ids, generations, handle validity
  across restart and cache wipe, inode recycling, ESTALE rules.
- Cancellation stays in the main model (Phase 22).

## 12.6-12.10 Ideas borrowed from prior art (notes/formal-prior-art-2026-10-07.md)

In this order, each with a known-bug variant and trace validation:
- 12.6 Recovery idempotence (FSCQ): an invariant that from every state
  where recovery has partly run, the recovery precondition still holds;
  check `Crash` fires during `RecoverDirty`/`StartRun` (coverage). ~1 day.
  12.6b (lane-1, 2026-10-08): the finding "recovery not idempotent" is
  fixed by keeping dirty rows across StartRun until each is probed; the
  review of that fix found a second, pre-existing hole: recovery cleared
  probed rows from the dirty set with no `syncfs` since the crashed
  run's backing syscalls (a power loss shortly after a daemon-crash
  restart leaves dcfs ahead for good), unseen by the model because
  `Crash` made every backing write durable even for a daemon-only crash.
  Fix in the same step: recovery clears nothing (rows stay dirty until
  the first sync point), a daemon crash keeps `bOpts`, plus a
  `known_bugs/` variant.
- 12.7 Effect-point property (SibylFS): each request is call, effect,
  reply; every reply equals what the backing would answer at some instant
  between call and reply (`Obs.tla`); covers mutation results, not only
  served answers. 3-5 days. 12.7 as merged with 12.6 names the syscall
  and commit actions in `EffectAtSyscall`/`CacheLearnsAtCommit`; 12.7b
  (pending): the reply ghost, mutation results and errnos checked against
  the backing states between call and reply.
- 12.5 Identity (`ident.tla`), from the kernel's exporting.rst, RFC 8881
  §4/5.8.1.5 and RFC 1813: handle classes, recycling, durable vs volatile
  ids, cache wipe and restart; safety: a handle never resolves to a
  different object; ESTALE exactly when gone or generation differs;
  disconnected objects rebuilt from the database. Before Phase 14.
- 12.3 adds a coherence parameter (NFS close-to-open vs delegation):
  `Exclusive` keeps CacheNeverWrong; `CloseToOpen` admits out-of-band
  changes with the weaker "fresh as of the open" invariant, making the
  documented limitation a checked statement. (Status 2026-10-07: 12.3
  shipped `OutOfBand` as the parameter and `formal/limitations/` as the
  checked statements of what breaks without exclusive access; the weaker
  "fresh as of the open" invariant is NOT written; deferred to 12.6+.)
- 12.8 Backing crash model with tree sequences (Ferrite, DFSCQ): an
  ordered sequence of backing states since the last sync replaces the set
  of crash states; a constant selects sequential / metadata-prefix /
  ext4-weak reordering; directory-fsync semantics as a switch; Ferrite's
  litmus tests (atomic-replace-via-rename, atomic-create-via-rename,
  implied-directory-fsync) as TLC configs: the bad outcome must be
  reachable through dcfs only if reachable on the backing directly
  (`CrashRefines`). Before any multi-directory model. 1-2 weeks.
- 12.9 Directory streams (SibylFS must/may): opendir, cookie-based
  chunks, concurrent mutation and refill between chunks; untouched
  entries returned exactly once; cookies stable across a refill. ~1 week.
- 12.10 (spike) Test generation from TLC's state graph replayed through
  the forged-request harness with `--wrap` holds forcing the model's
  interleavings (MongoDB's technique that worked; CCF's simulation
  driver). 1-2 weeks, after 12.4.
Phase 11 gains ACE's crash workloads (Apache-2.0) as inputs to the
dm-log-writes replay, oracle "dcfs view equals the backing after
recovery"; SibylFS's scripts (ISC) as a differential trace diff of dcfs
against the backing only if pjdfstest, fsx and fsstress leave gaps.

## 12.3 done (2026-10-07, 2c3d79c)

`formal/reval.tla` (not `fds.tla`: the widened revalidation model), with
`MC_reval`, `MC_reval_oob`, `MC_reval_liveness` (the last a smoke check:
under exclusive access the safety invariants already force convergence;
the real liveness content is `limitations/out_of_band_stale`);
known-bug variants: no re-check (the pre-Phase-23 bug), re-check only
when flags changed through dcfs (the bypass with OutOfBand; passes
without), casefold, no write fd, write fd replaced by a later writer,
write fd never dropped; `formal/limitations/` for out-of-band mode,
casefold and staleness. Trace validation: `FileOpened`/`FileReleased`
events, `ioctl_arg`, setattr `to_set`, per-file `DCFS-REVAL` traces,
`oob` lines from tests (`NoteOutOfBand`), harness only: the guest
recorder does not yet write file traces (gap documented in
formal/README.md). Every T_* branch is taken by a validated trace except
T_Write's EBADF (unreachable in fixed code). Review: merge as is +
follow-ups done. Deviation: recorder unit tests written after the code.

## 12.11 (from 8.2, 2026-10-08): the end of an attribute change as an event

Mutation testing showed that deleting a `Mutation::End()` after an
attribute change (copy_file_range, fallocate) is invisible to trace
validation: no model has an event for the end of an attribute change, so
the trace still validates. Add the event to dcfs.tla's attribute-change
transitions (and the recorder), with a known-bug variant "End skipped"
that validation rejects.
