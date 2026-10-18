# Phase 22 — Request cancellation (Ctrl+C, EINTR)

**Decision (russ, 2026-10-06).** dcfs supports cancelling requests: a
process interrupted while waiting on dcfs (Ctrl+C, a signal, a timeout)
gets EINTR promptly, and any slow operation dcfs was doing for it stops.
Cancellation is a stated goal even if today's operations turn out to be
fast: future slow operations must support it, and the design must carry
over to the coroutine + io_uring future.

**Mechanism.** The kernel sends FUSE_INTERRUPT for a request whose caller
got a signal. libfuse exposes it as `fuse_req_interrupt_func()` (a callback
when the request is interrupted) and `fuse_req_interrupted()` (poll). A
request answered after interruption should reply EINTR (or complete, if it
already changed state that cannot be undone).

## 22.1 Inventory (measure, decide)

- Measure the worst case of every request path on a slow backing
  (dm-delay from Phase 10 plus a spun-down-disk-like latency, cold cache,
  large directories): lookup/getattr on a miss, population of a 100k-entry
  directory (readdir + statx per entry + SQLite), readdirplus of a huge
  cached directory, create/unlink/rename (three phases, backing syscall,
  durable commits), fsync/fsyncdir and the sync point's syncfs after large
  writes, startup recovery of a large dirty set, open of a large file
  (passthrough setup). Reads and writes themselves are kernel passthrough
  (cancellable by the kernel, not dcfs).
- Classify: anything that can exceed about 100 ms on a slow disk is
  "slow" and must be cancellable at safe points; the rest is "fast" and
  needs no checks. Expected slow set: directory population, large
  readdirplus fills, recovery; and syscalls that block in the kernel
  (syncfs, fsync, a cold open), which dcfs cannot interrupt but should not
  start for an already-interrupted request.
- If the measurement shows everything is fast, say so with numbers; the
  rest of the phase still delivers the policy, the tests of the
  interruption path, and the docs.

## 22.2 Semantics (dcfs-protocol decides, with the model)

- Safe points: between entries of a population loop, between SQLite
  batches, before each backing syscall. At a safe point an interrupted
  request stops and replies EINTR.
- The tri-state rule holds through cancellation: an aborted population
  leaves the directory incomplete with the entries probed so far present
  (sound); an aborted fill must release its fill guard; a mutation is
  NOT cancellable between its backing syscall and phase 3 (the backing
  change exists; finish and reply success), but is cancellable before the
  syscall (undo phase 1: the name returns to unknown, which is sound).
- Single-threaded today: libfuse delivers FUSE_INTERRUPT as its own
  request, read by the loop only after the current request's handler
  returns, so a long handler never sees the interrupt unless the loop
  processes incoming requests at safe points. Decide: (a) call
  `fuse_session_process_buf`-style draining at safe points, or (b) accept
  that today only the "already interrupted on arrival" case is handled
  and make the safe points real with coroutines. The decision and its
  reason go in docs/design.md; the model gets an `Interrupt` action for
  whichever is chosen (cancellation never violates CacheNeverWrong or
  TriState).
- Coroutines + io_uring: a per-request cancellation token checked at
  every await; `IORING_OP_ASYNC_CANCEL` for in-flight backing I/O; the
  same safe-point rules. Written down now so the rewrite inherits it.

## 22.3 Tests first

- Unit (`dir_cache_fs_test`, forged requests): a request marked
  interrupted before dispatch replies EINTR without touching the backing
  (fake backing records calls); a population interrupted at a safe point
  (the `--wrap` hook raises the interrupt mid-loop) stops, leaves the
  directory incomplete and consistent (cache checker), releases the fill
  guard, and the next request completes the population; a mutation
  interrupted after its syscall still completes and replies success; one
  interrupted before its syscall leaves the name unknown and the backing
  untouched.
- Guest: on a dm-delay backing with a 100k-entry cold directory,
  `timeout -s INT 1 ls /mnt/huge` exits promptly (within 2 s) with EINTR
  visible to `ls`, dcfs keeps serving other requests during and after,
  and the checker passes; the same with Ctrl+C semantics via `kill -INT`
  on a blocked `find`. Failing first on today's dcfs (it should hang until
  the population finishes).
- Benchmarks: the inventory's numbers become a `bench_full` table so a
  later slow operation is noticed.

## 22.4 Docs and rules

- docs/design.md: "Cancellation" section (goal, semantics, safe points,
  the coroutine plan); README: EINTR behaviour.
- AGENTS.md: any operation that can take more than ~100 ms on a slow
  backing checks for interruption at safe points and has a cancellation
  test.

Owner: dcfs-protocol (22.2, 22.3's unit tests), dcfs-investigator (22.1),
dcfs-implementer (guest tests, docs). Order: after R4 merges (it touches
the same request paths), before Phase 13.

## Note from the readdir step (2026-10-08)

Since the readdir reductions, children's attributes are read in the
listing statement (`ListDir`'s join), not per entry when it is served.
Under checkpoints this changes nothing (no interleaving inside a request
today); under coroutines, a mutation interleaving during an earlier
entry's refresh no longer makes a later entry's attributes unknown before
they are served; the kernel discards readdirplus attributes older than a
completed modifying op on the same mount (attr_version), so this is safe,
but the coroutine design should re-check it. See docs/design.md "Readdir".

## As built (2026-10-08, step-22, pending its review fixes)

Option (b): synchronous checkpoints, single-threaded. Deviations from
the text above: an interrupted population commits nothing (22.2 said
"probed entries present"): the directory stays incomplete and the next
readdir completes it; the guest test uses a 20,000-entry directory (not
100k); the inventory (notes/cancellation-inventory-2026-10-08.md) is its
own enormous test, not a bench_full table. Review (merge after fixes):
a refused INIT must exit non-zero (SessionLoop cannot see libfuse's
private se->error: detect FUSE_INIT + fuse_session_exited); rename's
double re-resolve stops at the first EINTR; Drain stops after the first
non-INTERRUPT message so queued requests stay kernel-cancellable;
FUSE_CAP_SPLICE_READ unset; a parameterized cancellation test over every
checkpoint; cancel_test's bound scaled by the accelerator; AGENTS.md
wording: "checks for interruption at checkpoints (dcfs/checkpoint.h),
each just before a backing syscall and never between a mutation's
syscall and its phase 3, and has a cancellation test".

Merged 2026-10-08 (87b35a0, 7 commits, +2070/-32) after the review fixes:
a refused INIT exits -EPROTO; re-resolution stops at the first EINTR;
Drain reads one message ahead at most, FUSE_CAP_SPLICE_READ unset; a
parameterized cancellation test over all twelve checkpoints under the
invariant checker; cancel_test bounds 2 s KVM / 8 s TCG (0.8 s measured;
12.1 s with checkpoints off); large_test 7,238,097 states and nolock_test
6,036,816 unchanged (1508 s / 1272 s alone under load 20). AGENTS.md's
rule now reads "at checkpoints (dcfs/checkpoint.h), each just before a
backing syscall and never between a mutation's syscall and its phase 3".

## 22.5 Timely interrupts, in the model (russ, 2026-10-17)

russ: "Let's use the formal model to additionally provide some guarantees
around 'interrupt in a timely manner'. Not necessarily wall clock, but
maybe 'interrupt between every step' or something like that."

Today `Interrupt(p)` is an action that may happen at an interruption
point pc, so the model proves an interrupt is safe but not that it is
acted on: a dcfs that ignored interrupts forever passes. This step makes
acting on one an obligation and bounds how much work follows it.

- **Arrival, separate from acting on it.** `InterruptArrives(p)` marks a
  request (in flight, or queued before dispatch) interrupted at any step.
  At an interruption point pc an interrupted request's only enabled step is
  `Interrupt(p)`: the interruption point checks, as `CheckInterrupted` in
  `dcfs/interruption_point.h` does. Between a syscall and its phase 3 it
  proceeds, as now.
- **The bound, a safety property counted in work, not time.** After the
  interrupt arrives, a request takes at most K more backing syscalls before
  it replies, where K is the longest run of backing syscalls with no
  interruption point between them (the syscall already past its interruption
  point, plus the post-syscall reads before phase 3, e.g. a create's probe).
  Find K from the model, name every pc in that run, and state the property
  with K written out. If the honest answer is "unbounded somewhere" (a loop
  of backing calls with no interruption point), that is a finding: report it
  rather than bending the property.
- **Liveness.** An interrupted request eventually replies (`~>`), under
  the weak fairness of the request's own steps that the code provides.
- **What the bound does not count.** Work with no backing syscall: cache
  queries and commits, the kernel-blocked syscall itself (README future
  work, coroutines over io_uring). Say so in the model's header and in
  `docs/design.md` "Cancellation".
- **Known bugs.** A `known_bugs/` variant with one interruption point
  removed (say, in readdir's probe loop, or a rename's second name) must
  violate the bound, and one where the interruption point ignores the
  arrival must violate liveness.
- **The code keeps it.** Trace validation (Phase 12) must refuse a trace
  in which the code takes a backing syscall at an interruption point after
  the interrupt was visible there. Propose, and if cheap build, a mechanical
  check that every backing call reachable from a FUSE handler is preceded
  by an interruption point or sits in the named post-syscall run (the
  model's K), in the style of 25.9's backing-door matcher.
- Keep the state space affordable: only the `MC_interrupt*` configs turn
  arrival on; `MC_large` and `MC_nolock` state counts stay unchanged.

Owner: dcfs-protocol. Docs: `formal/README.md` (actions and properties
tables), `docs/design.md` "Cancellation", README's EINTR paragraph if
the guarantee is user-visible.

### As built (step-22.5; rounds 1-3, 2026-10-17/18, pending review)

Round 1 (the names below follow 22.6 and round 2):

- `InterruptArrives(p)` sets the slot's `intr` at any step of a request in
  flight; at an interruption point pc (`InterruptPcs`) `At` requires
  `Proceeds(p)`, so `Interrupt` is the only step there. "Queued before
  dispatch" is the same as arriving right after the first step (no D
  request's first step makes a backing syscall but a sync point's syncfs,
  which the model takes in its first step and the code has an
  interruption point before).
- Liveness under `FairSpec` (`RequestFairness == \A p :
  WF_vars(RequestStep(p))`): `InterruptedReplies` (`intr ~> idle`) and
  `InterruptActedOn` (at an interruption point: `~>` idle with an `EINTR`
  reply, or the daemon down). Known bugs `interrupt_ignored` (`Sees <-
  SeesNothing`: `InterruptActedOn`) and `interrupt_not_replied`
  (`InterruptedReplies`). Deviation: the brief's "ignores the arrival must
  violate liveness" holds only for `InterruptActedOn`; a request that
  ignores its interrupt still replies, hence the second variant.
- Trace validation: the recorder writes `interrupt_arrives` where a
  mutation's interruption point sees the interrupt (its `interrupt` line
  waits for `End`); `T_InterruptArrives`.
  `trace_fault_interruption_point_ignored_test` (a `--wrap` of
  `CheckInterrupted` that ignores its EINTR) is rejected at the unlink's
  syscall line; before the recorder change it was valid. Three
  `trace_tests/interrupt_*.log`.
- The mechanical check is proposed (formal/README.md, "Timely
  interrupts"), not built; `InterruptSweepTest` (round 2) is that check
  for the scenarios it lists.

Round 2, from the review (B1, S1-S4, three findings, nits) and russ's
rulings:

- B1: the bound is per request kind, at no state cost: `InterruptBound ==
  \A p : ps[p].nsys <= Run(ps[p].kind)`, `Run` 2 for a create (`C_probe`,
  `C_stat`), 1 for every other mutation past its syscall (`C_stat`,
  `U_stat`, `R_stat`, `A_stat`, F's `FC_probe`, `FS_stat`) and F's release
  (`FR_stat`), 0 for the rest (lookup, readdir, readdirplus, getattr,
  write, sync point, F's open and lookup). Round 1's single K = 2 saw only
  the create's interruption point removed: the new
  `known_bugs/interruption_point_removed_unlink` (an unlink then takes 2)
  found nothing under it. `interrupt_checkpoint_removed` is now
  `interruption_point_removed_create`. `PD_commit`'s interruption point is
  invisible to any count of the model's syscalls (said at the property;
  in the code it bounds the probes, which the sweep counts).
- S3: F's interruption points are the model's too (`FC_sys`, `FS_sys`, a
  write's `W_sys`; with round 2's code, `FG_stat`, `FG_rstat`), checked by
  two new medium configs, `MC_interrupt_files.cfg` and
  `MC_interrupt_borndirty.cfg`. They found two gaps in the model, fixed:
  an interrupted setattr of F must End F's mutation (`FEndMutation`;
  `GuardsBalanced` otherwise), and an interrupted file request's EINTR was
  no reply `ReplyObservable` allowed.
- S4, adopted: the safety configs take the arrival only where it can
  matter (`CanArrive <- CanArriveWhereItMatters`: a pc whose step reads
  `intr`, the slot free to step, the daemon up; the excluded `QuietPcs` is
  wrong only in the safe direction); the liveness configs keep arrival
  anywhere. Exact by the argument in `dcfs.tla`, and every safety known
  bug of the interrupts still finds its counterexample under it. Distinct
  states, anywhere -> where it matters (before round 2's new
  interruption points): `MC_interrupt` 1,134,929 -> 677,637, `_muts2`
  3,908,815 -> 2,252,176, `_nolock` 4,207,066 -> 2,774,256, `_files`
  78,402 -> 59,680, `_borndirty` 13,642 -> 10,266. After round 2's
  interruption points: `MC_interrupt` 533,393, `_muts2` 1,718,997 (8.7 min
  at load 40), `_nolock` 2,498,246 (13.2 min at load 40), `_liveness`
  64,417, `_liveness_slots` 107,473, `_files` 55,625, `_borndirty` 9,678.
  `_muts2` and `_nolock` keep "eternal" (`_nolock` is close to "long" at
  load 40).
- The three code findings, fixed (tests first, failing output in the
  commits): an interruption point before each getdents64 in
  `ReadDirNames`; one at the start of `PopulateDirectory` (a readdir's
  population and an empty directory's too; `LookupOrPopulate` keeps its
  own only before a probe); one in `ParentOf` before its backing reads and
  one in `FreshAttr`, by name, before a refresh that is the request's own
  work (`GETATTR`, `OPENDIR`, a `LOOKUP` of "." or "..", each
  `READDIRPLUS` entry), not after a mutation's syscall nor for a
  `LOOKUP`'s reply entry, which the model's lookup has replied before.
  The model's `GA_stat` and `RDP_stat` (F's `FG_stat`, `FG_rstat`) are
  interruption points. An interrupted `READDIR`/`READDIRPLUS` that built
  entries replies them, a short listing (russ, style 1.12); with none,
  `EINTR`. The recorder writes no interrupt for a request whose model
  request has replied (`Req::terminal`, set but never read before, now set
  for a lookup's answer only when the request is a lookup).
- The per-child probe loop (russ's question via the orchestrator,
  2026-10-18): it has an interruption point before each batch of 16
  probes (since 22.3), so at most 15 probes, about 4-6 syscalls each,
  follow an interrupt; the sweep counts them. Not made per child: each
  interruption point is a `poll` of `/dev/fuse` and may read one request
  ahead, so per child would add one syscall per probe; russ's call.
- `InterruptSweepTest` (its own target, small): the review's harness test,
  each scenario's request interrupted from each of its backing calls on
  (an empty directory and one of more than 64 KiB of names among them),
  counted against the case's run in the code's calls.
- S1 (docs): the bound is stated as model steps on D, with what follows
  the model's request listed (README, formal/README.md, docs/design.md).
- Nits: a population scenario for the ignored-interruption-point fault
  (`trace_fault_interruption_point_ignored_population_test`, rejected at
  `populate_read`); `interrupt_reach_<pc>_test` (`InterruptReach.tla`):
  every interruption point is reached by an interrupted request; the
  known-bug-only overrides live in their known_bugs modules; a probe is
  not a fixed number of syscalls; the `interrupt_after_syscall` trace
  test's comment.
- Not done: the test-name leftovers of 22.6 (`FakeInterrupts::
  checkpoints`, `CheckpointTest`, the cancel.sh check name, the
  `SessionLoop` log prefix), left for the follow-up the log names.

Round 3, from the re-review and the style review (as the orchestrator
decided under russ's rulings):

- `InterruptionPoint` (dcfs/interruption_point.h, russ's correction: no
  general strong-type helper yet): `CheckInterrupted` takes one, built
  only by a consteval constructor from a non-empty string literal;
  "maybe an interruption point" is `std::optional<InterruptionPoint>`.
  Its compile-time refusals (an empty literal, a non-constant array) are
  checked by hand and noted in its test: there is no compile-fail
  harness.
- Behave like the backing (S2): a listing builds its first entry whatever
  comes and honours an interruption point only once it has one, as the
  kernel's getdents (fs/readdir.c:366, filldir64); so it never replies
  EINTR from an interruption point, only a short listing. `ParentOf` takes
  its interruption point from the caller. `RDP_stat` is not an
  interruption point; a readdirplus's Run is 1. Populations keep theirs.
- One rule for reads after a model request has replied (S3): an
  interruption point only where a short reply is possible; a LOOKUP's
  reply entry has none.
- B1, S4: a held getattr's interruption point has its test; the sweep
  covers F's create, a held getattr, a write, a release and OPENDIR.
- S5: `InterruptPrompt`, `QuietStepsUncounted` (in the liveness config),
  `ASSUME QuietPcs \cap InterruptPcs = {}`, their premises, and the
  reduced known-bug copies as tests.
- S1: `PD_commit` stands for the between-batch checks only.
- Style: `AppendDirEntries`/`AppendDirEntriesPlus` in place of the
  immediately invoked lambdas; dcfs.tla's FUSE_INTERRUPT block cut to a
  page; the test and comment findings.
- State counts after round 3: `MC_interrupt` 547,645, `_muts2` 1,755,235
  (18.9 min at load 65), `_nolock` 2,622,519 (33.7 min at load 65),
  `_liveness` 65,261, `_liveness_slots` 110,435 (14.5 min at load 55; its
  timeout is now "eternal"), `_files` 55,625, `_borndirty` 9,678.

## 22.6 Interruption points: the cancellation checkpoint renamed (russ, 2026-10-17; merged 2026-10-17, b838960, Gemini Flash)

"Checkpoint" meant both a SQLite WAL checkpoint and Phase 22's place where
a request checks for FUSE_INTERRUPT. russ: "Agreed. Interruption points
also is more semantically meaningful." `dcfs::Checkpoint(ctx, where)`
becomes `dcfs::CheckInterrupted(ctx, where)`; `dcfs/checkpoint.{h,cc}`
become `dcfs/interruption_point.{h,cc}` (target renamed, users updated);
the prose term is "interruption point" everywhere (AGENTS.md's rule,
docs/design.md "Cancellation", formal/ comments and README, test names,
the plan's current text; history in log.md stays as written). "Checkpoint"
then means only the SQLite WAL checkpoint. Mechanical, after 22.5 round 2
and 25.28b merge (both edit those call sites). Owner: Gemini (agy), a
style review after.

**Do not confuse the two** (russ, 2026-10-17: "be careful the agent doing
the refactor doesn't get the two kinds of checkpoint confused"): rename
only `dcfs::Checkpoint` from `dcfs/checkpoint.h`, its call sites, and
prose about checking for FUSE_INTERRUPT. Never touch SQLite's:
`Connection::Checkpoint`, `CheckpointMode`, `sqlite3_wal_checkpoint*`,
`CheckpointCache`, `wal_checkpoint`/`WalCheckpoint`/`WalCheckpointed`
(model, trace, events), `wal_autocheckpoint`, "WAL checkpoint" prose. The
prompt lists every file:line to change, from a review of `git grep -n
-i checkpoint` sorted into the two senses by the orchestrator before the
job; the job changes only those lines, and the review checks that no
SQLite sense changed (a grep of the diff for those names must be empty
of edits).

## 22.7 Streamed directory population (russ, 2026-10-17; queued)

**Milestone M1, a provisional placement by the orchestrator, beside 25.10
(both change what a readdir may promise); russ to confirm.** Owner:
dcfs-protocol (Opus, high), reviewed by dcfs-reviewer. After 22.5 (which
lands first, with a per-page interruption point in `PopulateDirectory` as
the interim fix) and after 25.28b and 12.20 (all three edit `dcfs.tla`).
russ (2026-10-17) agreed: dcfs should stream directory population instead
of reading a whole backing directory before answering.

**Why.** `PopulateDirectory` reads every `getdents64` page and then probes
every child (`openat`, `statx`, the file handle, the generation, the
symlink target, every xattr) before the first reply, so the first `ls` of
a 20,000-entry directory waits for 20,000 probes, `ls | head` pays for all
of it, and one FUSE request does unbounded backing work. POSIX leaves an
entry added or removed during a `readdir` free to appear or not, and the
backing gives callers only that, so a whole-directory snapshot is stronger
than owed (12.19: no stronger than the backing). Streaming gives a fast
first page, a one-page `ls | head`, and bounded backing work per FUSE
readdir, so interrupt timeliness holds by construction rather than by an
interruption point in a loop. It supersedes 22.5's per-page interruption
point and the `kProbesPerCheckpoint` batching inside `PopulateDirectory`.
Today's `docs/style.md` 1.12 text ("the listing across calls stays one
snapshot", and `readdir` among the calls that are atomic) is stronger than
POSIX's `readdir` and than this design; the step amends it in the same
change (with russ's wording), and `docs/design.md` "Population policy" and
"Cancellation", `formal/README.md` and the README's limitations follow.

**Design to settle** (each is a question with the orchestrator's proposed
answer; dcfs-protocol decides, with the model, and records the answers in
`docs/design.md`):

1. *Where does a handle's listing come from?* Proposed: a directory handle
   whose first READDIR(PLUS) at offset 0 finds the listing incomplete
   (a miss) is a **stream handle**: it serves from its backing directory
   descriptor and the backing's own `d_off` values as the reply offsets,
   until EOF. A handle that finds the listing complete is a **cache
   handle** and serves from the cache with dcfs's offsets (rowid + 2), as
   today. `Opendir` still makes no backing call; it now returns a
   handle (`fi.fh`) whose state `Releasedir` frees. A handle's mode is
   fixed at its first reply (see 6 for why).
2. *What does a plain READDIR page cost?* Proposed: it replies from
   `getdents64`'s name, inode number and type, with no per-child syscall
   (the nodeid is the backing inode number, phase 14, so `d_ino` is what
   every other reply would show). `.` and `..` keep the backing's
   cookies and use `d_ino` (the mount's root is its own parent, as
   `ParentOf` has it). A mount point's `d_ino` is the covered directory's,
   not the stub's, so a plain page shows a boundary entry's `d_ino` that its
   later `stat` does not: this is native behaviour (readdir reports the
   covered directory's inode, stat the mounted root's), not a dcfs
   limitation (Q-2026-10-17-23).
3. *What does a READDIRPLUS page cost?* Proposed: it probes the entries
   that go in the reply (`ProbeChild`, as today), replies, and records
   them (`RecordChild`, one transaction per page). A request replies at
   most N entries (a constant, today's 16), a short listing being valid
   (the next call continues; style 1.12, most useful result), so a
   request's backing work is `getdents64` plus at most N probes whatever
   the reply buffer's size. How the entries past N are handled (an
   `lseek` back to the N-th entry's `d_off`, or a buffer held in the
   handle) is measured against the syscall budgets.
4. *When is the listing recorded, and when is it complete?* Proposed:
   page by page, under one fill snapshot and epoch taken when the stream
   starts (`BeginFill`, `DirEpoch`, as `PopulateStarted` takes them). A
   READDIRPLUS page records its children (`RecordChild`) as it probes
   them. A plain page records its names as `listed` (item 4a), with no
   per-child syscall. The listing stays incomplete (tri-state) during the
   stream. At EOF the stream marks it complete and prunes the names it did
   not see only if every page since offset 0 was read and recorded in
   order (the handle tracks "this request started where the last reply
   ended"), and `CanFill` still holds for the directory and the epoch has
   not moved: no mutation of the directory began, ended or ran since the
   snapshot. Otherwise the listing stays incomplete and is never wrong. The
   names seen are held in the handle for the prune, as today's population
   holds them (a name stamp on the dentries is the alternative if memory
   budgets object). Consequence: a plain `ls` to EOF completes the listing
   too, so negative lookups in it are answered from the cache (item 4a).
4a. *A `listed` dentry state* (orchestrator, 2026-10-17, from russ's "only
   cache data that the client asked for"). Today `dentries.state` is
   present, absent, unknown or refused, and `CHECK ((state = 'present') =
   (inode IS NOT NULL))` (dcfs/migrate.cc:148-156) welds "the name exists"
   to "its inode is fully identified" ((device_id, backing_ino,
   backing_gen) NOT NULL in dcfs/schema.sql:77-106), so recording a plain
   readdir page forces a per-child probe. Add state `listed`: the name
   exists, identity unknown. Its `getdents64` `d_ino` and `d_type` are kept
   as hints (nullable columns, set only for `listed`). Rules:
   - A plain READDIR records its pages as `listed`, with no per-child
     syscalls.
   - A lookup of a `listed` name probes that one child (identity,
     generation, mount boundary) and turns it into present (or refused).
   - READDIRPLUS probes the page's entries as it replies (item 4).
   - Serving a plain listing from the cache reports the hint `d_ino`, which
     for a mount point is native behaviour (item 2, Q-2026-10-17-23).
   - A mutation sets a `listed` name unknown before its syscall and present
     or absent after, as for a present name (the tri-state rule).
   - Model first: `listed` is a dentry state in `formal/dcfs.tla`, with its
     transitions under concurrent mutation, before any code.
   - What a `listed` entry answers without a probe (russ, 2026-10-17):
     anything needing only existence, d_ino and d_type. Plain READDIR from a
     complete listing (page 0 again included). Decided (russ,
     2026-10-17: "It adheres to 'cache only what a client requested'"):
     a partial listing serves its recorded prefix from the cache
     and resumes the stream on the backing from the last recorded d_off
     (the backing's d_off is stored per entry and used as the reply
     cookie for cached entries too, so a handle's cookies are the backing's
     throughout and seekdir/telldir work across the switch; if the backing
     refuses a stored cookie, the stream restarts at 0, which POSIX allows,
     as it lets entries changed meanwhile appear or not). Name-existence answers: EEXIST for
     mkdir/mknod/create/symlink/link onto a listed or present name,
     RENAME_NOREPLACE, and ENOENT for unlink/rmdir of a missing name in a
     complete listing; state whether these are answered from the cache or
     always sent to the backing (write-through makes the cache answer an
     optimisation; check what the code does today). READDIRPLUS probes the
     page's listed entries as it replies. Everything else needs a node ID,
     so a LOOKUP, so a probe.
   This supersedes Q-2026-10-17-22 (plain pages record nothing); a plain
   page is no longer a non-recording page.
5. *An abandoned or interrupted stream?* Proposed: the pages recorded stay
   (they are fills like any lookup's); the listing stays incomplete;
   nothing is undone. A handle released (or a request interrupted: it
   replies the short listing built so far, or `EINTR` if it has none)
   before EOF is that case. The backing descriptor is closed at EOF and at
   `Releasedir`.
6. *seekdir, telldir, rewinddir.* Proposed: a request at an offset other
   than the last reply's end (a `seekdir`) does `lseek` on the backing
   descriptor to that cookie (reopening it if EOF closed it), records its
   names as `listed` like any page, but the stream can no longer complete
   the listing (only a contiguous read from offset 0 to EOF does; a later
   `rewinddir` restores that), and passes the backing's answer
   through for a cookie it does not like (style 1.12: the backing's
   contract). `rewinddir` on a stream handle restarts the stream at 0 with
   a new snapshot (it may complete the listing); the handle does not turn
   into a cache handle, because a cookie from before the `rewinddir` would
   be read as a rowid. Cookies are the backing's: stable for the handle's
   life, as the backing keeps them.
7. *A second `opendir` during a stream, and handles after completion?*
   Proposed: independent. Each stream has its own descriptor, snapshot and
   `seen` set, records idempotently (upserts guarded by `CanFill`), and
   each completes or not on its own guard. A handle opened after a
   completion is a cache handle.
8. *Lookups resolve one name* (russ, 2026-10-17: point operations stop
   populating the parent directory). `LookupOrPopulate` on an unknown name
   of an incomplete directory resolves only that name on the backing
   (`openat` O_PATH, the reads of one child) and caches the answer:
   positive, or a negative entry (absent). A directory becomes complete
   only when a client reads its listing to the last `getdents64` page
   (item 4's guard); no lookup drains a directory, and `PopulateDirectory`
   as a run-to-EOF drain is removed (the page engine, driven only by
   readdir, is the one mechanism). Reason: the fetch-minimum rule
   (docs/design.md, "Fetch the minimum needed to serve the request"),
   "behave like the backing", and
   bounded memory (one `stat` no longer reads a million-entry directory).
   Repeated lookups of a name are cached either way, and the kernel's
   negative dentries absorb most repeats. The cost: the first lookup of
   each distinct name in a never-listed directory is one backing call, so
   probe-heavy workloads (PATH search, include paths, import paths) pay it
   once per name, and a negative lookup in an incomplete directory is not
   answered without the backing. M3 (README milestones) records spin-ups
   and backing lookups per workload; revisit with that data if probe-heavy
   workloads cost real spin-ups. The model: population is driven only by
   readdir; a lookup in an incomplete directory is a single-name resolve;
   12.24's no-over-fetch property is stated for lookups too.
   `ListCached`'s attempt loop and its `EAGAIN` ("kept
   changing") go in the same change (25.10 removes the retry there too;
   whichever lands second drops what is left): a readdir no longer needs
   the listing recorded to answer.
9. *Out-of-band changes.* The completion guard covers dcfs's own
   mutations. A backing directory changed or removed behind dcfs's back
   during a stream gets what the backing gives (`getdents64`'s answer, an
   errno passed through); the existing out-of-band policy
   (`docs/design.md`) is unchanged.

**The model first.** `formal/dcfs.tla`'s `PopulateRead` (`PDRead`) reads
the whole listing in one step. It becomes many steps with a partial
listing in the request's state: a cursor in the backing's name order
(`d_off`'s abstraction), the names seen so far, the stream's snapshot and
epoch (taken at its first page), and a flag "every page so far was
probed". A page step reads the names after the cursor from the *current*
backing (so a name created behind the cursor is missed and one ahead of it
appears, the POSIX freedom), replies them, and records them if `CanFill`;
`Abandon` ends a stream at any page boundary; the completion step at EOF
links the seen names, prunes the rest and marks complete only if the flag
holds, `CanFill` and the epoch is unchanged (`PDCommit`'s guard, at the
end of the stream instead of at the end of one read).

- `known_bugs/complete_despite_mutation`: the completion ignores the
  guard, so a create behind the cursor during the stream is marked
  complete as absent. `CacheNeverWrong` fails; the test expects the
  counterexample.
- A pinned non-guarantee (12.19's pattern): a configuration asserting
  that a streamed listing is a snapshot of one instant expects TLC's
  counterexample, so a dcfs that grew stronger than the backing (a
  whole-directory read back at the start) fails the test.
- Properties of the replies (new, `ListingContract`): every name replied
  existed in the backing at some instant between the first page and the
  reply; a name present throughout the stream is replied exactly once.
- `InterruptBound` (22.5) holds with no interruption point inside a page's
  probes: K is `getdents64` plus N probes' syscalls, written out, and the
  stream's loop has one interruption point at each page's start.
- Model state counts: the new steps only multiply the listing
  configurations; `MC_large` and `MC_nolock` are re-measured and the
  growth reported (the stream's page size in the model is one name).
- Trace validation: the recorder's `populate_read` line is replaced by
  one line per page (and the held-lines reordering note in
  `formal/README.md` shrinks with it: a page is a short step).

**Tests first** (each fails on today's code and is shown failing):

- *Cancellation:* a request interrupted during a stream replies within
  one page's work (`cancel_test`, counting backing calls after the
  interrupt, not time: at most K); an interrupted stream keeps its pages.
- *`ls | head` reads one backing page:* `getdents64` calls counted
  (load-agnostic, as 26.36's rule says) are one for the first page of a
  large directory, against all of its pages today; `probe` calls are at
  most N for READDIRPLUS and zero for plain READDIR.
- *A mutation during a stream leaves the listing incomplete:* a create
  and an unlink of a name between two pages; after EOF the listing is not
  complete, a lookup of the new name finds it, and a later full stream
  completes. Under the model's known bug this is the failing case.
- *Offsets resume:* two clients' interleaved streams; `seekdir` back to an
  earlier cookie; `rewinddir`; a directory larger than one reply listed
  across calls lists every untouched entry once (`readdir_boundary_test`
  stays, and its CPU-tick ratio still holds).
- *Cache handle:* a handle opened after completion serves from the cache
  with no backing syscall; a stream and a cache handle open at once.
- *Budgets (26.x):* `test/qemu/guest/syscall_budgets.txt` gains the first
  page's syscalls (plain and plus, exact), and the old whole-directory
  population's entry for a readdir goes.

Docs: `docs/design.md` "Population policy" and "Cancellation",
`formal/README.md` (actions and properties tables, the trace-validation
notes), `docs/style.md` 1.12 (the readdir sentence), README's limitations
(boundary `d_ino` is native behaviour, Q-2026-10-17-23; a plain `ls`
completes the listing, item 4a). One way to do each thing: the
whole-directory read for a readdir, `ListCached`'s retry loop, and
`kProbesPerCheckpoint` all go; report any that stays and why.
