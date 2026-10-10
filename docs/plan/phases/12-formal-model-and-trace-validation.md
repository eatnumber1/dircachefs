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
  `known_bugs/` variant. Merged 2026-10-08 (5776861) after two review
  rounds: dcfs.tla `Crash` = `PowerLoss` | `DaemonCrash`, `ProbesDone`;
  `known_bugs/recovery_clears_dirty` (CrashSafe) and
  `known_bugs/lifetime_probe_list_in_memory` (RowsNameLiveObjects);
  `MC_recovery`, `MC_lifetime_recovery`; the crash-during-recovery
  harness binary with a `local_defines` flag so the faulted probe is
  shown to fire; `RecoveryDone` recorder event. State counts: MC_small
  871k, MC_large 9.16M (about 800 s on the runner, timeout eternal),
  MC_nolock 6.37M.
- 12.7 Effect-point property (SibylFS): each request is call, effect,
  reply; every reply equals what the backing would answer at some instant
  between call and reply (`Obs.tla`); covers mutation results, not only
  served answers. 3-5 days. 12.7 as merged with 12.6 names the syscall
  and commit actions in `EffectAtSyscall`/`CacheLearnsAtCommit`; 12.7b
  merged 2026-10-08 (5094b31, two review rounds): `ReplyObservable` (a
  witness instant between call and reply over the backing's states;
  `Window(r)` = the answers the backing gave the request's own queries,
  `Remember` adds what other requests lose at a syscall; errno classes
  ok/ENOENT/EEXIST/EAGAIN/EINTR, EINTR/EAGAIN only without a successful
  syscall; `QueryKinds` shape check), `known_bugs/reply_after_failed_syscall`
  and `reply_unknown_as_negative`, trace reply lines carry the lookup's
  answer and the errno actually sent (`LookupAnswered`, `Replied`),
  `T_Reply` strict; MC_large and MC_interrupt_muts2 check it (large
  10.24M states, ~1 h here, ~2000 s on the runner, eternal); a create whose
  new name vanished before its probe is cut out-of-band. 12.7c (pending): a
  handler that returns OK without replying records errno 0 while
  `~FuseRequest` sends ECOMM; pass a sentinel so the recorder marks it
  unexplained. Still uncompared: the nodeid and attributes a lookup sends,
  readdir listings, attribute values.
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
  Status 2026-10-09: merged (638590d). As built: `bSeq` replaces the crash
  set; `Reorder` in {seq, metaprefix, ext4}, the ext4 regime stated as an
  over-approximation; directory fsync as a switch; litmus tests under
  `formal/limitations/crash_litmus.tla` (the bad outcome reachable on the
  backing directly) and `MClitmus.tla` (not reachable through dcfs);
  `CrashRefines` follows from `CrashSafe` and covers D only, since 23.8's
  F keeps cached attributes that can lag the backing after a power loss;
  F's attribute stamp crashes with D's metadata from one state under seq
  and metaprefix, from any state under ext4. The 23.8 configs bind
  `Reorder = "seq"`. design.md cites Ferrite's specification. The
  condition 23.10 must meet is recorded in phases/23 (23.10).
- 12.9 Directory streams (SibylFS must/may): opendir, cookie-based
  chunks, concurrent mutation and refill between chunks; untouched
  entries returned exactly once; cookies stable across a refill. ~1 week.
- 12.10 (spike) Test generation from TLC's state graph replayed through
  the forged-request harness with `--wrap` holds forcing the model's
  interleavings (MongoDB's technique that worked; CCF's simulation
  driver). 1-2 weeks, after 12.4.
  Also (russ, 2026-10-09, "I agree"): replay every known_bug counterexample
  through the harness as a generated test (a TLC trace is a script of
  requests, crashes and fills), starting with 23.11's ten-state ghost-row
  trace, so the model-to-code direction is closed for each finding. And a
  standing rule for formal/README.md "Changing the model" (added by 12.12a):
  every table or column that mirrors the backing has its row lifecycle
  (creation and deletion) under the crash prefixes in the model, not only
  its contents; 12.13's sweep says whether the invariants about it bite.
  Also 12.10's (moved from 12.14, 2026-10-09): the harness facility that
  copies the WAL at each transaction (the `SqliteTransaction` hook) and
  restores a chosen copy, the crash-point replay tool.
  Approved (russ, 2026-10-09): not a spike. The generated tests are NOT
  checked in: a Bazel target runs TLC (or reads its dumped state graph),
  generates the test cases every build, and a test target runs them; the
  generator itself is tested with a fixed small graph. Order: after 12.12
  and 12.11b/c.
- 12.11b Per-file trace validation (approved, russ, 2026-10-09): the
  recorder emits a file's events (attribute change, writable open and
  release, held fill, the atime-only mark) and Trace.tla takes them, so
  file-level protocol is checked against the model rather than by harness
  tests alone. 12.11c Cross-directory trace: one trace covering the
  interleaving of two directories' events (rename and link across them,
  sync points clearing each), the gap the 23.10 review named. Owner:
  dcfs-protocol; after 12.12.
- 12.12 Read-only audit, model against code and tests (approved, russ,
  2026-10-09; Opus reviewer): what dcfs.tla, lifetime.tla, reval.tla and
  ident.tla cover versus what the C++ does (every mutation class, fill,
  guard, sync and recovery path, cancellation, identity, writable opens,
  xattrs, directory streams); what trace validation actually checks per
  event; which properties bite (have a known_bug or a premise); which code
  paths have no model and no harness test; what of the unmerged
  `step-23.10` model (second directory, file names, hard links, the three
  "known parents" counterexamples) is worth merging on its own. Output: a
  gap list in `notes/formal-coverage-audit-2026-10-09.md` ranked by risk,
  each gap with the proposed step (a model extension, a trace event, a
  harness test, or "accepted, documented"), and the order for 12.13,
  12.11b/c, 12.10 and 23.11. No code changes.
  Done 2026-10-09: `notes/formal-coverage-audit-2026-10-09.md` (G1-G24,
  the matrix, what traces check, 22 properties with no failing variant,
  unmodelled code paths, what to merge from step-23.10, the order below).
  Headline gaps: G1 writable opens have no model and no trace (Write,
  Fallocate, CopyFileRange emit no mutation events); G2 the database
  durability abstraction is never tested where writes reorder; G4 the
  concurrency guards only bite in the large tier (under the kernel lock
  `Owns` is always true; MC_nolock checks ReplyObservable only); G5 a
  LATENT CRASH GAP: a created object's row is outside the create's mutation,
  so a listing of the parent between the create's syscall and RecordNewChild
  upserts a clean valid row at normal durability, and the crash state
  "listing committed, phase 3 lost, create lost on the backing" serves a
  nonexistent object by nodeid (unreachable today: one thread plus the
  kernel's directory lock; reachable in the harness and under parallel
  dirops; it breaks 23.11's premise and must be modelled there); G11
  Trace.cfg checks GuardsBalanced and four action properties only, while
  traces exceed the model-checking bounds; G12 only four guest scripts are
  traced and three of their root traces end at the first link or
  cross-directory rename; G15 ClearDirty's fast path assumes every dirty
  row came through a mutation, which RecordTmpfile's MarkDirty does not
  (latent); G16 Phase 13's OpenNode change needs ident.tla updated in the
  same change.
- 12.12a Oracle hygiene (from the audit; small, dcfs-protocol): premise
  configurations for the 22 properties with no failing variant (section
  3 of the note, reusing existing variants); `TriState`, `CacheNeverWrong`,
  `CrashSafe`, `DurableSetSound`, `CleanMeansNoDirty` added to Trace.cfg;
  a medium-tier `MC_nolock_small` and the three effect-point action
  properties added to MC_nolock; `GuardsBalanced` over F's guard with a
  known_bug `file_setattr_end_skipped`; hand-edited negative trace logs
  for the guard-decision checks; a TLC coverage report via `tlc_args`
  (`-coverage`); a two-name crash_f1 variant checking only CrashRefines.

Order from the audit (2026-10-09), replacing earlier guesses: (1) now, in
parallel as lanes free: 12.13's tool, 12.12a, 12.14 (with dm-log-writes
replay, CrashMonkey's method, plus a harness facility that copies the WAL
at each transaction), 12.17; (2) 12.13's first sweep once 23.8b is settled
(Trace*.tla in scope, MC_nolock_small in each mutant's test set); (3) 23.11's
model with F's row existence and G5 among its known_bugs, reusing 23.10's
F names; (4) 12.11b, first modelling writable opens the code's way (a
`wopen` count, not an in-flight mutation), then the missing events; (5)
12.11c first half: per-directory projection of cross-directory steps onto
the one-directory model plus "a syscall that fails without changing
anything", then traced fault_power and a short seeded fsstress; (6) 12.15
with the rule "in flight implies attributes unknown at every backing
call"; (7) 12.10: crash-point replay first, then known-bug counterexample
replay, then the concurrency branches; (8) 12.9, 12.11c's joint trace if
still needed, 12.16 last. From step-23.10, merge on their own: F's names
and hard link, the ghost of F's last change (two-sided FileOK), the ext4
item ordering, GuardsBalanced per guard, the three named conditions with
premise tests re-expressed per inode, the three "known parents"
counterexamples; not the home, directory marks, the three-way
RecoveredFile or the MC_dirset configs.
- 12.13 Mutating the model (approved, russ, 2026-10-09): extend
  `tools/mutation/` with TLA+ operators (negate a conjunct, drop a
  conjunct, swap `/\`/`\/`, `<=`/`<`, drop a guard from an action, drop a
  step from a sequence, swap two steps, replace a durability level); a
  mutant survives when every `//formal` test still passes (known_bugs
  must still produce their counterexamples, so a mutant that silences one
  is killed by that test). Survivors are under-specified properties:
  each gets a property or a config, or an `equivalent.txt` entry with the
  reason. Bespoke, in-tree, same report shape as the C++ mutation job;
  a weekly CI job after the first sweep. Owner: dcfs-implementer for the
  tool, dcfs-protocol for the survivors. After 12.12.
  Tool half done 2026-10-09, merged 9f5d17f (lane-5, three commits). As
  built: `mutate.py --lang tla` with a conjunct-aware token scanner (not a
  parser; masks comments, finds column-0 definitions, bulleted and inline
  `/\\`/`\\/` chains, LET, quantifiers, IF; never drops or negates an
  effect or frame conjunct); operators negate, drop-guard/conjunct,
  drop-disjunct (incl. Next), swap-junction (a whole chain at once: a
  partial swap is a SANY error), relational, constant (numbers, booleans,
  neighbours in `tla_sets.txt`), durability (Commit's second argument),
  drop-step, swap-step; arid: VIEW definitions, `tla_arid.txt` regexes with
  reasons, `\\* mutation: arid` markers; a mutant is the mutated module in
  a tree copy (known_bugs-style overrides cannot express an arbitrary
  conjunct change); killed = any `//formal` test fails (a silenced known_bug
  counts), survived = all pass, INVALID = TLC itself failed; scope file
  lists dcfs, ident, lifetime, reval and the four Trace modules; unreachable:
  mixed-precedence chains, non-column-0 definitions, cfg-only values. CI:
  `mutation-tla` weekly (90 mutants, 3 shards, small+medium) and a TLA+
  step in `mutation-changed` scoped to the definitions a push touches
  (small tier, 10 mutants). First run (20 mutants of dcfs.tla, seed 1,
  small tier only, 62 min of mutant time): 11 killed, 9 survived, 0
  invalid. Survivors for the sweep step: TypeOK swap-junction (241), Serve
  swap-junction (517), RDFromCode drop-step PD_read->PD_commit (651), S2
  durability FALSE->TRUE (1060), FBehind drop-conjunct `d.fAttr # b.f`
  (1503), Recover durability FALSE->TRUE (1568), Next drop-disjunct
  UnlinkSyscall (1707) and AttrChangeSyscall (1712), FileExactStrict
  drop-conjunct `mode = "up"` (1826). The durability survivors (a stronger
  commit is unobservable) are likely equivalent; a Next disjunct
  surviving the small tier says the small tier never needs that step. The
  first full sweep (small+medium, Trace*.tla in scope) runs after 23.11's
  model merges.
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
(done 2026-10-08, e05b345: `attrchange` request of D with phase 1,
syscall, End, refresh fill and reply; `known_bugs/attr_change_end_skipped`
violates GuardsBalanced; Trace.cfg checks GuardsBalanced; the recorder maps
directory setattr/setxattr/removexattr/SETFLAGS/FSSETXATTR to it and the
`dir-attrs` cut is gone; configs with `WithAttrChanges`: small 1.48M,
recovery 40k, liveness 147k, interrupt 281k; large/nolock keep
`AllRequests`. Limit: dcfs.tla models only D, so a skipped End after
copy_file_range, fallocate or a file's setattr is caught only by the
harness tests; 12.11b would be a per-file attribute trace. 12.7c in the
same merge: a handler returning OK without replying is an unexplained line.)
that validation rejects.

## 12.14-12.17 Further formal work (russ, 2026-10-09: "Yes to all")

Order after 12.12's audit, which may reorder them with the approved
12.13/12.11b/12.11c/12.10/23.11.
- 12.14 The model's assumption about SQLite, tested. dcfs.tla abstracts
  durability as two levels (a normal commit, a synced commit) and crash
  states as prefixes over them; nothing checks that abstraction against
  SQLite's WAL. A guest test cuts power (the 11.x power-cut harness) at
  several points inside and between transactions of each durability level
  and checks the surviving database against the set of prefixes the model
  allows (including: a normal commit may be lost, a synced one may not, a
  later normal commit never survives an earlier lost one). A mismatch is a
  finding against the model's durability abstraction, not against SQLite.
  Owner: dcfs-protocol.
  Built 2026-10-09 (lane-2, step-12.14, 91172d3; Opus review of the
  oracle's soundness running). VERDICT: the abstraction HOLDS on ext4, xfs
  and btrfs (cache filesystem), 0 violations; no model change. As built:
  `test/qemu/crash_states.c` (our own parsers of dm-log-writes' log and
  SQLite's WAL: list/apply whole or torn, WAL info/commits/truncate, a
  forged "commit j lost, j+1 kept" WAL for the self-check, database
  fingerprint); `guest/sqlite_durability.sh`: a WAL restart forced first
  (44 creates until autocheckpoint, so later frames overwrite in place;
  without it ext4's crash states within an epoch were all identical, new
  WAL blocks invisible until the journal commits the file size), dcfs's
  intent per operation from the checking build's `SqliteTransaction`
  counter after a STATFS barrier, cross-checked with strace (one WAL fsync
  right after each synced commit's frame: held), the log replayed
  (CrashMonkey's method) onto dm-snapshots kept at each FLUSH: every
  prefix before each FLUSH, the whole log, per epoch every FUA prefix,
  all FUA plus ordinary-write subsets (exhaustive to 64 or the budget,
  else each-lost/each-kept/seeded), torn multi-block writes at five cut
  points; each state opened by SQLite (recovery + integrity_check),
  fingerprinted, and required to equal some reference state (the final
  database with the WAL cut after commit k) and to include the last
  synced commit of every operation whose mark precedes the FLUSH. Results
  (budget 1500, load 13-18): ext4 447 states 72 s, xfs 338 states 54 s,
  btrfs 2026 states 209 s, short (medium) 199 states 30 s; 13-218 states
  per run lost a finished operation's commit; the bound's smallest margin
  0 on btrfs, 3 on ext4/xfs. Positive case: the whole log recovers the
  final state and a restarted dcfs serves the backing. Failing first: the
  oracle stubbed accepted a kept-after-lost commit and a lost synced one.
  Not covered: partial FUA sets with ordinary writes, tears finer than 4
  KiB, crashes inside a checkpoint or WAL restart, database creation,
  FinishRun's final checkpoint. Observations: SQLite fsyncs the WAL header
  at WAL start/restart even at synchronous=NORMAL (more durable than
  modelled); many commits leave identical databases (204 commits, 138
  states); a writable create is a second synced phase 1 (n creates, n+1
  synced commits unless the directory is durably dirty; 23.11 changes
  this). Deviations: loop devices over sparse tmpfs files instead of extra
  virtio disks (the fifth virtio disk's IRQ 8 collides with the RTC);
  targets declared by hand (the matrix macro varies the backing, not the
  cache); `checked_dcfs` large targets; strace added; `replay-log` built
  from the @xfstests pin here. Note: notes/sqlite-durability-2026-10-09.md.
  Opus review 2026-10-09: the oracle is sound (parsers verified against
  dm-log-writes.c and real SQLite: `wal-truncate k` recovers exactly k
  commits on a restarted WAL; 300 randomly damaged WALs agree with
  SQLite's recovered count; a value-based fingerprint is the right test
  for `dbOpts`, a set of database values, and can only err towards false
  alarms), but NOT yet merged: H1 an uninitialised awk counter left the
  first ordinary write of every epoch untorn, which on ext4 and xfs is
  always the WAL writeback, so "a later normal commit never survives an
  earlier lost one" under reordering was never exercised (every script
  epoch recovered exactly two values; fix, a reach check and a host
  self-check of `epoch_states`); M1 the forged-WAL self-check passes
  vacuously when the forgery is corrupt; M2 the "copy the WAL at each
  transaction" facility was neither built nor reported: MOVED TO 12.10
  (it is 12.10's crash-point replay tool); M3 the docs overstate (one WAL
  generation after a forced restart, no checkpoint/restart/growth; btrfs's
  within-epoch states are all equal by design, so its count is FLUSH-
  prefix coverage); L1-L9 (an upper bound on k, log-apply vs replay-log,
  sector size from the log, tears inside the budget, the README paragraph
  landing inside the 11.2 section). Also: the STATFS barrier relies on the
  single thread (revisit with Phase 22's coroutines); the test checks
  SQLite against the abstraction, not which phase 1s dcfs chooses to sync
  (23.11 should not count it as coverage of that); rerun the large targets
  after 23.11 merges. For russ: the test shows, between two checkpoints,
  that every tried power-loss state recovers to the database after some
  commit and never before the last kSync commit that returned; the
  reordering half is untested until H1 is fixed; checkpoints, WAL restart
  and growth, database creation and clean shutdown are outside it.
  MERGED 2026-10-10, 73ed7e5 (two commits). The second round: the awk
  counters initialised, so the WAL writeback is torn; `wal-info` prints
  `beyond=` (this generation's frames past the recovered ones) and
  `aftergap=` (those after a frame that did not land) and the check
  `some-state-kept-a-later-frame` requires such a state in an epoch before
  the cut mark (a whole-run version had passed with the bug still in,
  because the unmount epoch tears other writes; on btrfs the check is a
  SKIP with the reason: copy-on-write puts a frame in the file only at
  the FLUSH, so its states are FLUSH-prefix coverage); `epoch_states` moved
  to `sqlite_durability_lib.sh` with a host test over a synthetic log
  (failing first: "no torn state for 12:0-8"); the forged-WAL self-check
  walks candidates until one opens; an upper bound on the matched commit;
  `log-apply` cross-checked against `replay-log` on one epoch; the log's
  sector size from `log-info`; tears inside the budget; the restart check
  reads the database (syslog is asynchronous); docs state the regime
  tested and the gaps (growing WAL after a clean shutdown, crashes before
  the first FLUSH, btrfs's equal within-epoch states). Final run (budget
  1500, load 8-10): ext4 657 states, 70 distinct recovered, 48 states in
  script epochs kept a later frame past a lost one; xfs 565 / 70 / 48;
  btrfs 1486 / 27 / skipped; short 207 / 20 / 12; 0 violations; the
  bound's margin 0 on btrfs, 3 on ext4/xfs. One state quoted: every write
  of the epoch landed except the first 4 KiB block of the WAL writeback,
  the WAL held 10 frames after the hole, SQLite recovered commit 19, the
  last synced one. Not built: the per-transaction WAL copy (12.10).
- 12.15 Runtime invariant checks derived from the model. The checking
  build's rules (docs/design.md "Runtime invariant checks": tri-state,
  dirty set vs unknown rows, held fds) are hand-written restatements of
  model invariants. Either generate them from `dcfs.tla` (a small
  translator for the invariants that are state predicates over the
  database and the held-fd table, with its own test), or at least pair
  each rule with its model invariant and one known-bad fixture that fails
  both, so a drift between the two is a test failure. Owner:
  dcfs-protocol; the translator, if built, dcfs-implementer.
- 12.16 A refinement target. A small ideal-filesystem specification
  (SibylFS-style POSIX directory and file semantics, as observed through
  the FUSE replies: lookup, create, unlink, rename, link, attributes,
  listings) and a refinement mapping from dcfs.tla's observable replies to
  it, checked by TLC (`INSTANCE` + `Spec => Ideal!Spec` under the view), so
  "dcfs behaves like a plain filesystem" becomes a checked statement rather
  than a set of invariants. Where the mapping needs a limitation (atime
  lag, close-to-open), the limitation is a named weakening in the ideal
  spec, as `formal/limitations/` does today. Owner: dcfs-protocol.
  Refined (russ, 2026-10-10, "Yes I agree"; `notes/posix-contract-sources-2026-10-10.md`):
  the ideal spec is **SibylFS's semantics restricted to dcfs's operation
  set**, translated into TLA+ by hand, each action's allowed outcomes
  cited to the Lem definition that gives them (and the SibylFS per-fs
  flag where ext4/btrfs/xfs deviate: a deviation of the backing is one
  dcfs inherits, by the contract rule of `docs/style.md` 1.12). EAGAIN is
  not an outcome the ideal spec offers for any operation (EINTR is, for a
  cancelled request). The crash contract is not SibylFS's (it has none):
  12.8's regimes stay ours, described in DFSCQ's metadata-prefix and
  Ferrite's crash-consistency vocabulary. Atomicity is stated as
  linearizability (AtomFS's spec notion): each dcfs operation takes
  effect at one instant between its call and its reply, which the
  refinement mapping must exhibit; this is the property 25.10's
  unbounded retries and the coroutine design keep. First task of the
  step: fetch SibylFS (pin it under `third_party/sibylfs/` as a reading
  reference with a README; check its licence), list the Lem definitions
  for our operations, and state the ideal spec's module interface before
  writing actions. Owner: dcfs-protocol; Fable review of the mapping's
  design is warranted (the one place judgement, not labour, is the
  bottleneck).
- 12.18 SibylFS's trace checker as a second oracle (spike; russ,
  2026-10-10). Can the SibylFS checker (OCaml) be built hermetically
  through Bazel (an OCaml toolchain pinned like every other tool, or a
  prebuilt binary pinned by hash if the project offers one) and run on a
  trace of dcfs's FUSE replies converted to its syscall-trace format? If
  yes: a test runs one workload through dcfs and the same on the backing,
  checks both traces, and fails if dcfs's trace is rejected where the
  backing's is accepted (an oracle independent of our model, beside
  12.11). If the toolchain is too heavy or the checker will not build, the
  spike reports why and SibylFS stays the reading reference of 12.16.
  Time-box: one investigator session. Owner: dcfs-investigator.
- 12.17 The wrapper handoff modelled (contingent, 2026-10-11: 15.11 deletes
  the capture after a good trial, and this model with it; do not start). `mount.dcfs`'s capture (the helper's
  private namespace, the staging tmpfs, open_tree, the socketpair), the
  fork and daemonisation, readiness after INIT and the exit statuses form
  a state machine with crash points (helper dies before or after sending
  the fd; parent dies before or after readiness; the kernel's INIT never
  comes; a second mount races on the same cache). A small TLA+ module with
  the property "no mount is left that nobody owns, and mount(8)'s exit
  status is never 0 unless dcfs serves" and known_bugs for the cases the
  three 15.2 review rounds found. Owner: dcfs-protocol (mount namespaces).

