# The dcfs protocol model (TLA+)

`dcfs.tla` is a model of dcfs's write-through protocol: three-phase
mutations, fills and their guards, directory completeness, the durable dirty
set and sync points, crashes, and startup recovery. The TLC model checker
explores every behavior the model allows within small bounds and checks
that none breaks the cache's promises (the properties below). Each test in
`BUILD.bazel` is one TLC run.

The model describes what the code does today (`dcfs/backing.cc`,
`dcfs/metadata_cache.cc`, `dcfs/dir_cache_fs.cc`), not what `docs/design.md`
intends. Where the two differ, see [Findings](#findings).

This file assumes no TLA+ background.

Contents:

1. [Running it](#running-it)
2. [TLA+ in five minutes](#tla-in-five-minutes)
3. [What is modelled, and what is not](#what-is-modelled-and-what-is-not)
4. [Variables](#variables)
5. [Actions](#actions)
6. [Properties](#properties)
7. [Configurations](#configurations)
8. [Known bugs: the model's own tests](#known-bugs-the-models-own-tests)
9. [Findings](#findings)
10. [Trace validation](#trace-validation)
11. [Reading a counterexample](#reading-a-counterexample)
12. [Changing the model](#changing-the-model)

## Running it

```sh
bazel test //formal/...                    # everything (about 20 minutes)
bazel test //formal:small_test             # the real model, small bounds (~1 min)
bazel test //formal:known_bug_crash_f1_phase1_not_durable_test
```

(On russ's machine, wrap Bazel in `sg kvm -c '...'` as for every Bazel
command; these tests don't need KVM, but the Bazel server does.)

Trace validation, which checks recorded runs of the code against the model,
has its own targets next to the guests that record the traces (they do
need KVM): see [Trace validation](#trace-validation).

TLC runs on the host on a pinned JDK (`third_party/tlaplus/`). The test log
(`bazel-testlogs/formal/<test>/test.log`) holds TLC's full output, including
any counterexample.

To run TLC by hand, for example with different bounds:

```sh
bazel build //formal:small_test    # fetches the jar and the JDK
OB="$(bazel info output_base)/external"
JAVA="$OB/rules_java++toolchains+remotejdk21_linux/bin/java"
JAR="$OB/+http_file+tla2tools/file/tla2tools.jar"
mkdir -p /tmp/tlc && cp formal/*.tla formal/*.cfg /tmp/tlc && cd /tmp/tlc
"$JAVA" -XX:+UseParallelGC -jar "$JAR" -workers auto -config MC_small.cfg MC
```

`-coverage 1` adds how often each action fired (an action that never fires
is a sign that part of the model is dead). For a known bug, copy
`known_bugs/*` into the same directory and name its module instead of `MC`.

## TLA+ in five minutes

A TLA+ specification describes a system as a **state machine**:

- **Variables** (`VARIABLES`). A **state** is one value for each variable.
- **`Init`**: a formula saying which states the system may start in.
- **Actions**: formulas relating a state to a next state. Unprimed names
  (`dbCur`) are the current state, primed names (`dbCur'`) the next one.
  `dbCur' = new` means "in the next state, `dbCur` is `new`", and
  `UNCHANGED x` means `x' = x`. An action is **enabled** when some next
  state satisfies it; the guard conjuncts (`ps[p].pc = "C_sys"`) say when.
- **`Next`**: the disjunction (`\/`, "or") of all actions. Each step of a
  **behavior** (a sequence of states) is one action.
- **`Spec == Init /\ [][Next]_vars /\ Fairness`**: behaviors that start in
  `Init` and take `Next` steps. `Fairness` (`WF_vars(A)`, weak fairness)
  says an action that stays enabled is eventually taken; it matters only
  for liveness properties.

Notation used here: `/\` is "and", `\/` is "or", `=>` "implies", `~`
"not", `\A x \in S : P` "for all", `\E x \in S : P` "there exists",
`[r EXCEPT !.f = v]` "record `r` with field `f` set to `v`",
`[x \in S |-> e]` "the function mapping each `x` in `S` to `e`",
`IF ... THEN ... ELSE`, `LET ... IN`, and `\* comment` / `(* comment *)`.

Properties come in two kinds:

- An **invariant** is a formula that must hold in every reachable state.
- A **temporal property** talks about whole behaviors. The only one here,
  `P ~> Q` ("P leads to Q"), says that whenever `P` holds, `Q` holds then
  or later.

**TLC** explores the state graph breadth-first from the initial states,
checking every invariant in every state it reaches. If one fails, it prints
a **counterexample**: the shortest behavior from an initial state to the
bad one, state by state. A `.cfg` file tells TLC which specification to
check, the values of the model's `CONSTANTS` (the bounds), and which
invariants and properties to check. A `VIEW` tells TLC to treat states with
the same view value as one state (see `MC.tla`).

TLC only explores the bounds it is given (here: two names, two concurrent
requests, a few mutations and crashes). It is exhaustive within those bounds,
which is where protocol bugs (a missed interleaving, a crash at an unlucky
moment) usually show up first.

## What is modelled, and what is not

**Modelled**: one directory D (the root, so `ParentOf` needs no syscall)
whose names are the constant `Names`, and:

- the backing filesystem's D: which object each name points at (objects are
  identities, never reused, like an inode number plus generation), and D's
  attributes, abstracted to a stamp that changes with every change to D;
- the cache database's view of D: a dentry row per name (`present` with an
  object, `absent`, `unknown`, or no row), `children_complete` and its
  `epoch`, D's attributes and `attrs_valid`, D's row in the `dirty` table,
  and `clean_shutdown`;
- for both disks, every state a crash may leave (see `bOpts`, `dbOpts`);
- the daemon's memory: the fill guards, `Context::dirty.durable`, and the
  requests in flight, each a small program counter with its local state;
- requests: lookup, readdir, readdirplus, getattr of D, create, unlink,
  rename (within D), and sync points; crashes at any moment; startup
  (`StartRun`, `RecoverDirty`) and clean shutdown (`FinishRun`).

**Concurrency**: requests interleave at every backing syscall, as they will
under the planned coroutines (`docs/design.md`, "Concurrency, today and
with coroutines"), and so does a sync point at its `syncfs`. The code
between two syscalls runs without interruption (one thread, synchronous
SQLite), but a crash can still happen between any two of its transactions.
One switch says how much the environment serializes:

- `KernelDirLock`: the kernel holds D's lock across every lookup, readdir
  and namespace mutation of D (the parent's `i_rwsem`, plus FUSE's
  per-directory lock because dcfs does not request
  `FUSE_CAP_PARALLEL_DIROPS`), but not across a getattr or an fsync. TRUE in
  the main configurations, because that is true today and stays true under
  coroutines; `MC_nolock.cfg` checks the model without it.

**Abstractions**, each chosen because the protocol property does not depend
on the detail:

- Only D's records are tracked. Child objects' own attributes, xattrs and
  symlink targets, and their rows in the dirty set, are left out: in one
  directory they protect nothing D's records don't. Because every modelled
  mutation names D, `FillGuards::touched[D]` always equals `seq`, so the
  model keeps just `seq` and `inflight[D]`. The `touched` map's pruning
  (`floor`) is not modelled.
- Leaving the children out of the dirty set means the fast path in
  `BeginMutation` (commit at normal durability when every named inode is
  durably dirty) is taken whenever D alone is, more often than in the code.
  That is the less durable choice, so it only allows more crash outcomes.
- An unknown attribute's stale value (kept in the row as a hint, never
  served) is not modelled: `attr` is 0 while `attrValid` is FALSE.
- A fill's snapshot (`BeginFill`) is taken in the same step as its first
  backing read. In the code it comes just before, after `OpenNode`.
  Merging them only drops behaviors in which the guard rejects a fill
  whose read was already fresh.
- `PopulateDirectory` reads the whole listing in one step (getdents64 and
  every probe).
- A crash leaves each disk in any state since its last sync: SQLite's WAL
  recovers a prefix of the commits; the backing filesystem's journal keeps
  a prefix of its transactions (modelling every prefix of its operations
  allows more than real journals do).
- `clean_shutdown` is modelled but changes nothing: `StartRun` runs
  `RecoverDirty` whatever it says, and `RecoverDirty` with an empty dirty
  set does nothing.
- Writable opens are not modelled (`open_for_write`, which keeps a file's
  attributes unknown while the kernel writes to it through passthrough,
  the last release that records them, and its guard event
  `cache::EndWrites`; the sync point keeps the dirty row of an inode open
  for writing at any moment between its snapshot and its clear). D is a
  directory and cannot be opened for writing, so modelling them needs a
  file object with its own attributes, `attrValid`, dirty row and durable
  flag, an open/released state, and writes that change its backing
  attributes; and since `bOpts` and `dbOpts` are sets of whole disk
  states, every one of those multiplies the sets a crash may leave. That is
  a second inode's worth of state in every configuration, for a rule that
  is the same as for D's mutations (a phase 1 before, a guard event at the
  end, a sync point keeps what was in flight). The review of R4 (finding 1)
  found the gap by reading; `dir_cache_fs_test` checks the release during
  a sync point's `syncfs`, the writable create's window and the fill that
  read before the release, and `metadata_cache_test` each half of the sync
  point's protection.
- Child objects' records and hard links are not modelled, so the model
  cannot find the stale-resolve gap that `RemoveChild` had (review of R4,
  finding 2: a rename onto the name between the unlink's resolve and its
  phase 1 made the unlink mark one object unknown and remove another,
  whose link count stayed cached as current when it had another link).
  The model's unlink records nothing about the object it resolved, so a
  stale resolve leaves every modelled record right. Finding it needs each
  object's attributes (at least `nlink`) and `attrValid`, more than one
  link per object, and the unlink's phase 1 and phase 3 on the resolved
  object, which multiplies the database states (and every crash-state set)
  by the objects' attribute states. The verification itself is modelled
  (`U1`, as `R1`), so that the model allows what the code does (retries,
  `EAGAIN`); `dir_cache_fs_test` checks the interleaving
  (`UnlinkMarksWhatItRemovesUnknown`).
- Not modelled: xattrs, hard links, links
  across directories, out-of-band changes and `ReconcileAttrs`,
  `InvalidateInode` after `ESTALE`, refused boundaries, `ParentOf` of a
  non-root directory, `RENAME_EXCHANGE`/`RENAME_NOREPLACE`, and the
  periodic sync's timing (any request may be a sync point at any time).

## Variables

| Variable | Meaning | In dcfs |
|---|---|---|
| `bCur` | The backing filesystem's D now: `names` (name -> object or `"-"`) and `ver` (D's attributes) | the real directory |
| `bOpts` | Backing states a crash may leave: the state at the last `syncfs` and every later one | the backing filesystem's page cache and journal |
| `dbCur` | The cache database now (for D): `dent`, `complete`, `epoch`, `attrValid`, `attr`, `dirty`, `clean` | `dentries`, `directories.children_complete`/`epoch`, `inodes.attrs_valid` and attributes, `dirty`, `cache_state.clean_shutdown` |
| `dbOpts` | Database states a crash may leave: the last fsynced commit and every later one | the WAL at `synchronous=NORMAL` |
| `mode` | `"up"` (serving), `"down"`, `"recover"`/`"start"` (startup), `"stop_*"` (shutdown) | `main.cc`'s lifecycle |
| `seq` | The fill guards' logical clock | `FillGuards::seq` |
| `inflight` | Mutations of D between phase 1 and their end | `FillGuards::inflight[D]` |
| `durableD` | D's dirty row is known durable since the last sync point | `Context::dirty.durable` |
| `running` | The request running code between two syscalls (others wait) | the thread itself |
| `ps[p]` | Request slot `p`: program counter `pc`, request `kind` and names `n`/`m`, whether it holds the kernel's lock, and locals (see `IdleProc` in `dcfs.tla`) | a FUSE request being served |
| `servedWrong` | History: an answer served from the cache was wrong when served | (checking only) |
| `stamp` | The next fresh object identity / attribute stamp | (checking only) |
| `muts`, `crashes` | How many mutations and crashes so far, for the bounds | (checking only) |

A dentry value is an object (`"o1"`, ...: present), `"absent"`,
`"unknown"`, or `"none"` (no row: absent if `complete`, else unknown;
`ReadState` in `dcfs.tla`).

## Actions

Each request is a chain of steps; the step names are what a counterexample
prints. A request's first step runs inside `Arrive`.

| Action | What it does | `docs/design.md` | dcfs |
|---|---|---|---|
| `Arrive` | A request arrives and runs its first step (taking D's lock if `KernelDirLock` and the request needs it) | Concurrency | `fuse_ops.cc` dispatch |
| `LookupStep` | Cache lookup of a name: serve a known answer, else resolve it alone (listing complete) or list the directory | Population policy | `backing::LookupOrPopulate`, `cache::Lookup` |
| `ResolveProbe` | Fill snapshot and probe of one name | Population policy, Concurrency (fill guards) | `backing::ResolveName`, `cache::BeginFill`, `ProbeChild` |
| `ResolveCommit` | Record the name if `CanFill(D)`; answer from the probe either way | same | `ResolveName`, `cache::CanFill`, `SetNegative`/`LinkDentry` |
| `PopulateRead` | Fill snapshot, D's epoch, getdents64 and every probe | Population policy | `backing::PopulateDirectory` phase A, `cache::DirEpoch` |
| `PopulateCommit` | If `CanFill(D)` and the epoch is unchanged: link the listed names, prune the rest, mark complete. Answer from the cache if recorded, else from the listing | Population policy, Concurrency (compare-and-set completeness) | `PopulateDirectory` phase B, `PruneDentriesNotIn`, `MarkDirComplete` |
| `ReaddirStep` | If `IsDirComplete`, serve the listing (the present rows) at once, before any syscall; else populate and check again (at most 3 attempts, then `EAGAIN`). Readdirplus then refreshes D's attributes if unknown | Population policy | `DirCacheFS::ListCached`, `Readdir`, `Readdirplus`, `cache::IsDirComplete`, `ListDir` |
| `ReaddirplusStat`, `ReaddirplusFill` | Readdirplus: statx of D, then fill D's attributes and reply | same | `DirCacheFS::Readdirplus`, `EntryFor` |
| `GetattrStat`, `GetattrFill` | Getattr of D with unknown attributes: statx, fill if `CanFill(D)`; reply what was read (a getattr with valid attributes is served in `Arrive`) | Concurrency (fill guards) | `DirCacheFS::Getattr`, `EntryFor`, `backing::RefreshAttrs`, `FillAttrs` |
| (create, first step) | Phase 1: the name and D's attributes unknown, D dirty, committed kSync unless D is durably dirty (fast path) | The write-through protocol: Phase 1 | `DirCacheFS::CreateChild`, `cache::BeginCreate`, `BeginMutation` |
| `CreateSyscall` | Phase 2: mkdirat/openat(O_CREAT)...; `EEXIST` if the name exists | Phase 2 | `backing::MkdirAt` etc. |
| `CreateProbe` | Probe the new name | Phase 3 | `backing::RecordNewChild` phase A |
| `CreatePhase3` | Record the dentry if `Owns(D)`; `Mutation::End`; fill snapshot for D's refresh | Phase 3, Concurrency (`Owns`) | `RecordNewChild` phase B, `Mutation::Owns`, `End` |
| `CreateStat`, `CreateFill` | Refresh D's attributes as a fill, reply | Phase 3 | `backing::RefreshAttrsFromFd` |
| `CreateFailed` | Failed phase 2: `End`, re-resolve the name, reply the error | Phase 2 | `ReresolveAfterFailure` |
| `UnlinkPhase1` | After resolving the name (from a fill snapshot, `rsnap`): `ENOENT`, or verify that no mutation of D began or ended since the snapshot and none is in flight; if so, phase 1 (name and D's attributes unknown, D dirty); if not, resolve again (at most 3 times, then `EAGAIN`) | Phase 1 | `DirCacheFS::RemoveChild`, `cache::BeginRemove` (`resolved`) |
| `UnlinkSyscall` | unlinkat; `ENOENT` if gone | Phase 2 | `backing::UnlinkAt` |
| `UnlinkPhase3` | Name absent if `Owns(D)`; `End` | Phase 3 | `cache::SetNegative`, `Mutation::Owns` |
| `UnlinkStat`, `UnlinkFill`, `UnlinkFailed` | As for create | | `backing::RefreshAttrs`, `ReresolveAfterFailure` |
| `RenameResolveDst` | After resolving the source (the resolve began with a fill snapshot, `rsnap`): `ENOENT`, or resolve the destination | | `DirCacheFS::Rename` |
| `RenamePhase1` | Verify that no mutation of D began or ended since the snapshot taken before the source was resolved, and none is in flight; if so, both names and D's attributes unknown, D dirty; if not, resolve both names again (at most 3 times, then `EAGAIN`) | Phase 1 | `cache::BeginRename` (`resolved`), `DirCacheFS::Rename` |
| `RenameSyscall` | renameat2: moves whatever the source name holds now | Phase 2 | `backing::RenameAt` |
| `RenamePhase3` | If `Owns(D)`: destination -> the resolved (and verified) source, source name absent; `End` | Phase 3 | `Rename`'s phase-3 transaction |
| `RenameStat`, `RenameFill`, `RenameFailed`, `RenameFailed2` | As for create (a failure re-resolves both names) | | `RefreshAfterRename`, `ReresolveAfterFailure` |
| (sync, first step) | Snapshot of the fill guards' clock, then syncfs: every backing write so far is durable | Sync points | `backing::SyncBacking`, `cache::BeginSync` |
| `SyncClearDirty` | Clear D's dirty row (normal durability) unless a mutation of D began or ended since the snapshot or is in flight; forget `dirty.durable` | Sync points | `cache::ClearDirty` |
| `Crash` | Daemon crash, kernel crash or power loss: each disk keeps any of its possible states, memory is lost | Crashes, power loss and recovery | |
| `Restart`, `Recover`, `StartRun` | Start again: `RecoverDirty` (one transaction; it may also make unknown any present dentry, standing for those that point at dirty children, which the model does not track: `RecoverForgetting`), then `clean_shutdown = 0` with kSync | Recovery; Startup | `backing::StartRun`, `cache::RecoverDirty` |
| `BeginShutdown`, `StopSync`, `StopClear`, `StopCkpt`, `StopFlag` | Unmount, sync point, TRUNCATE checkpoint, `clean_shutdown = 1` with kSync, exit | Shutdown; What the clean-shutdown flag adds | `backing::FinishRun` |

## Properties

| Property | Kind | Says |
|---|---|---|
| `CacheNeverWrong` | invariant | While serving, everything the cache could serve about D (each name, D's attributes) is unknown or agrees with the backing filesystem now. This is "no cache ahead" (and behind), including after any crash and recovery |
| `CompleteNeverHides` | invariant | A complete listing never makes a name absent that the backing filesystem has (part of the above, stated alone as in the plan) |
| `ServedFromCacheIsCurrent` | invariant (over the history variable `servedWrong`) | Every answer served from the cache (lookup, listing, attributes) matched the backing filesystem at the moment it was served |
| `TriState` | invariant | From a mutation's phase 1 until its phase 3 records the outcome (or it fails), its names read unknown and D's attributes are not valid |
| `CrashSafe` | invariant | In every state, every combination of states the two disks could be left in recovers to a correct cache. It is checked without taking the crash, so it finds crash bugs early |
| `DurableSetSound` | invariant | If `Context::dirty.durable` has D, every database state a crash may leave has D dirty (the fast path's premise) |
| `CleanMeansNoDirty` | invariant | `clean_shutdown = 1` is never durable together with a dirty row |
| `TypeOK` | invariant | Every variable has the expected shape |
| `RecoveryTerminates` | temporal | `(mode # "up") ~> (mode = "up")`: after any crash or shutdown, the daemon gets back to serving (recovery always terminates) |

## Configurations

`MC.tla` is the root module every configuration checks: it extends `dcfs`
and defines the request sets and the `View` the configurations use. Times
are from russ's machine (4 cores, loaded); the state counts are what TLC
reports as distinct states (since 12.2b's `RecoverForgetting`).

| Configuration | Test (tier) | Bounds | States | Time |
|---|---|---|---|---|
| `MC_small.cfg` | `small_test` (medium) | 2 names, 2 slots, 2 mutations, 1 crash, kernel lock, all request kinds, all invariants | 687,731 | ~2-3 min |
| `MC_liveness.cfg` | `liveness_test` (medium) | as small with 1 slot, no VIEW; plus `RecoveryTerminates` | 72,604 | ~30 s |
| `MC_large.cfg` | `large_test` (large) | 3 mutations, 2 crashes | 6,343,605 | ~13 min |
| `MC_nolock.cfg` | `nolock_test` (large) | as small without the kernel lock | 5,103,028 | ~7-8 min |

The `View` (in `MC.tla`) merges database states a crash may leave when
recovery would make the same cache of them: a dirty state's rows are
forgotten by `RecoverDirty` anyway. When it was introduced it cut the small
configuration from 409,410 states to 241,381. Liveness is checked without it, because
merging states can hide or invent cycles.

## Known bugs: the model's own tests

A model that cannot find the bugs the audits found by reading the code is
too coarse. Each constant `Bug*` in `dcfs.tla` puts one historical bug back;
`known_bugs/<bug>.tla` (which explains the bug) and `.cfg` turn it on, and
the test passes only if TLC reports the expected violation. All of them are
FALSE in the real configurations.

| Variant | Bug (audit) | Expected | The counterexample |
|---|---|---|---|
| `crash_f1_phase1_not_durable` | phase 1 not durable before the backing syscall (crash F1) | `CacheNeverWrong` | populate records `a` absent; create `a` (phase 1 not fsynced); the create syscall; a crash keeps the created file but rolls the database back past phase 1 (D not dirty); recovery has nothing to forget; `a` is served absent |
| `crash_f3_create_keeps_parent_attrs` | the parent not marked unknown on create (crash F3) | `TriState` | getattr records D's attributes; a create's phase 1 leaves them valid while it is in flight |
| `tristate_f1_unguarded_fills` | unguarded fills (tri-state F1) | `TriState` | a getattr reads D's attributes, a create's phase 1 runs, the getattr commits what it read anyway (with only `CacheNeverWrong` checked: one step later, once the create's syscall changed D, the stale attributes are served) |
| `tristate_f4_restore_complete` | the restore-completeness lost update (tri-state F4); needs two mutations of D in flight, so without the kernel lock | `TriState` | an unlink's phase 1 deletes `a`'s row and clears completeness; its syscall; a create of `a` begins (phase 1); the unlink's phase 3 restores completeness, so `a` reads absent while its create is in flight |
| `sync_during_mutation` | a sync point cleared the dirty rows of mutations in flight (a [finding](#findings) of this model, fixed in plan step R4) | `CrashSafe` | a create of `b`: phase 1 (D dirty, kSync); a sync point: syncfs; the create's syscall, probe and phase 3 (`b` recorded), and its end; the sync point clears D's row. A crash may now keep that database and lose the unsynced create, and recovery has nothing to forget. (Keeping only the inodes in flight at `ClearDirty` would not help: the create had ended.) |
| `readdirplus_unlocked` | Readdirplus listed after a suspension point without checking completeness again (a finding, fixed in R4); without the kernel lock | `ServedFromCacheIsCurrent` | a lookup populates D (`a` present, complete); a readdirplus finds D complete with its attributes unknown and goes to statx D; a create of `a` begins (phase 1: `a` unknown); the readdirplus fills D's attributes and lists the present rows: none, while `a` exists |
| `rename_stale_source` | Rename's phase 3 linked a source resolved before phase 1 without verifying it (a finding, fixed in R4); without the kernel lock | `CacheNeverWrong` | a rename of `b` over `a` resolves `o2` and runs phase 1; a rename of `a` over `b` probes `a` (`o1`); the first rename's renameat2 and phase 3 (`a` -> `o2`); the second's probe cannot be recorded but answers `o1`; it resolves `b` (absent), runs phase 1 and renameat2 (which moves `o2`), and phase 3 records `b` -> `o1` |

## Findings

Writing and checking the model exposed three places where the code did not
deliver what `docs/design.md` says the concurrency rules are for ("Rules
that hold now so that coroutines need no redesign"). None was reachable
today, since dcfs is single-threaded and the kernel serializes each
directory; each would have become a real bug under coroutines. Each was
kept as `findings/<name>.tla`/`.cfg`, with a test that expected its
counterexample, until the code was fixed (plan step R4). Then its
configuration became part of the real model, and the old behavior a
`Bug*` constant and a [known bug](#known-bugs-the-models-own-tests). A
new finding goes the same way.

- `sync_during_mutation`: a sync point cleared the dirty rows of mutations
  in flight. The real configurations used to set a `SyncExclusive`
  constant (no sync point overlaps a mutation); the constant is gone, and
  every configuration lets sync points interleave. Fixed by
  `cache::BeginSync`/`ClearDirty` (see `docs/design.md`, "Sync points").
- `readdirplus_unlocked`: Readdirplus (and Readdir of a non-root
  directory) checked completeness, ran syscalls ("."'s refresh,
  `ParentOf`), then listed without checking again, so a name made unknown
  in between was left out. `DirCacheFS::ListCached` now takes the listing
  right after the check, before any syscall. `MC_nolock.cfg` includes
  readdirplus (the finding's configuration).
- `rename_stale_source`: `Rename` resolved the source before phase 1 and
  phase 3 linked the destination to it if it `Owns` the parent, but `Owns`
  only sees overlaps from phase 1 on. A source answered from a probe that
  was not recorded (a concurrent rename was in flight) was stale once that
  rename ended, and phase 3 recorded the old object under the new name (in
  the code, `LinkDentry` then failed if the old object's row was gone, but
  not if it had another link). Now `cache::BeginRename` verifies, in phase
  1's transaction, that no mutation of the inodes the rename names began or
  ended since a snapshot taken before the resolve, or is in flight, and
  `Rename` resolves again if one did (`R1` in the model). `MC_nolock.cfg`
  includes rename (the finding's configuration).

## Trace validation

Model checking shows that the model keeps its promises; it says nothing
about whether the code does what the model says. Trace validation closes
that gap: the code records its protocol steps while tests run, and TLC
checks that each recorded run is a behavior of the model (the method of
Cirstea, Kuppe and Merz, "Validating traces of distributed programs against
TLA+ specifications", 2024). A trace the model cannot explain fails its
test and names the first event it cannot explain.

```sh
bazel test //dcfs:dir_cache_fs_trace_test     # the forged-request harness (~3 min)
bazel test //dcfs:all --test_tag_filters=e2e      # with the fault builds (test first)
bazel test //test/qemu:trace_crash_test //test/qemu:trace_power_test \
           //test/qemu:trace_rename_test //test/qemu:trace_create_test
```

The pieces:

- `dcfs/protocol_events.h`: one call (`ProtocolEvents`, reached through
  `Context::events`) for each step of the protocol, plus request frames
  that say which steps belong to one request. Production binaries call a
  no-op (`NoProtocolEvents()`; `main.cc` gets it from
  `MainProtocolEvents()`, defined in `dcfs/protocol_events_main.cc`). The
  call sites are the only change to production code.
- `dcfs/testonly/trace_recorder.cc`: the recorder. The daemon's recording
  build, `//dcfs:main_static_traced` (testonly), links
  `dcfs/testonly/main_recorder.cc` instead of `protocol_events_main.cc`
  (link-time selection), and writes to the serial console
  (`/dev/console`), which outlives a killed daemon; the harness installs a
  recorder itself (`StartTrace()` in `dcfs/dir_cache_fs_test.cc`) and
  writes to stdout, which in a unit-test guest is the serial console too.
- `formal/Trace.tla` and `Trace.cfg`: the trace-validation spec. It extends
  `dcfs.tla` and takes one step per event (`TraceNext`'s actions are named
  `T_<model action>`), reading the trace with the CommunityModules `Json`
  module (`ndJsonDeserialize`).
- `formal/trace_validate.sh` and `formal/trace.bzl` (`tla_trace_test`):
  boot the recording guest, split the serial log into traces, run TLC on
  each, and summarize (valid, invalid, where each trace was cut, action
  coverage).

### What a trace is

The model describes one directory. A run touches many, so the recorder
projects it onto every directory at once: each directory has its own
trace, and each trace is checked on its own. A line of the serial log is

```
DCFS-TRACE <trace> <dir> {"i":7,"c":"LookupDecided","ev":"lookup","p":"p1","req":{"k":"unlink","n":"a","m":""},"n":"a","out":"found","key":"k:15.1791271592.42903629","db":{...}}
```

`<trace>` names the run (a harness test, or `e2e` for a guest boot: every
daemon process of the boot appends to one trace), `<dir>` the directory's
inode id. `ev` is the event in the model's terms, `p` the request slot,
`req` the request's kind and names on its first line, `c` the C++ call it
came from (for people). `db` is the directory's cached state after the
event: its dentries (each `unknown`, `absent`, or the object it points at,
named by backing inode number and birth time), `complete`, `epoch`,
`valid` (its attributes), `dirty`, the clean-shutdown flag, `durable` (in
`Context::dirty.durable`) and `inflight` (`FillGuards::inflight`); a line
whose state equals its trace's previous line's leaves `db` out (the serial
console is slow) and `trace_validate.sh` puts it back. Every step compares
the model's state with it: an action that matches the event
but leaves the cache differently is no match. A trace starts with a
`begin` line (the state the directory starts in) and ends at the end of
the run, or at a `cut` line (below), or at `gone` (the directory's row was
deleted, e.g. by an rmdir).

Names are bytes: a trace spells each one as `EscapeBytes` does
(`dcfs/escape.h`), as a JSON string.

### Events

Each event, the model action it stands for (in its directory's trace), and
where `docs/design.md` describes the step. The call sites are commented
with the model step they mark; the rule they follow is that a call comes
right after the code the model's step stands for, with no backing syscall
(under coroutines: no suspension point) in between.

| Event (`ProtocolEvents::`) | Call site | Line | Model action (`Trace.tla`) | `docs/design.md` |
|---|---|---|---|---|
| `RequestBegin` / `RequestEnd` | `fuse_ops.cc` (`Serve`): dispatch, reply | `reply` | which steps are one request; `T_Reply`: the model's request has replied | Concurrency |
| `GetattrBegin` / `GetattrEnd` | `DirCacheFS::FreshAttr` | `attr_check`, `rdp_attr_check` | `Arrive` of a getattr (`GAFrom`); for Readdirplus's ".", part of its `RDFrom` (`T_ReaddirplusAttrCheck`) | Population policy; Concurrency (fill guards) |
| `LookupBegin` / `LookupEnd` | `backing::LookupOrPopulate` | | a LookupOrPopulate of a lookup, an unlink's or rename's resolve, or a failed mutation's re-resolve | Population policy |
| `RefreshBegin` / `RefreshEnd` | `backing::RefreshAttrs`, `RefreshAttrsFromFd` | `attr_check` (a refresh of unknown attributes that no request of the directory expects) | the statx and fill that end a getattr, readdirplus or mutation; otherwise a getattr of its own | The write-through protocol: Phase 3 |
| `SyncBegin` / `SyncEnd` | `backing::SyncBacking` | | a sync request (or, in `FinishRun`, `StopSync`/`StopClear`) | Sync points |
| `LookupDecided` | `LookupOrPopulate`, after the cache read | `lookup` | `LookupStep`, or the `Arrive` of a lookup, unlink or rename (`LKFrom`): served, `RN_probe` next, or `PD_read` next | Population policy |
| `ResolveProbed` | `ProbeChild` (for `ResolveName`), after its openat and statx | `probe` | `ResolveProbe` | Population policy; Concurrency |
| `ResolveCommitted` | `ResolveName`, after its transaction | `resolve_commit`; then the `child_fill` lines of its `ChildRowRecorded` | `ResolveCommit`; `T_GetattrWhole` (below) | Population policy |
| `ChildRowRecorded` | `RecordChild` (in a listing's or resolve's transaction), with the code's decision `filled` | `child_fill` (written once the transaction committed), or `unexplained` if it filled against the guard's rule | `T_GetattrWhole` | Population policy |
| `PopulateStarted`, `PopulateRead` | `PopulateDirectory`: after its snapshot and epoch; after phase A | `populate_read` (placed at `PopulateStarted`) | `PopulateRead` | Population policy |
| `PopulateCommitted` | `PopulateDirectory`, after phase B | `populate_commit`; then its `child_fill` lines | `PopulateCommit` | Population policy; Concurrency (completeness epoch) |
| `ListChecked` | `DirCacheFS::ListCached`, after `IsDirComplete` | `list_check` | `ReaddirStep`, or the `Arrive` of a readdir (`RDFrom`) | Population policy (Readdir) |
| `AttrsStatted` | `RefreshAttrs*`, after the statx | `stat` | `GetattrStat`, `ReaddirplusStat`, `CreateStat`, `UnlinkStat`, `RenameStat` | Phase 3 |
| `AttrsFilled` | `backing::FillAttrs`, after its transaction | `fill` | `GetattrFill`, `ReaddirplusFill`, `CreateFill`, `UnlinkFill`, `RenameFill` | Concurrency (fill guards) |
| `ParentRecorded` | `backing::ParentOf`, after recording the parent row, with the code's decision `filled` | `child_fill`, or `unexplained` if it filled against the guard's rule | `T_GetattrWhole` | Population policy |
| `RootRecorded` | `backing::InitRoot` | `child_fill` | `T_GetattrWhole` | Startup |
| `MutationBegun` | `cache::BeginMutation`, after the commit and `RegisterMutation` | `phase1` (`begun`, `synced`) | the create's `Arrive` (`C1From`), `UnlinkPhase1`, `RenamePhase1` | Phase 1 |
| `MutationAborted` | `cache::BeginMutation`, when `BeginRemove`/`BeginRename`'s verification fails | `phase1` (`aborted`) | the retry or `EAGAIN` case of `UnlinkPhase1`, `RenamePhase1` | Rules that hold now (resolves) |
| `NameResolved` | `RemoveChild`, `Rename`, after the (source) resolve | `resolved` | `UnlinkPhase1`'s ENOENT case; `RenameResolveDst` | Phase 1 |
| `MutationSyscallStarting` | `CreateChild`, `RemoveChild`, `Rename`, `Link`, before the syscall | (`unexplained` if phase 1 has not begun) | none: the recorder requires the request's phase 1 to have begun | Phase 2 |
| `MutationSyscall` | `CreateChild`, `RemoveChild`, `Rename`, `Link`, after the syscall | `syscall` | `CreateSyscall`, `UnlinkSyscall`, `RenameSyscall` | Phase 2 |
| `NewChildProbed` | `RecordNewChild`, after its openat and statx | `probe` | `CreateProbe` | Phase 3 |
| `MutationEnding`, `MutationEnded` | `cache::Mutation::End`: before and after it changes the guards | `end` (`owned`: what `Owns` said) | `CreatePhase3`, `UnlinkPhase3`, `RenamePhase3`; `CreateFailed`, `UnlinkFailed`, `RenameFailed` | Phase 3; Concurrency (`Owns`) |
| `Reresolve` | `ReresolveAfterFailure`, per name | `reresolve` | `RenameFailed2` for a rename's second name | Phase 2 |
| `WritesEnded` | `cache::EndWrites` | | not modelled (a file's; the model has no writable opens) | Writable opens |
| `SyncSnapshotTaken` | `SyncBacking`, after `cache::BeginSync` | `sync_begin`, `stop_sync` | a sync's `Arrive` (`S1From`); `StopSync` | Sync points |
| `SyncfsStarting` | `SyncBacking`, before the syncfs calls | (`unexplained` unless the snapshot came right before) | none: the recorder requires `SyncSnapshotTaken` to be the callback right before it | Sync points |
| `SyncfsDone` | `SyncBacking`, after the syncfs calls | `syncfs` | nothing (the model's syncfs takes effect at S1) | Sync points |
| `SyncCleared` | `SyncBacking`, after `cache::ClearDirty` | `sync_clear`, `stop_clear` | `SyncClearDirty`; `StopClear` | Sync points |
| `RunStarting` | `backing::StartRun`, first | `crash` (if the clean-shutdown flag is 0), `restart` | `Crash`, `Restart` | Crashes, power loss and recovery |
| `Recovered` | `StartRun`, after `cache::RecoverDirty` | `recover`, with the keys of the inodes that were dirty (read at `RunStarting`) | `Recover`, a dentry made unknown in a clean D only if its object's key is among them | Recovery |
| `RunStarted` | `StartRun`, after its kSync commit | `start_run` | `StartRun` | Startup |
| `ShutdownBegin`, `Checkpointed`, `CleanShutdownRecorded` | `backing::FinishRun` | `shutdown`, `checkpoint`, `clean` | `BeginShutdown`, `StopCkpt`, `StopFlag` | Shutdown |
| `OutOfBandChange` | `backing::ReconcileAttrs`, when it adopts a change | `cut` | none (no out-of-band changes in the model) | Out-of-band change detection |
| `InodeForgetting`, `InodeForgotten` | `cache::InvalidateInode` (and `DeleteInode`), before and after its DELETE | `gone`; `cut` (`invalidated`) | none. The recorder notes the present rows that point at the inode; once the (outermost) transaction has committed, a directory whose state changed by exactly those names becoming unknown is cut, any other change is `unexplained` | Identity model |

### The projection, and why it is sound

The model is one directory D; its `seq` is `FillGuards::touched[D]`, its
`inflight` `FillGuards::inflight[D]` (`dcfs.tla`'s comment on `CanFill`).
The code's clock is global, but a fill or sync snapshot's test of D,
`touched[D] <= snapshot`, is the same as the model's `seq <= snapshot`
measured in D's own touches: `touched[D]` only grows, and a touch of D
after the snapshot is at a global seq above it. The guards' pruning
(`floor`) is not modelled; no test comes near `FillGuards::max_touched`.

Which requests are which model request, in D's trace:

- LOOKUP(D, n) is a lookup of n; LOOKUP(D, ".") and GETATTR(D) and
  OPENDIR(D) a getattr; READDIR(D) a readdir; READDIRPLUS(D) at offset 0
  a readdirplus, at a later offset a readdir (a continuation has no "."
  entry, so it never refreshes D's attributes); MKNOD/MKDIR/SYMLINK/CREATE
  in D a create; UNLINK/RMDIR in D an unlink; RENAME within D with no flags
  a rename.
- A getattr of D inside another request (an `EntryFor(D)` replying D's
  entry from its parent's lookup, readdirplus or ".." lookup) is a getattr
  request of its own. A refresh of D's unknown attributes that no request
  of D expects (e.g. a rename's refresh of a directory it moved) is a
  getattr that found them unknown; one of valid attributes is no step at
  all (the model would serve them, and refreshing a correct value changes
  nothing it can see), but the recorder checks its fill: recording
  (`AttrsFilled`'s `recorded`) after a `phase1` or `end` line of D since
  the refresh began, or with a mutation of D in flight, is `unexplained`.
- A sync point is a sync request in every directory's trace.
- A LookupOrPopulate outside any request of D (a test resolving a name
  directly) is a lookup request.
- Each request holds the smallest slot `p1`, `p2`, ... no open request of
  D holds, until the C++ request ends (at which point the model's request
  must have replied: `T_Reply`). `p0` is never used by the code: it is the
  free slot `T_GetattrWhole` needs.

Steps that are more than one model action, each a sequence of model steps
with nothing in between, which the model allows since it allows every
interleaving:

- `ArriveAs`: the case of `Arrive` an event names (the request's kind and
  names), conjoined with `Arrive(p)` itself as a check.
- `T_GetattrWhole` (`child_fill` lines): a fill of D's attributes that is
  not one of D's own requests' (its parent's listing recording D's row,
  `ParentOf`, `InitRoot`): a whole getattr (`GAFrom`, `GetattrStat`,
  `GetattrFill`) taken at once, whose snapshot is therefore taken in the
  same step as its fill. The line carries the code's own decision
  (`filled`); the recorder makes one that filled against the guard's rule
  (`CanFill` with the code's snapshot) an `unexplained` line instead. Valid
  attributes stay valid (the value is not compared, as for the silent
  refresh above); a fill that did not record leaves unknown ones unknown.

Where the code's step is spread over syscalls and the model's is one:

- `PopulateRead` reads the listing and probes every name in one step; the
  code reads them over many syscalls, at which (in the harness, under
  coroutines) other requests run. The recorder puts the `populate_read`
  line where the population took its snapshot (`PopulateStarted`), before
  the lines of whatever ran during its reads. If something that ran then
  changed a name the reads saw, no behavior matches (the model's earlier
  read returns the old value) and validation fails: the reordering can
  only reject, never accept a run the model does not allow.
- When a syscall starts is checked by the recorder, not the model: a
  mutation's phase-2 syscall must start after its phase 1 began
  (`MutationSyscallStarting`), a sync point's syncfs calls right after its
  snapshot (`SyncfsStarting`). Otherwise a syscall issued before the step
  that must precede it, but returning after, would match the model's
  order.
- A resolve's snapshot (`ResolveName`'s `BeginFill`) and its probe are one
  step in the model (`ResolveProbe`) and two places in the code with one
  statx between them, at which no request runs today or in the harness.

What a trace observes and what it leaves free:

- Objects. The model names objects by identity (`o1`, `o2`, ...), the code
  by backing inode number and birth time. `Trace.tla`'s `okey` maps model
  objects to the code's keys as the trace reveals them, and every
  observation must agree with it. It is a function from model objects to
  keys, not a bijection: two names hard-linked to one file (which the
  model does not have) are two model objects with one key. It starts over
  at every restart, because a test may rearrange the backing filesystem
  while dcfs is down (`power.sh` recreates an unlinked file to stand for a
  power loss that lost the unlink; the model's power loss restores the
  original object). Within a run it is checked.
- The initial state is the model's `Init` except that the directory's
  cached state is the one its trace begins with (a directory's trace
  begins when the cache first has it, or when the harness starts
  recording), and that a name whose backing state the trace observes (a
  probe, a listing) before any syscall could have changed it starts in
  that state. The second only removes initial states. The first is the
  code's own state, so it is checked against how the row came to be (the
  begin line's `origin`): a directory created by a mkdir starts with an
  empty backing directory, no cached entries, its dirty row and epoch 0;
  one first seen in its parent's listing or by `ParentOf` with no entries,
  an incomplete listing, epoch 0 and no dirty row; a row that appears at
  any other step is `unexplained`. Only a directory already in the cache
  when the trace began (`existing`: at the harness's `StartTrace`, or a
  guest's first start) is assumed correct (and durable).
- Attribute values (the model's stamp), the guards' absolute clock, and
  which answer a listing served are not compared. Their effects are: the
  attributes' validity, the dentries a listing was built from, and the
  guard decisions, each the code's own: a fill's `recorded`, phase 3's
  `owned`, a verification's outcome, compared with the model's guard for
  every fill that is a model step; and for the fills over valid attributes
  that are no model step (a silent refresh, a `child_fill` over valid
  attributes), the recorder checks the decision against the mutations of
  D the trace saw. Before the review of 2026-10-06 those were dropped (a
  silent refresh had no check, and `child_fill`'s decision was the
  recorder's own computation, not the code's).
- Phase 1's durability: the code commits with a WAL fsync unless every
  inode it names is durably dirty, the model unless D is. Where the model
  takes the fast path the code may still fsync; that leaves fewer crash
  outcomes than the model allows, so the trace accepts either there
  (`SyncedOK`). Where the model fsyncs, the code must.

Soundness. Each trace step is a model step (or a sequence of them), so a
valid trace is a behavior of the model restricted to what the trace
observes. Nothing outside a directory's events may change its cached
state: after every event the recorder compares every other directory's
state with its last line, and a change gets an `unexplained` line, which
no action matches (an uninstrumented write fails validation where it
happened). It looks only outside transactions: a callback inside one (an
invalidation inside an upsert) leaves the check to the next callback after
the commit, so a half-written transaction is never compared. A step the model does not have ends the trace with a `cut`
line, `"why":"<category>: <detail>"`; what came before is still checked.
Only steps the model *lacks* are cuts, and each test lists the categories
its traces may end at (`allow_cuts` of `tla_trace_test`): any other cut
fails it. A step the model *forbids* is never a cut: it is written as a
line for the model to reject (a syscall before phase 1, a phase 1 before
the resolve, a second phase 1, a step of the wrong kind of request), or,
where no line can stand for it, as an `unexplained` line (a mutation that
ended before its syscall in a request that succeeded; a resolve, listing,
probe or refresh outside any request). A frame (request, getattr, lookup,
refresh, sync point) that returns an error ends the trace with a `failed`
cut; one that returns OK replies, and `T_Reply` requires its model request
to have replied, so a frame that skipped a step is rejected there. The
guest tests also name the root directory's trace (`root`), which must reach
the end of the run or one of the cuts listed for it (`root_cuts`). The
categories:

| Category | Why the model cannot follow | Allowed in |
|---|---|---|
| `cross-directory-rename`, `rename-flags` | the model's rename is within D, flags 0 | crash, rename |
| `link` | the model's objects never get a second name | crash, rename, create |
| `dir-attrs`, `dir-itself` | the model has no mutation of D's own attributes; `dir-itself`: D named as an object (removed, moved) | crash (both), rename, create (`dir-itself`) |
| `boundary` | a refused mount or subvolume boundary is not modelled | rename, create |
| `out-of-band` | not modelled (`ReconcileAttrs`) | create |
| `invalidated` | a forgotten inode's dentries became unknown (`InvalidateInode` after `ESTALE`) | none |
| `failed` | a syscall error other than create's `EEXIST` and unlink's/rename's `ENOENT`, or a frame that returned an error: the model's syscalls fail only that way, and its requests always finish | the guest tests |
| `overlapping-listings` | a listing of D during another listing's reads (the reordering of `populate_read` cannot place both) | none |

The root traces: `power.sh`'s reaches the end of the run; `crash.sh`'s ends
at its first cross-directory rename, `rename.sh`'s and `create.sh`'s at
their first link. Following them further needs the model (or the
projection) to have links and cross-directory renames, and for `rename.sh`
renames with flags and syscall failures.

### The traces

- `//dcfs:dir_cache_fs_trace_test` runs `dcfs/dir_cache_fs_test.cc` and
  validates every test that records (`StartTrace()`): the rename and
  unlink stale-resolve races (`RenameStaleSourceTest.*`,
  `UnlinkMarksWhatItRemovesUnknown`), a release and a MKDIR during a sync
  point's syncfs (`ReleaseDuringASyncPointKeepsTheDirtyRow`,
  `MkdirDuringASyncPointKeepsItsDirtyRows`), a writable create with a sync
  point inside it (`WritableCreateIsDirtyWhenReplied`), and scenarios
  written for trace validation: a rename, an unlink and a readdir that a
  mkdir in the same directory keeps invalidating until they give up
  (`*GivesUpWhile*`: phase 1's retry and EAGAIN, readdir's), the common
  requests in a row (`CommonRequestsMatchTheModel`), refreshes of unknown
  attributes (`UnknownAttributesAreRefreshed`), and the fault-injection
  scenario (`CreateMarksItsNameUnknown`). The harness runs requests inside
  other requests' syscalls, so it is validated without the kernel's lock
  (`KernelDirLock` FALSE).
- `//test/qemu:trace_{crash,power,rename,create}_test` run those guest
  scripts, unchanged, on the traced initramfs (`//test/qemu:initramfs_traced`),
  with the kernel's lock (`KernelDirLock` TRUE). `crash.sh` and `power.sh`
  cover crashes (a SIGKILLed daemon), recovery and clean shutdown.
- Test first: fault builds. Each `//dcfs:trace_fault_<fault>_test` runs one
  scenario of the harness in a build with a fault (a link-time `--wrap`
  fake in `dcfs/testonly/`, listed in `dcfs/BUILD.bazel`'s
  `DIR_CACHE_FS_FAULTS`) and passes only if validation rejects that
  scenario's trace at an event, and with a state, that show the fault:

  | Fault | Scenario | Rejected at |
  |---|---|---|
  | `skip_mark_unknown`: phase 1 does not mark names unknown | `CreateMarksItsNameUnknown` | the create's `phase1`, whose state has no row for `new` (the model's has it unknown) |
  | `syscall_before_phase1`: an unlink's unlinkat before its phase 1 | `TraceScenarioUnlink` | `unexplained` at `MutationSyscallStarting`, with nothing in flight and `a` still present (without that event: the `syscall` line, which the model rejects at `U1`) |
  | `phase3_before_syscall`: an unlink's phase 3 and End before its unlinkat | `TraceScenarioUnlink` | `unexplained` at the End: `a` absent with no syscall yet |
  | `snapshot_after_syncfs`: a sync point's snapshot taken after its syncfs calls | `TraceScenarioMkdirDuringSync` | `unexplained` at `SyncfsStarting`: no snapshot right before |

  For example:

  ```
  trace_validate.sh: rejected: DirCacheFSTest.CreateMarksItsNameUnknown@1: the model explains 0 of 5 events; the first it cannot (event 1):
    {"i":2,"c":"MutationBegun","ev":"phase1","p":"p1","req":{"k":"create","n":"new","m":""},"outcome":"begun","synced":true,"db":{"dent":[["a","k:14.1791271400.915708585"]],"complete":true,"epoch":0,"valid":false,"dirty":true,"clean":true,"durable":true,"inflight":1}}
  ```

  Before the review of 2026-10-06 (`docs/plan/audits/`), the two unlink
  faults validated (the syscall before phase 1 wrote no line, and the End
  before the syscall was a cut), and so did the sync fault (no event said
  when a syscall started).

- `//formal:trace_*_test` check `Trace.tla` itself on hand-written traces
  (`formal/trace_tests/`, no guest): the begin-line origins, and how
  recovery may forget a clean directory's dentries.

### Action coverage

Which model actions the valid traces took, and in which tests (the test
logs print the counts: states that matched an event; this table is from
the run of 2026-10-06). `Arrive` is split by request kind.

| Model action | Traces that take it |
|---|---|
| `Arrive`: lookup, unlink, rename (`T_ArriveLookup`) | harness, crash, power, rename, create |
| `Arrive`: readdir, readdirplus (`T_ArriveReaddir`) | harness, crash, power, rename, create |
| `Arrive`: getattr (`T_ArriveGetattr`) | harness, crash, power, rename, create |
| `Arrive`: create (`T_ArriveCreate`) | harness, crash, power, create |
| `Arrive`: sync (`T_ArriveSync`) | harness, power, rename |
| `LookupStep` | harness, power |
| `ResolveProbe`, `ResolveCommit` | harness, power |
| `PopulateRead`, `PopulateCommit` | harness, crash, power, rename, create |
| `ReaddirStep` (including the EAGAIN case) | harness, crash, power, rename, create |
| `ReaddirplusStat`, `ReaddirplusFill` | harness |
| `GetattrStat`, `GetattrFill` | harness, crash |
| `CreateSyscall`, `CreateProbe`, `CreatePhase3`, `CreateStat`, `CreateFill` | harness, crash, power, create |
| `CreateFailed` | harness |
| `UnlinkPhase1` (begin, retry, EAGAIN, ENOENT), `UnlinkSyscall`, `UnlinkPhase3`, `UnlinkStat`, `UnlinkFill` | harness, power, rename |
| `RenameResolveDst`, `RenamePhase1` (begin, retry, EAGAIN), `RenameSyscall`, `RenamePhase3`, `RenameStat`, `RenameFill` | harness, power |
| `SyncClearDirty` | harness, power, rename |
| `Crash`, `Restart`, `Recover`, `StartRun` | crash, power (`Restart`, `Recover`, `StartRun` also rename, create) |
| `BeginShutdown`, `StopSync` | crash, power, rename, create |
| `StopClear`, `StopCkpt`, `StopFlag` | power, rename, create |
| composite `T_GetattrWhole` | harness, crash, power, rename, create |

Never taken: **`UnlinkFailed`, `RenameFailed`, `RenameFailed2`**. They need
the name to vanish between the resolve and the syscall, which nothing in
the model can do (see [Changing the model](#changing-the-model): they
report 0 in every configuration too), and in the code only an out-of-band
change can, which the model does not have either. The code's failure path
for other errors (`ENOTEMPTY`, `EACCES`, ...) ends the trace with a cut
instead (a finding below).

### Findings of trace validation

Instrumenting the code and validating its traces exposed these places where
the code takes a step the model does not have. None is a safety problem
(each makes the cache know less, never more); each is a gap in the model,
reported here rather than fitted silently.

- **Recovery forgets more than the model's did** (now in the model).
  `cache::RecoverDirty` marks unknown every dentry that points at a dirty
  inode, wherever it is (the inode may have been renamed or unlinked); the
  model had no child objects in the dirty set, so its recovery never
  touched a clean D's dentries. `power.sh`'s root trace hit it (the root
  was clean; `a`, `b` and `f` were dirty, so the root's dentries for them
  became unknown). The model's `Recover` now may also make any of D's
  present dentries unknown (`RecoverForgetting`: those whose objects were
  dirty, which the model does not track), and `Trace.tla`'s `T_Recover` is
  that action, restricted by the `recover` line's `dirty_keys` (the keys of
  every inode that was dirty) to dentries pointing at one of them
  (`formal:trace_recover_forgets_*_test`). No known-bug variant: in the
  one-directory model a child is only ever dirty with D (every mutation of
  a child names D, and a sync point clears them together), so the old
  recovery and the new one leave the same states reachable for every
  property; forgetting more is always safe, and `CrashSafe` checks the
  least recovery forgets.
- **Syscall failures.** The model's create fails only with `EEXIST` and
  its unlink and rename only with `ENOENT`, each when the name says so; the
  code handles any error the same way (End, re-resolve, reply the error),
  e.g. an rmdir's `ENOTEMPTY` or a create's `EACCES`. Traces with such a
  failure are cut there (none of the four guest scripts' validated
  directories hit one in the run recorded above). A failure that changes
  nothing, at any syscall, would cover them.
- **Verification of other inodes.** `BeginRemove` and `BeginRename` verify
  the child (and the rename's other parent, source and destination) as
  well as D; the model verifies only D. A verification that failed only
  because of another inode would be a step the model does not allow
  (`UnlinkPhase1`/`RenamePhase1` would begin the mutation). No trace hit
  it: it needs a mutation of that inode alone between the resolve and
  phase 1.
- **A child row recorded while a mutation of it overlaps.** A parent's
  listing that records a directory's row (`RecordChild`) or `ParentOf`
  marks that directory's attributes unknown when it may not fill them,
  even if they were valid (a mutation of it ended since the listing's
  snapshot and its refresh made them valid). The model's fill would leave
  them valid. No trace hit it; `T_GetattrWhole` would reject it.
- **Phase 1's fast path** is taken by the model whenever D is durably
  dirty, by the code only when every inode it names is (see the
  projection). The code is the more durable one; validation accepts it.

### When trace validation fails

The log names the first event no behavior of the model explains, with the
line before it. Either the code took a step the model does not allow (a
bug, or a gap in the model to fix in `dcfs.tla` as the model's own tests
require), or a protocol change moved a step without moving its event, or
added a step without one (an `unexplained` line: the write that caused it
happened during the C++ call named in its `c` field). A change to the
protocol keeps its events next to the steps they mark, and the event
table above up to date.

## Reading a counterexample

When an invariant fails, the test log has:

```
Error: Invariant CacheNeverWrong is violated.
Error: The behavior up to this point is:
State 1: <Initial predicate>
/\ seq = 0
/\ bCur = [names |-> [a |-> "-", b |-> "-"], ver |-> 0]
/\ dbCur = [ dirty |-> FALSE, epoch |-> 0, dent |-> [a |-> "none", b |-> "none"], ... ]
...
State 2: <Arrive line 630, col 5 to line 667, col 34 of module dcfs>
...
```

Each `State n` lists every variable, and its heading names the action that
produced it (`<Arrive ...>`, `<CreateSyscall ...>`, `<Crash ...>`). The
last state is the one that breaks the property. To follow it, track a few
variables from state to state: usually `bCur.names`, `dbCur.dent`,
`dbCur.complete`, `dbCur.dirty`, each slot's `pc`, `kind` and `n`, and
`mode`. The crash F1 variant's counterexample, condensed to those:

| State | Action | Slots | `bCur.names` | `dbCur` |
|---|---|---|---|---|
| 1 | initial | idle | a: -, b: - | nothing cached, not dirty |
| 2 | `Arrive` (lookup a: miss, list D) | lookup(a): `PD_read` | | |
| 3 | `PopulateRead` | `PD_commit` | | |
| 4 | `PopulateCommit` | idle | | a: none, b: none, complete (both absent) |
| 5 | `Arrive` (create a: phase 1, not fsynced) | create(a): `C_sys` | | a: unknown, dirty |
| 6 | `CreateSyscall` | `C_probe` | **a: o3** | |
| 7 | `Crash` (keeps the backing change, loses phase 1) | idle | a: o3 | a: none, complete, **not dirty** |
| 8-10 | `Restart`, `Recover` (nothing dirty), `StartRun` | | | unchanged |
| | violated: `a` reads absent, the backing filesystem has `o3` | | | |

In the real model the phase-1 commit in state 5 is fsynced, so the crash
in state 7 cannot drop it: D stays dirty and recovery forgets `a`.

TLC also writes a trace-explorer module (`*_TTrace_*.tla`) next to the
specification. The TLA+ Toolbox or the VS Code TLA+ extension can load it
to step through the counterexample, but the log is enough.

## Changing the model

A change to the write-through protocol (mutation phases, fills and their
guards, completeness, the dirty set, sync points, recovery) updates this
model in the same change (AGENTS.md). In practice:

- Change the step (or add one) in `dcfs.tla` that stands for the code you
  changed; keep its comment naming the dcfs function.
- Run `bazel test //formal/...`. If a property fails, read the
  counterexample: either the code change is wrong, or the model needs
  more care.
- A new request kind: add it to `AllKinds`, an `Arrive` branch for its
  first step, one action per later step (listed in `Next` and in the
  action table above), and, if it mutates, its names in `MutatedNames`.
- A bug fixed in the code that the model can express: add a `Bug*`
  constant that puts it back, and a `known_bugs/` variant whose test
  expects the counterexample.
- Trace validation checks the code against the model: a changed step
  needs its protocol event (`dcfs/protocol_events.h`) at the place the
  model's step now stands for, `Trace.tla`'s action for it, and the event
  table in [Trace validation](#trace-validation) updated. Run
  `bazel test //dcfs:dir_cache_fs_trace_test //dcfs:trace_fault_*` (every
  `trace_fault_*_test`)
  and the `//test/qemu:trace_*_test` targets.
- Keep every action reachable: run TLC with `-coverage 1` and check that
  no action reports 0, except these, which report 0 in every
  configuration: `RenameFailed`, `RenameFailed2` and `UnlinkFailed`.
  Nothing in the model can remove a name between a rename's or an
  unlink's resolve and its syscall any more: under the kernel lock nothing
  runs there, and without it the phase-1 verification (`R1`, `U1`) refuses
  to begin while another mutation of D is in flight or has run since the
  resolve, and the model has no out-of-band changes. They stay because the
  code's failure paths do (an out-of-band change can still make the
  syscall fail). `MC_nolock.cfg` reaches both branches of a failed
  phase-1 verification (the retry and the `EAGAIN`).
