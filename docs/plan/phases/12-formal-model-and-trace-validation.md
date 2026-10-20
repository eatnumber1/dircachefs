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

## 12.13c SANY-based TLA+ mutator (clerk batch 55, 2026-10-20; queued)

russ approved 2026-10-20: "Yes". After the Claude reset of Fri 2026-10-23.
12.13's TLA+ mode (`tools/mutation/mutate.py --lang tla`) is a
conjunct-aware token scanner, not a parser. Rebuild it on SANY, the parser in
`tla2tools.jar` that we already ship, per `docs/style.md`
`rule-authoritative-model` and `rule-survey-and-name-mechanism`.

0. Survey first: existing TLA+ or Apalache mutation tools, and SANY's AST
   and semantic API. Report what each would cost; the step names the
   mechanism it picks.
1. Same operators and report shape as today (negate, drop-guard,
   drop-conjunct, drop-disjunct, swap-junction, relational, constant,
   durability, drop-step, swap-step; arid rules and `equivalent.txt`).
   Tests first.
2. The scanner's unreachable cases become reachable: mixed-precedence
   chains and definitions not at column 0.

Owner: dcfs-implementer. Dependencies: none; 12.13b follows it.

## 12.13b model-mutation survivor sweep (clerk batch 55, 2026-10-20; queued)

russ approved 2026-10-20; after 12.13c. Run the full sweep (small and
medium tiers, `Trace*.tla` in scope). Each survivor gets a property or
config that kills it, or an entry in
`formal/equivalent.txt` with the reason. Start from the first run's nine
survivors listed in 12.13: TypeOK swap-junction (241), Serve swap-junction
(517), RDFromCode drop-step PD_read->PD_commit (651), S2 durability
FALSE->TRUE (1060), FBehind drop-conjunct `d.fAttr # b.f` (1503), Recover
durability FALSE->TRUE (1568), Next drop-disjunct UnlinkSyscall (1707) and
AttrChangeSyscall (1712), FileExactStrict drop-conjunct `mode = "up"` (1826).
The weekly `mutation-tla` job then fails on an unexplained survivor instead
of only reporting it.

Owner: dcfs-protocol. Dependencies: 12.13c.

## 12.28 TLC output streams to test.log (clerk batch 55, 2026-10-20)

Small tooling, M0; queued, then done. `third_party/tlaplus/` runner
`tlc_test_runner.sh.tpl` (lines 40 to 45) sends TLC's output to `$log` and
cats it only after TLC exits, so when Bazel times out a test, `test.log` is
empty. That happened in CI run 39380803978 on
79beb5fd: `//formal:interrupt_nolock_test` hit TIMEOUT at 3600 s with no
output, after passing in 160 s on 15676f30 and in 188 s locally on
79beb5fd, with no `formal/` change between those runs. The cause of the
hang cannot be found from the log.

1. Test first: a runner test whose TLC run is killed mid-run still leaves
   its progress lines in `test.log`. It fails on the current runner.
2. Fix: tee TLC's output to `$log` while it streams, keeping TLC's exit
   status (`pipefail` or `PIPESTATUS`).

Owner: dcfs-implementer. Dependencies: none.

Done 2026-10-20 (aeac8fa1, squashed from 72dbf108 and 66acb253; clerk batch
55): `test_first_tool` ACCEPTED; tools tier 51/51. The hang is logged in
`docs/plan/log.md` as a one-off runner hang, local rerun green.

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
  Its operation set includes open's flags as a client observes them
  (russ, 2026-10-17): each flag dcfs passes through (23.12's inventory:
  `O_NOATIME`'s owner rule and access-time effect, `O_APPEND`, `O_TRUNC`,
  `O_EXCL`, `O_NOFOLLOW`...) gets its allowed outcomes in the ideal spec.
- 12.18 SibylFS's trace checker as a second oracle (spike; russ,
  2026-10-10). Done 2026-10-11, merged 849f372. Report in `docs/plan/notes/sibylfs-checker-12.18.md`.
  Findings: source compilation is intractable under Bazel (ancient Lem/OCaml 4.02);
  prebuilt x86_64 binaries (2015-08-20 release) run against glibc and can be pinned,
  but are frozen snapshots. Fundamental impedance mismatch: FUSE requests with nodeids/64-bit fh
  do not map mechanically to sequential per-process path/fd traces; VFS caches elide FUSE calls;
  SibylFS's internal abstraction mandates synthetic metadata (st_dev=2049, st_ino=0..N, st_size=9999)
  and rejects real Linux ext4 traces without lossy normalization; no xattr, fallocate, or crash
  consistency modeling. Recommendation: discard SibylFS trace checker as an oracle; retain
  SibylFS strictly as a reading reference for 12.16's TLA+ refinement.

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


## 12.18b SibylFS as a differential second oracle (2026-10-16; after the re-verification)

From the independent re-verification (log, 2026-10-16): pin the 2015-10-05
prebuilt checker (x86_64; sha256
080c5bced08bbb34dedde43cdbd4f8fd3e1c5d678c60dff7067cc6f189208879) and the
test suite at sibylfs_test_suite 14f81504 (http_archive, not vendored: no
licence file in those repos; the source is ISC); a wrapper that turns the
checker's "# trace accepted" line into an exit status (it always exits 0); a
QEMU guest test that runs `fs_test_posix -r <fresh dir>` on a dcfs mount and
on the plain backing, checks both with `fs_test_check -arch linux --root`,
and fails only where dcfs is rejected and the backing accepted (the 2015
Linux model diverges from current kernels on a few adhoc cases). The source
build is the fallback (about 14 archives under a custom repository rule).
Licence correctness (russ, 2026-10-16): every pinned artifact's licence,
including what the prebuilt bundles, is found from primary sources and
recorded in third_party/sibylfs/README.md with how we use it (fetched,
test guests only, `testonly`, never vendored or shipped); anything
unlicensed or unclear goes to russ, not decided by the agent.
Follow-up: a mechanical check of 12.16's Ideal.tla against the checker
(bounded TLC behaviours rendered as SibylFS traces). Investigator.

## 12.16 amended: verify against SibylFS, then retire it (russ, 2026-10-16)

russ agreed to the chain (12.18b's differential oracle now; 12.16's
syscall-level Ideal.tla as the top-level model; a TLA+ model of the
kernel's FUSE client in the middle, so dcfs's protocol composed with it
refines Ideal.tla), with one change: "let's mechanically verify it against
SibylFS, and then once we can prove it completely covers everything in
SibylFS, let's delete SibylFS. No need to carry around that code when we've
proven our model covers everything SibylFS does." So 12.16 ends with:
1. A syscall-trace checker built from Ideal.tla (TLC trace validation, the
   same shape as Trace.tla, over syscall traces in SibylFS's format or a
   converted one).
2. Agreement, checked mechanically in both directions on the operations
   SibylFS models: every trace in the SibylFS test suite gets the same
   verdict from both checkers (accepted or rejected, on the same Linux
   flags), and bounded TLC enumerations of Ideal.tla's behaviours rendered
   as traces are all accepted by SibylFS, with mutants of each rejected by
   both. "Covers everything" is defined by that agreement over the whole
   suite plus the enumeration bound, stated in the step (it is bounded
   checking, not a proof; the bound is chosen and recorded).
3. Then the retirement: 12.18b's differential guest test switches to the
   Ideal.tla checker, and the SibylFS pins (checker binary, test suite
   archive, wrapper) are deleted in the same change; the SibylFS test suite's
   scripts may stay as a pinned corpus if they still feed the guest test,
   without the checker.

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **12.16a** `formal/Ideal.tla` and the refinement mapping, before 25.10a and
  without the FUSE-client model (12.18b is merged and is needed only by
  12.16b; the kernel's caches are 12.20, which follows this step). Slices:
  - a1: rename, the hardest guard, plus the expected-red config
    `formal/limitations/atomicity_eagain.cfg` and a TLC test that expects its
    counterexample (with its length). Red today because `dcfs.tla` lets
    unlink, rename and readdir reply EAGAIN after bounded retries and the
    ideal outcomes have no EAGAIN; 25.10a removes the branches, retires the
    config into the model and turns the property green. First action: show the
    red config reachable under the current `Procs` and `MC_*` bounds, adding
    the smallest config that reaches it. If russ declines expected-red
    configs on main, a1 lands on a branch with 25.10a as red then green
    commits.
  - a2: unlink, create, mkdir, link, setattr. Prerequisite: a1.
  - a3: lookup, getattr, readdir (weak: entries added or removed during a
    readdir may appear or not, so 22.7 needs no amendment), open flags, sync
    kinds. Prerequisite: a1.
  Prerequisite: the reading-reference pin of SibylFS's Lem definitions.
  Out of scope: atomicity of a composite syscall (LOOKUP then UNLINK under the
  inode lock), which 12.16b and 12.20 cover. Milestone M1.
- **12.16b** the syscall-trace checker from `Ideal.tla`, agreement with
  SibylFS in both directions, retirement of the pins. Prerequisites: 12.16a,
  12.18b (done), 12.20a, 22.7 (so the readdir semantics and the kernel layer
  are final). Milestone M2.

## 12.19 No stronger than the backing: pinned non-guarantees (russ, 2026-10-17; queued)

russ: "Maybe we should make the formal model include guarantees around
*non* durability? E.g. cases where functions are not guaranteed to be
durable, perhaps we should test that they in fact are not? ... we need
something stronger" against overzealous safety. 25.28b introduces the
pattern for fsync: for an outcome the backing's crash regime permits (a
loss, a reordering), a config whose invariant forbids it and whose test
expects TLC's counterexample, so dcfs adding a barrier (a syncfs, a
synchronous commit) makes the outcome unreachable and fails the test. This
step extends it to every operation in the litmus set (create, rename,
unlink, setattr, write, fsyncdir) and to the cost side: each request's
backing barriers and cache fsyncs budgeted exactly in
`syscall_budgets.txt`, so an extra one fails. Owner: dcfs-protocol, after
25.28b.

## 12.20 The kernel's caches in the model (russ, 2026-10-17; queued)

russ, on the attr/entry timeouts: "this kind of subtle cache invalidation
interaction with the kernel needs to be part of the formal model." The
model has dcfs's cache, the backing and the requests; it does not have the
kernel's dentry, negative-dentry and attribute caches between the client
and dcfs, which serve a client without asking dcfs until their timeout
expires or dcfs invalidates them.

- **State:** what the kernel holds for each name and inode (an entry or
  attributes, with a validity that is either "until invalidated" or
  expires on the model's clock), set from dcfs's replies.
- **Actions:** a client served from the kernel's cache with no request;
  expiry; dcfs's invalidations (`notify_inval_entry`, `notify_inval_inode`,
  `notify_delete`); every place dcfs changes or learns something behind
  the kernel's back (the inventory below).
- **Property:** what a client observes through the kernel is what 12.16's
  ideal filesystem allows (or, before 12.16, what the backing holds where
  the cache is complete), for any timeout, including an effectively
  infinite one, given the invalidations dcfs makes; a change with no
  invalidation is a counterexample whose length shows the staleness it
  causes. A `known_bugs/` variant drops one invalidation.
- **Input:** `notes/kernel-cache-invalidation-2026-10-17.md`: of 38
  candidate places, the kernel's own FUSE code covers 29, 7 are
  out-of-band, and 2 are real (ctime after FS_IOC_SETFLAGS; MAP_SHARED
  stores after close). The model's kernel actions include what the kernel
  invalidates on its own, so those 29 are not counterexamples.

- **Cost invariants:** per-request counters of the model's backing calls
  and commits, with invariants bounding them (a lookup makes at most one
  backing call; no request iterates the whole dirty set), checked by TLC
  in the model's configurations.

- **Entry expiry uses `FUSE_EXPIRE_ONLY`** (russ, 2026-10-18, approved).
  Expire-only is the default and expected form of entry invalidation: dcfs
  expires the dentry's timeout, the next walk revalidates with a LOOKUP,
  and the kernel invalidates the entry itself if dcfs answers gone or
  different. Verified in the kernel tree (v7.3-rc7-245-g32c52ec87d62):
  `FUSE_EXPIRE_ONLY` is protocol 7.38
  (`include/uapi/linux/fuse.h:202`); the flag is read in
  `fuse_reverse_inval_entry` (`fs/fuse/dir.c:1628`) and the kernel
  advertises `FUSE_HAS_EXPIRE_ONLY` (bit 35, `fs/fuse/inode.c:1469`), which
  dcfs must see before it sends the flag. Lock: `fuse_reverse_inval_entry`
  takes `inode_lock_nested(parent, I_MUTEX_PARENT)` before it tests the
  flag, so expire takes the parent lock too; the earlier deadlock finding
  applies to expire as well as to full invalidation.
  Full entry invalidation (`d_invalidate`: immediate drop, subtree prune,
  detaches mounts on the name) is used only where the model or a test shows
  the immediate drop is needed. If none does, dcfs never calls it (no dead
  code). Mounts on a name (23.13's bind over a stub) must survive every
  notification dcfs sends. Inode invalidation (attributes and data, e.g.
  the chattr case's synchronous attribute invalidation) is separate and
  unchanged.

- **Handle opens go by identity, not names** (russ, 2026-10-18: "does
  open_by_handle need entry invalidation?"). `open_by_handle_at` reaches
  an object through the export operations by nodeid and generation, not
  by name, so entry expiry and invalidation do not affect it. The kernel's
  handle-decoding path checks the generation: `fuse_fh_to_dentry`
  (`fs/fuse/inode.c:1132`) calls `fuse_get_dentry` (`fs/fuse/inode.c:1054`),
  which answers ESTALE when `inode->i_generation` differs from the handle's
  (`fs/fuse/inode.c:1085`); verified at v7.3-rc7-245-g32c52ec87d62. Guest
  test (native differential): `open_by_handle_at` on the dcfs mount after a
  removal, a rename and an inode-number reuse gives the same results as
  native, ESTALE where native gives it. If the kernel did not check the
  generation, pause and bring it to russ (the same condition as the 12.24
  symlink check).

Owner: dcfs-protocol, after 25.28b and 22.5 (all three touch dcfs.tla).

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **12.20a** state, actions, property and known-bug; two red cases; the
  native `open_by_handle_at` differential test first. Prerequisites: 12.16a
  (the property is stated against the ideal), 12.27 items 1-3, 25.28b and
  22.5 (done), 24.2 (so the tests are first written on the 6.12 floor).
  Milestone M1.
- **12.20b** adopt `FUSE_EXPIRE_ONLY` and the handle-open test (code).
  Prerequisite: 12.20a.
- **12.20c** the cost invariants. Prerequisite: 12.20a.

## 12.21 The model's environment assumptions, registered and enforced (russ, 2026-10-17: "I agree with your proposals"; queued)

Found because russ asked whether the model covers signal delivery: the
model starts a clean shutdown only with no request in flight
(`BeginShutdown`), and the code did not enforce that (25.10's signalfd
will). Nothing checks the model's guards against the code.

- **The register:** every guard or abstraction in `dcfs.tla` that claims
  something of the environment (shutdown only when idle, requests never
  overlap, the kernel waits for a reply after FUSE_INTERRUPT, a backing
  syscall succeeds or fails cleanly, ...), each with its enforcement: a
  trace-validation check, a code assertion or a test. An entry without
  one is a finding. Kept in `formal/README.md`.
- **First draft by spec-to-code tracing,** run as a measured experiment
  (process.md rule 5): Gemini Flash as about six narrow jobs (one per
  group of actions) and Argon as one broad job, on the same frozen commit;
  about six guards labelled beforehand (BeginShutdown unenforced; no
  overlap, interrupts only at interruption points, no interruption point
  between a syscall and phase 3 enforced, ...); scored on the known
  answers, precision (each finding verified by its quote), recall
  against the union of verified findings, citation accuracy, and quota
  spent per job (`agy -p "/usage"` before and after). Results in
  `notes/gemini-sweep-2026-10-17.md`.
- **The abstraction map, checked (code side; russ, 2026-10-17: "Yes"):**
  every backing call the code makes (`ProtocolEvents::BackingCall`) must
  be attributed by trace validation to the model step it belongs to,
  within that step's declared maximum (a create's `C_probe`: ProbeChild's
  statx plus one getxattr per xattr; `PopulateRead`: a bounded number of
  getdents64 calls). A call no step covers, or more calls than a step
  declares, makes the trace invalid. So the map checks that each action
  stands for bounded work (a constant number of backing syscalls,
  independent of directory or file size; an action that stands for a loop
  is split or recorded as a finding: `formal/README.md`, "Changing the
  model"). This is what would have caught 22.5's
  getdents64 loop and readdir's per-entry refreshes, which the model's
  granularity hid. The register also lists the model's abstractions
  ("PopulateRead is one step", "D has no parent", "a reply carries no
  child attributes") with the code paths each hides; a property stated per
  step needs a code-level test for each hidden path (as 22.5's harness
  sweep does for the interrupt bound).
- **Guard mutation (12.13):** a mutant class that removes or weakens one
  guard at a time; a load-bearing guard must be in the register with its
  enforcement, a guard whose removal breaks nothing is deleted.
- **Signal sweep (26.6):** SIGTERM/SIGHUP to the daemon at each backing
  call site in a guest test, alongside the syscall-fault sweep.
- **Kernel facts the register records (2026-10-18, from 23.15):**
  - "On a non-passthrough FUSE file, the kernel turns an O_SYNC, O_DSYNC or
    RWF_*SYNC write into a FUSE_FSYNC (generic_write_sync)." Entered once
    23.15 verifies it; enforced by 23.15's differential test.
  - "Passthrough I/O honours the open flags of the FUSE file at open time"
    (fs/fuse/passthrough.c:169-171). Enforced by the 23.15 audit's test for
    each flag on the passthrough path.

## 12.22 Worst cases nobody listed: exhaustive and guided search (russ, 2026-10-17: "I want to do it, but put it after M3. Let's deploy to my machine first.")

Prior art: PerfFuzz (Lemieux et al., ISSTA 2018), SlowFuzz (Petsios et
al., CCS 2017), Singularity (Wei et al., FSE 2018), WISE (Burnim et al.,
ICSE 2009), Badger (Noller et al., ISSTA 2018); the small scope hypothesis
and bounded exhaustive testing (Jackson's Alloy; Korat, TestEra).

1. **Model, exhaustive:** 12.20's per-request cost counters (added in
   12.20; the worst-case search over them waits for this step); TLC visits
   every reachable state within the bounds, so an invariant `cost <=
   f(size)` is proved there or returns the worst state, which becomes a
   named worst-case scenario.
2. **Code, exhaustive at small sizes:** every tree and short operation
   sequence up to a small bound through the in-process harness (forged
   requests), exact counts recorded, the maximiser kept per size.
3. **Code, guided search at larger sizes:** a PerfFuzz-style loop over
   operation sequences with the exact counts (requests, syscalls, SQL
   steps, per-function coverage counts) as its objective.
4. Every family found joins 26.41's exact-collinearity slope tests, next
   to each operation's typical family and a hand-listed worst-case family.

## 12.23 A static cost analyser on dcfs's code: an Infer spike (russ, 2026-10-17: "Yes"; after M1)

Infer's cost analysis (Meta) computes symbolic complexity bounds per
function and reports a diff that raises one (EXECUTION_TIME_COMPLEXITY_
INCREASE). Pin it through Bazel, run it on `dcfs/` with models at the
boundary: each backing syscall a cost of its kind (26.41 1a), each SQL
statement O(log n) per row once 26.41's query-plan check holds, and see
what bounds it proves for the main request paths. Expected obstacles: its
C++ maturity next to Java, and dcfs's virtual calls through the Syscalls
interface (models or devirtualisation). If it works, a differential check
in CI: a diff that raises a function's complexity fails. Owner:
dcfs-investigator.

## 12.24 Backing-contract audit follow-ups (russ, 2026-10-17; queued, M1)

The first pass is `notes/contract-audit-2026-10-17.md` (about 45 of about
200 claims). Items below cite its ids.

- Finish the audit: the remaining ~155 claims, `formal/limitations/` and
  `formal/no_stronger/`. Verify the POSIX readdir sentence (F1) against the
  POSIX text itself, not the man page.
- F1 and S1 (listing snapshot, FALSE/STRONGER): fixed by cf4a100 and 22.7.
- S2: `formal/README.md:641-643` says fsync is followed by syncfs. That
  contradicts russ's ruling that FSYNC is the backing fsync plus a WAL
  fsync, with no syncfs. Check whether the text predates 25.28b and
  correct it.
- B1, B2 (invented EAGAIN and the retry loop in ListCached): removed by 22.7.
- B3, B4 (first stat pays for the whole directory; "all I/O, then one
  transaction"): 22.7.
- B5 (one thread serves one request, so a stalled backing blocks cache
  hits): already decided, coroutines + io_uring (required, no fallback;
  docs/plan/README.md future work, the io_uring phase).
- B6 (FUSE_CAP_ATOMIC_O_TRUNC off, so open(O_TRUNC) takes two backing
  steps where the backing takes one): decided, turn it on (russ,
  2026-10-17: "Yeah!"; follows from behaving like the backing). Open()
  then carries the truncate as one backing open(O_TRUNC): it becomes a
  mutation (tri-state phases for the size, killpriv as for a truncate),
  the model covers it, and the comment at dcfs/dir_cache_fs.cc:251-262
  goes. Agent: dcfs-protocol.
- B8 (unreachable same-inode rename branch): remove it or make it a
  RET_CHECK that names the invariant (style 1.10a).
- Unbounded working memory M1-M12: 22.7 covers ReadDirNames and
  PopulateDirectory. Still open: PruneDentriesNotIn (M3), ListDirty,
  BeginSync and ClearDirty paging (M6), `stubs_logged_` (M7), and the
  per-inode maps (M7).
- UNSOURCED U1-U7: each gets the native differential test named in the
  note, run without dcfs, before any claim it backs is kept.
- Model "no over-fetch" (russ, 2026-10-17, from docs/design.md "Fetch the
  minimum needed to serve the request"; russ: "Is there a way to
  mechanically enforce don't cache what the user didn't ask for? Maybe
  something in the model?"). A ghost demand per in-flight request: lookup
  X demands X; readdir demands its listing pages; getxattr k demands
  (object, k); readdirplus demands its page's entries. Invariant: every
  backing read action targets its own request's demand, and no backing
  read occurs outside a request (this catches background or start-time
  probes); a fill records only what a read for that request returned.
  Trace validation (12.2) then enforces it on the code; 26.41's exact
  per-operation syscall counts are the backstop. Decided (russ,
  2026-10-17): lookups are included; a lookup resolves one name and never
  populates the directory (22.7 item 8), so the property covers lookups
  too.
- Costly fields (russ, 2026-10-17: "non-free field masks sounds like
  nuance that needs to make it into the model"): backing read actions
  carry the fields they request (e.g. statx's mask); a request's demand
  includes the costly fields it needs. The property: a costly field (e.g.
  STATX_DIOALIGN) is requested only when the request needs it; free fields
  always come back and are always cached.
- Xattr population stops prefetching: values are read on demand, and the
  name list is cached only on listxattr (russ, 2026-10-17).
- Per-filesystem xattr limits (decided 2026-10-17 from the kernel source,
  notes/contract-audit-2026-10-17.md "Xattr limits per filesystem"): names
  need nothing; values pass through. Native differential test: a 64 KiB
  setxattr through dcfs on xfs succeeds, and on ext4 (no ea_inode) fails
  with the same ENOSPC as native; a test that dcfs's FUSE read buffer holds
  a 64 KiB SETXATTR.
- NAME_MAX (found 2026-10-17, missed by the audit): `LookupOrPopulate`
  (dcfs/backing.cc:1549-1564) rejects names over NAME_MAX, citing it as "the
  generic Linux VFS limit"; the kernel says otherwise (fs/readdir.c:151-153:
  PATH_MAX is "the real kernel limit on a possible path component, not
  NAME_MAX"; FUSE allows FUSE_NAME_MAX = PATH_MAX - 1, fs/fuse/fuse_i.h:45).
  Behave like the backing: use the backing's own limit, statfs(2) f_namelen,
  read at mount. Decided (russ, 2026-10-17: the hybrid) and done by "12.24:
  names longer than the backing's f_namelen go to the backing": a name of at
  most `ctx.name_max` (f_namelen, read by `InitRoot`) is answered from the
  cache as before; a longer one skips the cache and is resolved alone on
  the backing (ENAMETOOLONG on ext4, xfs and btrfs, recorded nowhere);
  `formal/dcfs.tla` LKFrom allows that path.
  Tests: `NameLimitTest` in dcfs/backing_test.cc (a fake f_namelen of 16);
  no vfat differential test (no vfat mkfs in test/qemu). Per-filesystem
  xattr limits stay deferred. **Done 2026-10-18:** 32ea4ab (the hybrid)
  and 2552c4a (LongNames in the model, with the upstream citations).
- cpio (found 2026-10-17): `.github/ci/prepare.sh:40` installs `cpio` on the
  CI host, but nothing uses it: `test/qemu/scripts/mkinitramfs.sh:333`
  packs the initramfs with the pinned busybox's own `cpio` (the host one was
  the failure of Phase 5.2's act run). Work: remove `cpio` from
  `HOST_PACKAGES` and its comment at `prepare.sh:29`, and check CI still
  passes (code change, with the CI check).
- ZFS (2026-10-17): on Linux the VFS caps names (255) and values (64 KiB)
  before ZFS sees a request (fs/xattr.c:615-620, 641-643, 797-815). A pool
  holding a larger value (written by another OS; ZFS's own limits
  unverified: no OpenZFS source here) reads back as E2BIG. Today
  GetXattrOPath (dcfs/backing.cc:179-196) fails population on it, so one
  such file makes the whole directory unlistable through dcfs while the
  backing lists it. Fetching xattrs on demand (russ's no-prefetch ruling)
  fixes it: only the client asking gets E2BIG, as from the backing.
- ListXattrOPath and GetXattrOPath (dcfs/backing.cc:159-196) retry up to 4
  times on ERANGE, a retry loop contrary to russ's 1.12 ruling
  ("ERANGE-on-races with the backing store, then ERANGE-on-races for us").
  With on-demand xattrs a client's getxattr/listxattr passes its own size to
  the backing and returns the backing's ERANGE unchanged; the loops go.
- **Symlink identity and `READDIRPLUS_AUTO`** (russ, 2026-10-18).
  `CACHE_SYMLINKS` is on today (`dcfs/dir_cache_fs.cc:241`). Question: can a
  cached target survive when dcfs reports a reused nodeid with a new
  generation? Verified in the kernel tree (v7.3-rc7-245-g32c52ec87d62): no.
  `fuse_iget` compares the generation through `fuse_stale_inode`
  (`fs/fuse/inode.c:496-509`, `fs/fuse/fuse_i.h:847`); on a mismatch it
  marks the old inode bad, unhashes it and retries, so the new nodeid gets
  a fresh inode and a fresh readlink. `fuse_get_link` fails EIO on a bad
  inode (`fs/fuse/dir.c:1870-1872`). Only LOOKUP and READDIRPLUS make this
  check (`fs/fuse/dir.c:453`, `fs/fuse/readdir.c:212`); GETATTR and SETATTR
  replies carry no generation (`struct fuse_attr_out`,
  `include/uapi/linux/fuse.h:742-747`). So the kernel distinguishes a reused
  nodeid by generation on lookup, and dcfs needs no
  `notify_inval_inode` for a new generation. Guest test: recycle an inode
  number between two symlinks with different targets; the second target is
  read fresh. If that test fails, stop and bring it to russ before changing
  dcfs (russ, 2026-10-18: "I'll be surprised if it doesn't use the
  generation. If you find that it doesn't, pause and let's discuss.").
  russ, 2026-10-18, on setting `READDIRPLUS_AUTO` explicitly: "No. Rely on
  defaults, but test that our relied-upon defaults are true. That way prod
  code is minimized while still having protections." So dcfs leaves
  `READDIRPLUS_AUTO` at libfuse's default, as it leaves the other libfuse
  defaults it relies on (ASYNC_READ, AUTO_INVAL_DATA, ...), and a guest
  test asserts each relied-upon default's value in the negotiated
  connection.
- **Merge record (2026-10-18):** the name-length hybrid was cherry-picked to
  main as 32ea4ab and 2552c4a, with the conflicts in `docs/design.md` and
  this file resolved by keeping main's text and adding the branch's "Name
  limits are the backing's" bullet and "Decided ... done" paragraph. The
  full `//formal/...` gate ran 206 of 209 passing, with 3 TIMEOUTs:
  crash_ext4_test at its 300 s moderate limit, and nolock_test and
  large_test at 3600 s. Load was about 55, and every config's state space
  was unchanged (small_test 1,479,307 states and interrupt_test 280,869,
  identical; LongNames = {} adds only an empty-set test). Merged on that
  evidence; 12.25 addresses the load.

## 12.25 TLC tests declare their resources (russ, 2026-10-18: approved; queued)

Four TLC JVMs ran at `-workers auto` with 8 GC threads each, while Bazel
counted each as 1 CPU. That drove the load to 55 on 8 cores.

- `third_party/tlaplus/tlc.bzl` and `tlc_test_runner.sh.tpl`: a fixed
  `-workers` per target class instead of auto (the default is auto at
  tlc.bzl:82-88), with a matching `cpu:N` tag.
- `-XX:ParallelGCThreads=N` to match the workers.
- An explicit `-Xmx`, with a matching `resources:memory` tag. Today no
  `-Xmx` is passed, so the JVM default (about 15.5 GB) applies.
- The numbers come from the time analysis's TLC scaling measurements
  (`docs/plan/notes/time-analysis-2026-10-18.md`, section 4.1). Classes, with
  GC threads equal to the workers in every class:
  - Classes, revised 2026-10-18 from the low-load measurement (below), which
    supersedes the loaded numbers this step first recorded:
  - default (every target not listed below, 50 k to 2 M distinct states):
    2 workers, `cpu:2`, `-Xmx1g`, `resources:memory:1300` (measured: 512 MB and
    1 GB pass at 0.67 to 1.05 GB RSS).
  - small (under 100 k distinct states, as the time analysis counted them):
    1 worker, `cpu:1`, `-Xmx512m`, `resources:memory:700` (measured at 512 MB:
    0.37 to 0.65 GB RSS).
  - largest (large_test and nolock_test, 6 M to 10 M states; only configs near
    their timeouts get 4 workers): 4 workers, `cpu:4`, `-Xmx3g`,
    `resources:memory:3500` (not re-measured; lane-5's 2.66 GB run of
    2026-10-07 passed).
- Findings, loaded machine (superseded by the low-load measurement below): CPU
  per state was taken as flat across worker counts (interrupt_test 293 to
  325 CPU s for 1 to 8 workers). The gain claimed was about 80 % less TLC
  memory (the default heap is 14.3 GB; RSS 5.7 to 6.2 GB per JVM) and honest
  CPU accounting in Bazel. CI wall is unchanged: large keeps 4 workers, equal
  to a CI runner's vCPUs.
- Low-load measurement (2026-10-18; Gemini Flash via foreman; load under 4
  before each run; nproc 8; full tables in
  `docs/plan/notes/tlc-scaling-lowload-2026-10-18.md`):
  - CPU per state rises with workers at low load: MC_interrupt 0.32 ms per
    state at 1 worker, 0.64 at 8; liveness 0.84 at 1 worker, 1.41 at 8. This
    corrects the loaded finding above, which said flat. Machine-wide throughput
    is therefore best with few workers per checker and more checkers in
    parallel (russ's aim): 2 workers give about 1.8 times the wall speed-up for
    about 1.1 times the CPU; 8 workers give 3.2 times for 2 times the CPU.
  - `-Xmx` from 512 MB to 4 GB never slowed TLC (512 MB and 1 GB were
    marginally faster) and cut RSS from about 5.6 GB to 0.65 to 1.2 GB (safety)
    and from about 4.3 GB to 0.37 to 0.5 GB (liveness).
  - The measurement run is not a step and is done.
- No test is weakened: the same configs and the same state counts.
- Measure on a quiet machine before choosing classes (russ, 2026-10-18):
  states per second at 1, 2, 4 and 8 workers, and run time with and without
  the `-Xmx` limits. Fewer workers per checker is wanted even at equal CPU
  per state, because Bazel can then run more checkers in parallel (russ).

Owner: dcfs-implementer. The time analysis reported on 2026-10-18; the numbers
above are its measurements.

## 12.26 Faster model checking without weaker checks (russ, 2026-10-18: "Is there anything we can do to speed up model checking?"; queued, not started)

Each item keeps every config's premises and known_bugs variants; none trades
a check for speed.

1. **SYMMETRY on the large safety configs.** Only 18 of 185 configs declare
   it. For each large safety config (large, nolock, interrupt_*, small,
   crash_*), add symmetry over truly interchangeable sets (Procs; the Names
   classes). Never in liveness configs: TLC's liveness checking is unsound
   under symmetry. Evidence per config: the state count drop, every known_bugs
   variant still found, premises still pass. Owner: dcfs-protocol.
2. **Profile and rewrite the hot operators.** Profile with TLC `-coverage` and
   rewrite the costliest definitions. State counts stay unchanged, and the
   evidence is the same count before and after. Owner: dcfs-protocol.
3. **Measured TLC classes.** Apply 12.25's low-load classes (2 workers and
   `cpu:2` with `-Xmx1g` by default; small configs 1 worker with `-Xmx512m`;
   only the largest configs near their timeouts get 4 workers). Owner:
   dcfs-implementer.
4. **Pre-review runs.** Agents' pre-review runs: small and medium configs run
   exhaustively; large configs get a bounded `-simulate` pass; exhaustive large
   runs stay in CI. Owner: dcfs-implementer.
5. **Later: split `dcfs.tla`.** Split it so each config depends only on the
   modules it uses, so Bazel's cache skips configs a change does not affect.
   Not started; after items 1 to 4.
6. The soundness check for config shortcuts (the `CacheDurable` argument) moved
   to 12.27, "Unit tests of the spec", item 10.

Russ's question is answered by items 1 to 4; item 5 waits for them.

## 12.27 Unit tests of the spec: mechanical checks on the TLA+ model (russ, 2026-10-18: "Are there more properties of the model we can check mechanically? Essentially this becomes unit tests of the TLA+"; queued, not started)

Each item is a Bazel test under `//formal/...`, mostly SANY-based (the parser
in `tla2tools.jar`). Items 1 to 3 come first.

1. Every `.cfg` for a module assigns every CONSTANT. This would have caught
   22.5's configs lacking LongNames (CI run 39138000104).
2. Frame completeness: every action primes or lists `UNCHANGED` for every
   variable.
3. No orphaned properties: every invariant and temporal property defined is
   checked by some config, and every name a config references exists.
4. Vacuity: for each `P => Q` property, a config asserting `~P` expects a
   violation.
5. Spec mutation testing: drop a conjunct, weaken a guard, or drop an
   `UNCHANGED` entry, and require some config to fail (like the weekly code
   mutation run). Covered by 12.13b (the survivor sweep) and 12.13c (the
   SANY-based mutator), clerk batch 55, 2026-10-20.
6. Every disjunct reachable: extend `formal/coverage` to branches and fail on
   zero hits.
7. Model-code name agreement: `InterruptPcs` against the code's
   `InterruptionPoint` names, and the other mapped sets.
8. Symmetry soundness, once 12.26 adds SYMMETRY: no property names an element
   of a symmetric set.
9. The state counts in `formal/README.md` are generated from the last run, not
   hand-edited.
10. The `CacheDurable` soundness check (moved from 12.26 item 6; russ,
    2026-10-18: "Doesn't the parser-based check already cover this? Running two
    additional TLA+ runs sounds expensive"). The argument for omitting
    `CacheDurable` in `MC.tla` (used by `MC_large`, `MC_nolock`,
    `MC_interrupt_nolock`, `MC_interrupt_muts2`) is checked by a Bazel test that
    parses `dcfs.tla` with SANY and fails with the offending line when either
    part does not hold: (a) `CacheDurable`'s `UNCHANGED` covers every variable
    except `dbOpts`; (b) every occurrence of `dbOpts`, following operator
    definitions transitively, sits in a positive position under
    `\A x \in dbOpts` (no `\E`, no negation, no equality or cardinality). Any
    occurrence the check cannot prove to be a positive universal read (for
    example, passed as an operator argument, or inside `LET` or `RECURSIVE`)
    fails the test with its line, and the spec is rewritten to the plain form.
    The check fails closed. There is no CI backstop and no paired
    with/without runs. The same pattern covers any config shortcut that omits a
    step on a soundness argument.
11. The model includes FUSE passthrough (russ, 2026-10-18: "make sure tests
    and the formal model would have caught that"). An open that the kernel
    grants passthrough registers a backing file, and the kernel then never
    sends READ or WRITE for that handle. The model says exactly which opcodes
    still reach the server on a passthrough handle: READ and WRITE do not
    (v6.12 fs/fuse/passthrough.c covers read_iter, write_iter, splice_read,
    splice_write and mmap only), while FALLOCATE, COPY_FILE_RANGE, SETATTR,
    FSYNC, FLUSH and the rest do. 25.46's recording FUSE server pins that
    opcode list as a kernel fact.
12. Branch reachability: this check reports any model action that can never
    fire. A dcfs handler whose model action is unreachable is dead code. The
    actions map to handlers by item 7's model-code name agreement. Under item
    11, the READ and WRITE handlers are unreachable for a passthrough handle,
    and FALLOCATE and COPY_FILE_RANGE are reachable. A stub of a reachable
    handler then shows up as a trace-validation or conformance divergence.
    The unreachability rests on 25.46's kernel fact that no READ or WRITE
    reaches the server for a passthrough-opened handle.

Owner: dcfs-protocol for items 5, 7, 11 and 12; dcfs-implementer for the
others.

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **12.27a** items 1 to 3.
- **12.27b** items 4 to 6, 9 and 10.
- **12.27c** items 7 and 8. Prerequisite: 12.26 (SYMMETRY).
- **12.27d** items 11 and 12. Prerequisite: 25.46b.
