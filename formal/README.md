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
10. [Reading a counterexample](#reading-a-counterexample)
11. [Changing the model](#changing-the-model)

## Running it

```sh
bazel test //formal/...                    # everything (about 20 minutes)
bazel test //formal:small_test             # the real model, small bounds (~1 min)
bazel test //formal:known_bug_crash_f1_phase1_not_durable_test
```

(On russ's machine, wrap Bazel in `sg kvm -c '...'` as for every Bazel
command; these tests don't need KVM, but the Bazel server does.)

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
is a sign that part of the model is dead). For a known bug or a finding,
copy `known_bugs/*` or `findings/*` into the same directory and name its
module instead of `MC`.

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
with coroutines"). The code between two syscalls runs without interruption
(one thread, synchronous SQLite), but a crash can still happen between any
two of its transactions. Two switches say how much the environment
serializes:

- `KernelDirLock`: the kernel holds D's lock across every lookup, readdir
  and namespace mutation of D (the parent's `i_rwsem`, plus FUSE's
  per-directory lock because dcfs does not request
  `FUSE_CAP_PARALLEL_DIROPS`), but not across a getattr or an fsync. TRUE in
  the main configurations, because that is true today and stays true under
  coroutines; `MC_nolock.cfg` checks the model without it.
- `SyncExclusive`: a sync point never overlaps a mutation. That holds today
  only because dcfs is single-threaded; nothing in the code ensures it (see
  [Findings](#findings)).

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
- Not modelled: writable opens (`open_for_write`, which keeps attributes
  unknown and inodes dirty across sync points), xattrs, hard links, links
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
| `ReaddirStep` | Serve the listing if `IsDirComplete`; else populate (at most 3 attempts, then `EAGAIN`). Readdirplus first refreshes D's attributes if unknown | Population policy | `DirCacheFS::EnsureListed`, `Readdir`, `Readdirplus`, `cache::IsDirComplete`, `ListDir` |
| `ReaddirplusStat`, `ReaddirplusList` | Readdirplus: statx of D, then fill D's attributes and list the present rows | same | `DirCacheFS::Readdirplus`, `EntryFor` |
| `GetattrStat`, `GetattrFill` | Getattr of D with unknown attributes: statx, fill if `CanFill(D)`; reply what was read (a getattr with valid attributes is served in `Arrive`) | Concurrency (fill guards) | `DirCacheFS::Getattr`, `EntryFor`, `backing::RefreshAttrs`, `FillAttrs` |
| (create, first step) | Phase 1: the name and D's attributes unknown, D dirty, committed kSync unless D is durably dirty (fast path) | The write-through protocol: Phase 1 | `DirCacheFS::CreateChild`, `cache::BeginCreate`, `BeginMutation` |
| `CreateSyscall` | Phase 2: mkdirat/openat(O_CREAT)...; `EEXIST` if the name exists | Phase 2 | `backing::MkdirAt` etc. |
| `CreateProbe` | Probe the new name | Phase 3 | `backing::RecordNewChild` phase A |
| `CreatePhase3` | Record the dentry if `Owns(D)`; `Mutation::End`; fill snapshot for D's refresh | Phase 3, Concurrency (`Owns`) | `RecordNewChild` phase B, `Mutation::Owns`, `End` |
| `CreateStat`, `CreateFill` | Refresh D's attributes as a fill, reply | Phase 3 | `backing::RefreshAttrsFromFd` |
| `CreateFailed` | Failed phase 2: `End`, re-resolve the name, reply the error | Phase 2 | `ReresolveAfterFailure` |
| `UnlinkPhase1` | After resolving the name: `ENOENT`, or phase 1 (name and D's attributes unknown, D dirty) | Phase 1 | `DirCacheFS::RemoveChild`, `cache::BeginRemove` |
| `UnlinkSyscall` | unlinkat; `ENOENT` if gone | Phase 2 | `backing::UnlinkAt` |
| `UnlinkPhase3` | Name absent if `Owns(D)`; `End` | Phase 3 | `cache::SetNegative`, `Mutation::Owns` |
| `UnlinkStat`, `UnlinkFill`, `UnlinkFailed` | As for create | | `backing::RefreshAttrs`, `ReresolveAfterFailure` |
| `RenameResolveDst` | After resolving the source: `ENOENT`, or resolve the destination | | `DirCacheFS::Rename` |
| `RenamePhase1` | Both names and D's attributes unknown, D dirty | Phase 1 | `cache::BeginRename` |
| `RenameSyscall` | renameat2: moves whatever the source name holds now | Phase 2 | `backing::RenameAt` |
| `RenamePhase3` | If `Owns(D)`: destination -> the resolved source, source name absent; `End` | Phase 3 | `Rename`'s phase-3 transaction |
| `RenameStat`, `RenameFill`, `RenameFailed`, `RenameFailed2` | As for create (a failure re-resolves both names) | | `RefreshAfterRename`, `ReresolveAfterFailure` |
| (sync, first step) | syncfs: every backing write so far is durable | Sync points | `backing::SyncBacking` |
| `SyncClearDirty` | Empty the dirty set (normal durability), forget `dirty.durable` | Sync points | `cache::ClearDirty` |
| `Crash` | Daemon crash, kernel crash or power loss: each disk keeps any of its possible states, memory is lost | Crashes, power loss and recovery | |
| `Restart`, `Recover`, `StartRun` | Start again: `RecoverDirty` (one transaction), then `clean_shutdown = 0` with kSync | Recovery; Startup | `backing::StartRun`, `cache::RecoverDirty` |
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
are from russ's machine (4 cores); the state counts are what TLC reports
as distinct states.

| Configuration | Test (tier) | Bounds | States | Time |
|---|---|---|---|---|
| `MC_small.cfg` | `small_test` (medium) | 2 names, 2 slots, 2 mutations, 1 crash, kernel lock, all request kinds, all invariants | 241,381 | ~1 min |
| `MC_liveness.cfg` | `liveness_test` (medium) | as small with 1 slot, no VIEW; plus `RecoveryTerminates` | 32,839 | ~15-30 s |
| `MC_large.cfg` | `large_test` (large) | 3 mutations, 2 crashes | 2,637,471 | ~10 min |
| `MC_nolock.cfg` | `nolock_test` (large) | as small without the kernel lock; no rename or readdirplus (see Findings) | 1,098,314 | ~5 min |

The `View` (in `MC.tla`) merges database states a crash may leave when
recovery would make the same cache of them: a dirty state's rows are
forgotten by `RecoverDirty` anyway. That cuts the small configuration
from 409,410 states to 241,381. Liveness is checked without it, because
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

## Findings

Writing and checking the model exposed three places where the code does not
deliver what `docs/design.md` says the concurrency rules are for ("Rules
that hold now so that coroutines need no redesign"). None is reachable
today, since dcfs is single-threaded and the kernel serializes each
directory. Each would become a real bug under coroutines, and each is kept
as `findings/<name>.tla`/`.cfg` with a test that expects its counterexample.
When the code is fixed and the model updated to match, the test fails: then
fold the configuration into the real model.

| Finding | Expected | What happens |
|---|---|---|
| `sync_during_mutation` (`SyncExclusive = FALSE`) | `CrashSafe` | `SyncBacking` empties the dirty set (keeping only writable opens) without keeping inodes with a mutation in flight. A mutation whose phase 1 precedes the `syncfs` and whose syscall follows it loses its dirty row; a later power loss can keep its phase 3 and lose the syscall. Fix: `ClearDirty` also keeps `FillGuards::inflight`'s inodes, or the sync point waits for them |
| `readdirplus_unlocked` (no kernel lock) | `ServedFromCacheIsCurrent` | `Readdirplus` checks `IsDirComplete`, then refreshes "."'s attributes (syscalls), then `ListDir` lists present rows without checking again: a name made unknown in between is left out. `Readdir` has the same shape for a non-root directory (`ParentOf`'s syscalls). Under the kernel lock it would still need a name made unknown without D's lock (an `InvalidateInode`, not modelled) |
| `rename_stale_source` (no kernel lock) | `CacheNeverWrong` | `Rename` resolves the source before phase 1 and phase 3 links the destination to it if it `Owns` the parent; `Owns` only sees overlaps from phase 1 on. A source answered from a probe that was not recorded (a concurrent rename was in flight) is stale once that rename ends; phase 3 records the old object under the new name. In the code `LinkDentry` then fails if the old object's row is gone, but not if it has another link |

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
- Keep every action reachable: run TLC with `-coverage 1` and check that
  no action reports 0. Today `RenameFailed` and `RenameFailed2` fire only
  in `findings/rename_stale_source`, and `UnlinkFailed` only in
  `MC_nolock.cfg`: under the kernel lock nothing can remove a name between
  its resolve and the unlink or rename syscall.
