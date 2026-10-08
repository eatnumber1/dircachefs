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

A second, smaller model, `reval.tla`, covers what dcfs reuses across a
change of the backing filesystem's state (shared backing descriptors and
their access mode, cached permission state, cached directory answers): see
[The revalidation model](#the-revalidation-model).

A third, `lifetime.tla`, covers which nodeids the kernel holds and what
dcfs keeps for each (rows, removed records, the written-file set and its
held descriptors, open files), and when each goes: see
[The lifetime model](#the-lifetime-model).

A fourth, `ident.tla`, covers what a nodeid and its generation stand for:
how dcfs mints them, how a request and an NFS client's handle resolve to a
backing object, and what happens when the backing filesystem recycles an
inode number, at a crash, a power loss or a cache wipe, and with changes
behind dcfs's back, for today's identity and Phase 14's: see
[The identity model](#the-identity-model).

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
13. [The revalidation model](#the-revalidation-model)
14. [The lifetime model](#the-lifetime-model)
15. [The identity model](#the-identity-model)

## Running it

```sh
bazel test //formal/...                    # everything (about 20 minutes)
bazel test //formal:small_test             # the real model, small bounds (~1 min)
bazel test //formal:known_bug_crash_f1_phase1_not_durable_test
bazel test //formal:reval_test             # the revalidation model (~20 s)
bazel test //formal:lifetime_test          # the lifetime model (~40 s)
bazel test //formal:ident_test             # the identity model (~15 s)
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
- requests: lookup, readdir, readdirplus, getattr of D, create, linkcreate
  (the link of an unnamed `O_TMPFILE` file into D, step 23.4: a create
  whose phase 3 needs no probe), unlink, rename (within D), and sync
  points; crashes at any moment; startup (`StartRun`, `RecoverDirty`) and
  clean shutdown (`FinishRun`).

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
- Failed cache writes are not modelled: every `Commit` succeeds. A create
  whose syscall succeeded but whose new row cannot be recorded replies
  `EEXIST` (`CreatedButNotCompleted`, step 11.4), which the model's create
  never does after a successful syscall; trace validation ends such a
  trace ("failed").
- A `syncfs` that succeeds without making anything durable is outside the
  model, whose sync makes the backing filesystem's current state the only
  one a crash may leave. A filesystem that went read-only by itself after
  an error does that (step 11.5); the code guards it instead: a sync point
  fails when the source went read-only during the run, and dcfs refuses to
  start over a superblock read-only under a read-write mount
  (`docs/design.md`, "A filesystem that went read-only by itself").

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
| `ps[p]` | Request slot `p`: program counter `pc`, request `kind` and names `n`/`m`, whether it holds the kernel's lock, and locals (see `IdleProc` in `dcfs.tla`); and the reply ghost (step 12.7b): `eff` (what the backing filesystem answered at the request's syscall), `win` (the answers its queries had at earlier instants since its call), and on an idle slot `rep` (the last request's reply) and `rb` (flipped at every reply) | a FUSE request being served (the ghost fields: checking only) |
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
| `CreateSyscall` | Phase 2: mkdirat/openat(O_CREAT)... (a linkcreate: linkat of the unnamed file, which is new to D, and its phase 3 comes next, knowing it); `EEXIST` if the name exists | Phase 2 | `backing::MkdirAt` etc.; `backing::LinkAt` |
| `CreateProbe` | Probe the new name (not a linkcreate's) | Phase 3 | `backing::RecordNewChild` phase A |
| `CreatePhase3` | Record the dentry if `Owns(D)`; `Mutation::End`; fill snapshot for D's refresh | Phase 3, Concurrency (`Owns`) | `RecordNewChild` phase B (a linkcreate: `backing::RecordNewLink`), `Mutation::Owns`, `End` |
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
| `Interrupt` | With `Interrupts`: FUSE_INTERRUPT seen at a checkpoint, just before a backing syscall (`RN_probe`, `PD_read`, `PD_commit`: the population's reads abandoned; `C_sys`, `U_sys`, `R_sys`): the request replies `EINTR`; a mutation past phase 1 `End`s without phase 3, its names and D's attributes left unknown, D dirty, the backing unchanged. Never between a syscall and its phase 3 | Cancellation | `Checkpoint` (`dcfs/checkpoint.h`), `SessionLoop` |
| `Crash` | A kernel crash or power loss (`PowerLoss`: each disk keeps any of its possible states, and that is now all there is) or a daemon crash (`DaemonCrash`: the disks keep everything, but nothing more is durable than before, step 12.6b); memory is lost. `Next` takes it as `CrashServing`, `CrashRecovering` (from `Restart` to `ProbesDone`) and `CrashStopping`, so that coverage shows each fires. On `MC_small.cfg` (one crash, starting up) `CrashRecovering` can only follow a clean shutdown, an empty recovery; `MC_recovery.cfg` (two crashes) has a crash during the recovery of a dirty database (step 12.6) | Crashes, power loss and recovery | |
| `Restart`, `Recover`, `StartRun`, `ProbesDone` | Start again: `RecoverDirty` (one transaction; it may also make unknown any present dentry, standing for those that point at dirty children, which the model does not track: `RecoverForgetting`; it keeps the dirty set, which only a sync point clears: step 12.6b), then `clean_shutdown = 0` with kSync, then (after `InitRoot`'s fill) the probe of the recovered rows ends | Recovery; Startup | `backing::StartRun`, `cache::RecoverDirty`, `backing::Startup` |
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
| `GuardsBalanced` | invariant | `FillGuards::inflight` is the number of requests between their phase 1 and their `End` (an interrupted mutation releases its guard) |
| `RecoveryIdempotent` | invariant | Recovery may crash and start again (FSCQ's crash condition for recovery, step 12.6): while it runs (from a crash to `ProbesDone`), every database state a crash may leave recovers to a correct cache whatever a crash left of the backing filesystem. It is `CrashSafe` restricted to the recovery modes, named for what a crash during recovery relies on (recovery's own commits never leave a state it cannot start from again: `known_bugs/recover_clears_dirty_first`). Checked by `MC_small.cfg` and `MC_recovery.cfg` (in the large configurations `CrashSafe` already covers it) |
| `TypeOK` | invariant | Every variable has the expected shape |
| `EffectAtSyscall` | action property (`[][A]_vars`, step 12.7) | SibylFS's call, effect, return: what another request would see of D (`Observed`: each name and D's attributes from the cache where it knows them, else from the backing filesystem) changes only at a step that is a mutation's backing syscall, the actions `CreateSyscall`, `UnlinkSyscall` and `RenameSyscall` themselves (not merely a step from their pc: an interrupt there would be no effect point); never at a phase 1, a phase 3, a fill, or a step of a request with no syscall. A request takes its syscall at most once, so it has at most one effect point (a failed syscall has none). Checked on every transition: the observer reads D between any two steps. It follows from `CacheNeverWrong` and `BackingAtSyscall` (where the cache is correct, `Observed` is the backing filesystem); stated for what it says about requests, and for a change that breaks the cache's correctness at the wrong step |
| `BackingAtSyscall` | action property | While serving, the backing filesystem changes only at a mutation's syscall (a crash may undo unsynced changes: no request's effect). It holds by construction (only the syscall actions write `bCur`); it is the statement the other two rest on |
| `CacheLearnsAtCommit` | action property | The cache learns (a name it did not know, D's attributes) only at a commit of what a request read: a resolve's, a population's, an attribute fill's, a mutation's phase 3 (the actions), or, in trace validation, the fills the code makes outside a request slot (`OutOfSlotFill`, `Trace.cfg`: `GetattrWhole`). With `EffectAtSyscall`, a fill's effect is on the cache only and changes nothing anyone sees |
| `ReplyObservable` | action property (step 12.7b) | SibylFS's observation: every reply is one the backing filesystem gave. A mutation that reached its syscall replies that syscall's result (success, `EEXIST`, `ENOENT`); an answer (a lookup's entry or negative entry, a listing, D's attributes, an unlink's or rename's `ENOENT` from its resolve) is what the backing filesystem answered at some instant between the request's call and its reply; `EAGAIN` and `EINTR` come only from a request that had no effect. See [Replies](#replies-what-replyobservable-quantifies-over) for the exact quantifier and the errno classes |
| `RecoveryTerminates` | temporal | `(mode # "up") ~> (mode = "up")`: after any crash or shutdown, the daemon gets back to serving (recovery always terminates) |

### Replies: what `ReplyObservable` quantifies over

Each reply step records what the request replied (`Reply(p, v)` in
`dcfs.tla`): `v.e`, an errno class, and `v.a`, the set of answers the reply
carries (`FoundOrNeg`, `ListRes`, `AttrRes`: a lookup carries its entry or
negative entry, a readdir its listing, a getattr D's attributes, a
readdirplus its listing and D's attributes; a mutation, a sync or an error
carries none). The idle slot keeps `v` in `rep` until its next request
arrives, and flips `rb`, so that a reply is always a step that changes the
state (two equal replies in a row would otherwise be a stutter, which an
action property cannot see).

The errno classes are the errnos the model's requests reply: `ok` (a
negative entry is a successful reply, as `ReplyNegativeEntry` makes it),
`ENOENT`, `EEXIST`, `EAGAIN` and `EINTR`. Every other errno (`ENOTEMPTY`,
`EACCES`, `EIO`, ...) is one the model never replies: the model's syscalls
fail only with create's `EEXIST` and unlink's and rename's `ENOENT`, and a
trace whose request fails otherwise is cut there (`failed`, below). Adding
`ENOTEMPTY` would need children of children.

The instants of a request are the backing states from its call to its
reply: the one at its call and each one a syscall made since (the backing
filesystem changes at nothing else: `BackingAtSyscall`). Rather than keep
those states, each request keeps only the answers its own queries had at
them (`BAns`: a lookup's name, an unlink's or a rename's source name until
its phase 1, D's attributes for a getattr, the listing for a readdir, both
for a readdirplus): when a syscall changes D, every other request in
flight adds to `win` the answers it had before and no longer has
(`Remember`), so `Window(r) = r.win ∪ BAns(r, bCur)` is every answer the
backing filesystem gave that request's queries since its call. With the
kernel's lock no syscall comes during a lookup or a listing, so their `win`
stays empty; a getattr, which does not take the lock, is the one request
whose window can hold more than one instant there.

The property is checked on every step at which a slot replies (it becomes
idle, or an idle slot flips `rb`: a request that arrived and replied at
once):

```tla
ReplyWitnessed(r, v) ==
    CASE v.e \in {"EAGAIN", "EINTR"} -> r.eff = None /\ v.a = {}
      [] r.eff # None -> v = Rep(r.eff, {})
      [] r.kind = "sync" -> v = Rep("ok", {})
      [] OTHER ->
           /\ v.a # {} /\ v.a \subseteq Window(r)
           /\ IF r.kind \in {"unlink", "rename"}
              THEN v.e = "ENOENT" /\ \A x \in v.a : x.k = "neg"
              ELSE v.e = "ok"
```

where `r` is the slot's state before the reply step. A request that
arrived and replied in the same step (an answer served from the cache at
once) has one instant, the current one: `v.e = "ok"`, `v.a # {}`, and every
answer in `v.a` agrees with `bCur` (`ImmediateWitnessed`). In words:

- **A mutation that reached its syscall** replies what the backing
  filesystem answered there (`eff`, recorded by `CreateSyscall`,
  `UnlinkSyscall`, `RenameSyscall`): its effect point, the only instant
  that counts for it (`EffectAtSyscall`). A failed create replies `EEXIST`
  even though its re-resolve then finds the name. A create whose phase 3
  finds its new name gone replies `ENOENT`, which the property rejects (the
  syscall succeeded); the fill guards make that unreachable in the model
  (nothing can remove the name while the create is in flight), and in the
  code only an out-of-band change can, which the model does not have.
- **An answer** is in the request's window: the backing filesystem gave it
  at some instant between the call and the reply. For an answer served
  from the cache the quantifier is over the backing filesystem's states,
  not over what the cache says: the protocol lets the cache know an answer
  only while it is the backing's (`CacheNeverWrong`), so a served answer is
  the backing's answer at the instant it is served, and a cache that is
  wrong fails this property as well as `ServedFromCacheIsCurrent`. An
  answer read from the backing filesystem (a probe, a listing, a statx) is
  the backing's at the instant of the read, which is inside the window; a
  reply built from something read before the call, or from the cache's
  unknown row taken for an answer, is not
  (`known_bugs/reply_unknown_as_negative`).
- **A readdirplus's** listing and D's attributes are two answers, each
  checked on its own: the listing is taken from the cache before the statx
  of D, so without the kernel's lock they can come from different
  instants, as from two requests.
- **`EAGAIN` and `EINTR`** report that nothing happened: they are allowed
  only from a request that had no effect point, that is no syscall that
  succeeded (a mutation interrupted before its syscall; an unlink, rename
  or readdir that gave up; a failed mutation interrupted in its re-resolve,
  which replies `EINTR` rather than its syscall's error:
  `ReresolveAfterFailure`, and the model's `Interrupt` at the re-resolve's
  `RN_probe` or `PD_read`). An interrupt after a syscall that succeeded
  (`BugInterruptAfterSyscall`) breaks this too. The first version of the
  property allowed them only with no syscall at all; `MC_interrupt.cfg`
  found the interrupted re-resolve, which is the code's intended reply (a
  failed syscall changes nothing, and a failed syscall has no effect point
  under `EffectAtSyscall` either).

What the reply leaves out: the create's entry (its object and attributes;
the model's reply to a create is its errno class), and, in trace
validation, attribute values, which traces do not compare (below). The
idle slots' `rep` and `rb` are merged by the `View` (`IdleView` in
`MC.tla`): only the property, at the step that writes them, and trace
validation (which has no `VIEW`) read them, so they cost no states where a
`VIEW` is used.

## Configurations

`MC.tla` is the root module every configuration checks: it extends `dcfs`
and defines the request sets and the `View` the configurations use. Times
are from russ's machine (4 cores, loaded); the state counts are what TLC
reports as distinct states (since step 12.7b's reply ghost, run of
2026-10-08; before it, from step 23.4's `linkcreate` on: small 871,017,
recovery 25,861, liveness 101,898, large 9,164,576, nolock 6,365,804,
interrupt 226,438, interrupt_muts2 840,476, interrupt_nolock 966,942;
12.2b's `RecoverForgetting` had small at 687,731). The reply ghost's
windows (`win`) add the states where a request spans another's syscall:
with the kernel's lock only a getattr can.

| Configuration | Test (tier) | Bounds | States | Time |
|---|---|---|---|---|
| `MC_small.cfg` | `small_test` (medium) | 2 names, 2 slots, 2 mutations, 1 crash, kernel lock, all request kinds, all invariants, the three effect-point properties (step 12.7; also in `MC_recovery.cfg`, `MC_liveness.cfg` and `MC_interrupt.cfg`, and in `Trace.cfg`: every recorded trace is checked for them) and `ReplyObservable` (step 12.7b; also in `MC_recovery.cfg`, `MC_interrupt.cfg`, `MC_nolock.cfg`, `MC_interrupt_nolock.cfg` and `Trace.cfg`) | 935,825 | ~2 min (4 min at load 15, 2026-10-08) |
| `MC_recovery.cfg` | `recovery_test` (medium) | 1 name, 1 slot, 2 mutations, 2 crashes (one can come during the recovery of a dirty database: steps 12.6, 12.6b), all request kinds, all invariants and properties | 25,861 | ~10 s |
| `MC_liveness.cfg` | `liveness_test` (medium) | as small with 1 slot, no VIEW; plus `RecoveryTerminates`. The reply ghost is off (`RecordReply <- ForgetReply`): without a VIEW the idle slot's last reply tripled the states (338,790), for no property checked here | 101,898 | ~20-45 s |
| `MC_large.cfg` | `large_test` (large) | 3 mutations, 2 crashes | LARGE_COUNT | ~8 min unloaded (CI 634 s on 2026-10-08 before step 12.6b's daemon crash; 28 min alone at load 13) |
| `MC_nolock.cfg` | `nolock_test` (large) | as small without the kernel lock; `ReplyObservable` (lookups and listings overlap mutations here) | NOLOCK_COUNT | ~5 min unloaded (CI 506 s before step 12.6b's daemon crash; 16 min alone at load 13) |
| `MC_interrupt.cfg` | `interrupt_test` (medium) | as small with `Interrupts`, 1 mutation; plus `GuardsBalanced` | 230,662 | ~30-70 s |
| `MC_interrupt_muts2.cfg` | `interrupt_muts2_test` (large) | as small with `Interrupts`, no crash (a mutation after an interrupted one) | 892,708 | ~1-2.5 min |
| `MC_interrupt_nolock.cfg` | `interrupt_nolock_test` (large) | as nolock with `Interrupts`, 1 mutation; `ReplyObservable` | 1,036,982 | ~1.5-4 min |

`Interrupts` (Phase 22) is off in the first five: with it, `MC_small.cfg`
grows to 2,154,085 states (6 min), so the interrupts have configurations
of their own. All of them check `GuardsBalanced`.

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
| `effect_before_syscall` | a phase 1 that records a removal's outcome (the names absent) instead of marking them unknown (step 12.7; `MarkUnknown <- MarkAbsentEarly`) | `EffectAtSyscall` | a lookup populates D (a present); an unlink of a runs phase 1: a reads absent while the backing filesystem still has it |
| `effect_after_syscall` | crash F3 again (`BugCreateKeepsParentAttrs`), checked for its effect point (step 12.7) | `EffectAtSyscall` | a getattr records D's attributes; a create of a: phase 1 leaves them valid, so its syscall changes nothing others see; the refresh's fill does, after the syscall |
| `reply_after_failed_syscall` | not historical (step 12.7b): a failed mutation that replies success after re-resolving its name (`FailedReply <- FailedReplyOK`) | `ReplyObservable` | a exists (o1); a create of a: phase 1, its syscall fails with `EEXIST`; it ends and re-resolves a (a listing); it replies success, while the backing filesystem answered `EEXIST` at its only effect point. Every record stays right, so no invariant sees it |
| `reply_unknown_as_negative` | not historical (step 12.7b): a lookup whose population the guard refuses answers from the cache, its unknown row taken for a negative entry (the `kUnknown` that `LookupOrPopulate` must never return; `UnrecordedAnswer <- UnknownAsNegative`); without the kernel lock | `ReplyObservable` | a exists (o1); a lookup of a lists D (reads o1); a create of a runs phase 1 (a unknown); the listing cannot be recorded (a mutation in flight), and the lookup replies a negative entry: a existed at every instant between its call and its reply. The answer is not served from a known record and changes none, so no invariant sees it |
| `recovery_clears_dirty` | the start takes the recovered rows out of the dirty set when its probe ends, with no syncfs since the crashed run's backing syscalls (step 12.6b's first version; `ProbesDone <- ProbesDoneClearing`) | `CrashSafe` | a lookup populates D; an unlink of a: phase 1, syscall; a daemon crash (the unlink not yet durable); the start, whose probe clears D's dirty row; a lookup records a absent: a power loss may now bring a back while D is not dirty |
| `recover_clears_dirty_first` | `RecoverDirty` in two transactions, the first emptying the dirty set (step 12.6; put in by overriding `Recover` with `RecoverClearsDirtyFirst` from the configuration, no `VIEW`) | `RecoveryIdempotent` | a create of a: syscall, probe, phase 3 (a recorded present); a crash keeps that database and the backing filesystem before the create; the start's first transaction empties the dirty set: a crash may now keep a database that says a is present, with nothing left to make recovery forget it |
| `sync_during_mutation` | a sync point cleared the dirty rows of mutations in flight (a [finding](#findings) of this model, fixed in plan step R4) | `CrashSafe` | a create of `b`: phase 1 (D dirty, kSync); a sync point: syncfs; the create's syscall, probe and phase 3 (`b` recorded), and its end; the sync point clears D's row. A crash may now keep that database and lose the unsynced create, and recovery has nothing to forget. (Keeping only the inodes in flight at `ClearDirty` would not help: the create had ended.) |
| `readdirplus_unlocked` | Readdirplus listed after a suspension point without checking completeness again (a finding, fixed in R4); without the kernel lock | `ServedFromCacheIsCurrent` | a lookup populates D (`a` present, complete); a readdirplus finds D complete with its attributes unknown and goes to statx D; a create of `a` begins (phase 1: `a` unknown); the readdirplus fills D's attributes and lists the present rows: none, while `a` exists |
| `rename_stale_source` | Rename's phase 3 linked a source resolved before phase 1 without verifying it (a finding, fixed in R4); without the kernel lock | `CacheNeverWrong` | a rename of `b` over `a` resolves `o2` and runs phase 1; a rename of `a` over `b` probes `a` (`o1`); the first rename's renameat2 and phase 3 (`a` -> `o2`); the second's probe cannot be recorded but answers `o1`; it resolves `b` (absent), runs phase 1 and renameat2 (which moves `o2`), and phase 3 records `b` -> `o1` |
| `interrupt_after_syscall` | not historical (Phase 22): interruptible between the backing syscall and phase 3, cancelling by putting the resolved name back | `CacheNeverWrong` | a rename of `a` over `b` resolves `a`, runs phase 1 and renameat2; interrupted before phase 3, it puts `a` back, which the backing filesystem no longer has |
| `interrupt_undo` | not historical: an interrupt before the syscall puts the resolved name back instead of leaving it unknown; without the kernel lock | `TriState` | a rename of `a` runs phase 1; a create of `a` runs phase 1 (in flight); the rename, interrupted before its syscall, puts `a` back while the create is in flight |
| `interrupt_leaks_guard` | not historical: an interrupted mutation that never `End`s | `GuardsBalanced` | a create's phase 1; interrupted before its syscall, it replies without `End` |

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
| `RequestBegin` / `RequestEnd` | `fuse_ops.cc` (`Serve`): dispatch, reply | `reply` (`errno`: the status the frame returned, 0 if OK) | which steps are one request; `T_Reply`: the model's request has replied, with the same errno class (step 12.7b) | Concurrency |
| `GetattrBegin` / `GetattrEnd` | `DirCacheFS::FreshAttr` | `attr_check`, `rdp_attr_check` | `Arrive` of a getattr (`GAFrom`); for Readdirplus's ".", part of its `RDFrom` (`T_ReaddirplusAttrCheck`) | Population policy; Concurrency (fill guards) |
| `LookupBegin` / `LookupEnd` | `backing::LookupOrPopulate` | | a LookupOrPopulate of a lookup, an unlink's or rename's resolve, or a failed mutation's re-resolve | Population policy |
| `RefreshBegin` / `RefreshEnd` | `backing::RefreshAttrs`, `RefreshAttrsFromFd` (also the refresh that ends `DirCacheFS::ReconcileWritten`, step 23.1: a file's, never a directory's, so no directory's trace has a line for it) | `attr_check` (a refresh of unknown attributes that no request of the directory expects) | the statx and fill that end a getattr, readdirplus or mutation; otherwise a getattr of its own | The write-through protocol: Phase 3 |
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
| `ParentLookupStarted` | `backing::ParentOf`, after its fill snapshot | | (where `T_GetattrWhole`'s snapshot is) | Population policy |
| `ParentRecorded` | `backing::ParentOf`, after recording the parent row, with the code's decision `filled` | `child_fill`, or `unexplained` if it filled against the guard's rule | `T_GetattrWhole` | Population policy |
| `RootRecorded` | `backing::InitRoot` | `child_fill` | `T_GetattrWhole` | Startup |
| `MutationBegun` | `cache::BeginMutation`, after the commit and `RegisterMutation` (also `BeginAttrChange` in `DirCacheFS::ReconcileWritten` at a written file's last FORGET, step 23.1, outside any request: a file's, so no directory's trace has a line for it, and the recorder's check that no other directory changed covers it) | `phase1` (`begun`, `synced`) | the create's `Arrive` (`C1From`), `UnlinkPhase1`, `RenamePhase1` | Phase 1 |
| `MutationAborted` | `cache::BeginMutation`, when `BeginRemove`/`BeginRename`'s verification fails | `phase1` (`aborted`) | the retry or `EAGAIN` case of `UnlinkPhase1`, `RenamePhase1` | Rules that hold now (resolves) |
| `NameResolved` | `RemoveChild`, `Rename`, after the (source) resolve | `resolved` | `UnlinkPhase1`'s ENOENT case; `RenameResolveDst` | Phase 1 |
| `MutationSyscallStarting` | `CreateChild`, `RemoveChild`, `Rename`, `Link`, before the syscall | (`unexplained` if phase 1 has not begun) | none: the recorder requires the request's phase 1 to have begun | Phase 2 |
| `MutationSyscall` | `CreateChild`, `RemoveChild`, `Rename`, `Link`, after the syscall | `syscall` | `CreateSyscall`, `UnlinkSyscall`, `RenameSyscall` | Phase 2 |
| `NewChildProbed` | `RecordNewChild`, after its openat and statx | `probe` | `CreateProbe` | Phase 3 |
| `MutationEnding`, `MutationEnded` | `cache::Mutation::End`: before and after it changes the guards | `end` (`owned`: what `Owns` said) | `CreatePhase3`, `UnlinkPhase3`, `RenamePhase3`; `CreateFailed`, `UnlinkFailed`, `RenameFailed` | Phase 3; Concurrency (`Owns`) |
| `Reresolve` | `ReresolveAfterFailure`, per name | `reresolve` | `RenameFailed2` for a rename's second name | Phase 2 |
| `WritesEnded` | `cache::EndWrites` | | not modelled (a file's; the model has no writable opens) | Writable opens |
| `FileOpened`, `FileReleased` | the end of `DirCacheFS::Open`, `Release`, and of a successful `Create` or `Tmpfile` | (a file's trace: `open`, `release`) | none in `dcfs.tla`; `reval.tla`'s `OpenF`, `ReleaseF` ([below](#trace-validation-of-files)) | Writable opens and the flags |
| `SyncSnapshotTaken` | `SyncBacking`, after `cache::BeginSync` | `sync_begin`, `stop_sync` | a sync's `Arrive` (`S1From`); `StopSync` | Sync points |
| `SyncfsStarting` | `SyncBacking`, before the syncfs calls | (`unexplained` unless the snapshot came right before) | none: the recorder requires `SyncSnapshotTaken` to be the callback right before it | Sync points |
| `SyncfsDone` | `SyncBacking`, after the syncfs calls | `syncfs` | nothing (the model's syncfs takes effect at S1) | Sync points |
| `SyncCleared` | `SyncBacking`, after `cache::ClearDirty` | `sync_clear`, `stop_clear` | `SyncClearDirty`; `StopClear` | Sync points |
| `RunStarting` | `backing::StartRun`, first | `crash` (if the clean-shutdown flag is 0), `restart` | `Crash`, `Restart` | Crashes, power loss and recovery |
| `Recovered` | `StartRun`, after `cache::RecoverDirty` | `recover`, with the keys of the inodes that were dirty (read at `RunStarting`) | `Recover`, a dentry made unknown in a clean D only if its object's key is among them | Recovery |
| `RunStarted` | `StartRun`, after its kSync commit | `start_run` | `StartRun` | Startup |
| `RecoveryDone` | `backing::Startup`, after the probe of the recovered rows (step 12.6b) | `recovery_done` | `ProbesDone` | Startup |
| `ShutdownBegin`, `Checkpointed`, `CleanShutdownRecorded` | `backing::FinishRun` | `shutdown`, `checkpoint`, `clean` | `BeginShutdown`, `StopCkpt`, `StopFlag` | Shutdown |
| `LifetimeChanged` | after every `++lookups_` (`ReplyEntry`, `Readdirplus`'s entries, `Create`, `Tmpfile`), a successful `Open`, the end of `Release`, each `Forget` and `ForgetMulti` entry, phase 3 of `RemoveChild` and of a rename over an object (`RefreshAfterRename`), and `backing::Startup`'s probe of a recovered row | (a nodeid's trace: `lookup`, `create`, `tmpfile`, `open`, `release`, `forget`, `removed`) | none in `dcfs.tla`; `lifetime.tla`'s ([below](#trace-validation-of-nodeids)) | Row lifetime; mmap after close |
| `Destroyed` | the end of `DirCacheFS::Destroy` | (every nodeid's trace: `destroy`) | `lifetime.tla`'s `Destroy` | mmap after close |
| `Interrupted` | `Checkpoint` (`dcfs/checkpoint.h`), when the request being served was interrupted | `interrupt` (a mutation's with its `End`; a population's where it started, its reads abandoned); the request's `EINTR` reply is its `reply` | `T_Interrupt` | Cancellation |
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
  in D a create; a LINK into D of an unnamed `O_TMPFILE` file a linkcreate
  (`events::Op::kLinkTmpfile`, which `fuse_ops.cc` picks when
  `DirCacheFS::IsUnnamedTmpfile`; step 23.4); UNLINK/RMDIR in D an unlink;
  RENAME within D with no flags a rename. TMPFILE in D is no request of
  D's (it changes nothing cached about D), nor are COPY_FILE_RANGE and
  IOCTL of a file (a file's attributes are outside the model); an IOCTL of
  D that sets its flags is a `dir-attrs` cut, one that reads them
  (`lsattr`) nothing (the ioctl's command travels in the request's
  `flags`).
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
  an `unexplained` line instead, judged from its own record: a phase 1 or
  end line of D since the fill's snapshot event (`PopulateStarted`,
  `LookupDecided` `resolve`, `ParentLookupStarted`), or a mutation of D in
  flight, and not `cache::CanFill` with the snapshot the code reports (a
  snapshot taken late would agree with itself). A fill whose snapshot
  event was not seen is unexplained too. Valid
  attributes stay valid (the value is not compared, as for the silent
  refresh above); a fill that did not record leaves unknown ones unknown.

Where the code's step is spread over syscalls and the model's is one:

- `PopulateRead` reads the listing and probes every name in one step; the
  code reads them over many syscalls, at which (in the harness, under
  coroutines) other requests run. The recorder puts the `populate_read`
  line where the population took its snapshot (`PopulateStarted`), before
  the lines of whatever ran during its reads (held until then). If the
  trace ends while lines are held (a cut, an `unexplained` line, the
  directory gone), the held lines are dropped and the end line is written
  at once: the trace ends where the population began. If something that ran then
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
where no line can stand for it, as an `unexplained` line (a resolve,
listing, probe or refresh outside any request). Where whether a step is
lacking or forbidden depends on how its request ends (a mutation that ends
before its syscall, a syscall error the model does not have, a listing
whose reads never report), the trace ends there and the request's end
decides: a `failed` cut if it failed, `unexplained` if it replied OK. A
frame (request, getattr, lookup, refresh, sync point) that returns an error
ends the trace with a `failed` cut, unless the error is one the model's
request replies (create's `EEXIST`, unlink's and rename's `ENOENT` from
their syscall, `EAGAIN`, `EINTR`); otherwise it replies, and `T_Reply`
requires its model request to have replied, so a frame that skipped a step
is rejected there. The reply line carries the frame's errno (`errno`), and
`T_Reply` requires the model's reply (`rep`, see
[Replies](#replies-what-replyobservable-quantifies-over)) to have its class
(`ReplyErrnoOK`): an errno names the class (Linux's numbers: 2, 4, 11,
17); 0 is a success, or the `ENOENT` that `RemoveChild` and `Rename` reply
themselves (`ReplyErrno`, so their frame returns OK) when the resolve of
their (source) name found nothing, which the model's reply marks by
carrying that negative answer. A reply line's errno is the frame's status,
not the bytes sent to the kernel; no recorder hook sees those. `Trace.cfg`
also checks `ReplyObservable` on every recorded behavior. The
guest tests also name the root directory's trace (`root`), which must have
events and reach the end of the run, its last line the run's final event
(`clean` or `stop_clear`, with no later line of the run but that step's
for other directories), or end at one of the cuts listed for it
(`root_cuts`; a `gone` line or a cut whose category does not parse never
qualifies). `formal/trace_tests/root_*.log` test these rules. The
categories:

| Category | Why the model cannot follow | Allowed in |
|---|---|---|
| `cross-directory-rename`, `rename-flags` | the model's rename is within D, flags 0 | crash, rename |
| `link` | the model's objects never get a second name (an unnamed `O_TMPFILE` file's first one is a linkcreate, not a cut) | crash, rename, create |
| `dir-attrs`, `dir-itself` | the model has no mutation of D's own attributes (a setattr, xattr change or flag-setting ioctl of D); `dir-itself`: D named as an object (removed, moved) by a request that resolved one of its names to D (else `unexplained`) | crash (both), rename, create (`dir-itself`) |
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
  attributes (`UnknownAttributesAreRefreshed`), an `O_TMPFILE` file linked
  into a name, after a link onto an existing one failed
  (`TmpfileLinkedIntoANameIsACreate`: linkcreate's begin, `EEXIST` and
  success), a written file's reconciliation at its last FORGET
  (`LastForgetOfAWrittenFileReconcilesItsAttributes`: a file's mutation
  outside any request, which no directory's trace may show), and the
  scenarios the
  fault builds break (`CreateMarksItsNameUnknown`, `TraceScenarioUnlink`,
  `TraceScenarioMkdirDuringSync`), whose traces must validate here.
  `//dcfs/testonly:trace_recorder_test` tests the recorder's own decisions
  (invalidations, fills over valid attributes) on an in-memory cache. The harness runs requests inside
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
  | `phase3_before_syscall`: an unlink's phase 3 and End before its unlinkat | `TraceScenarioUnlink` | `unexplained` at the request's reply: the mutation ended before its syscall and the request replied OK (`a` absent) |
  | `snapshot_after_syncfs`: a sync point's snapshot taken after its syncfs calls | `TraceScenarioMkdirDuringSync` | `unexplained` at `SyncfsStarting`: no snapshot right before |
  | `swallow_syscall_error`: an unlink's syscall reports EBUSY, which the request ignores, replying OK | `TraceScenarioUnlink` | `unexplained` at the request's reply: a syscall error the model does not have, and its request replied OK |
  | `create_syscall_before_phase1`: a create's syscall started before its phase 1, inside a readdir's population of the same directory | `TraceScenarioMkdirDuringListing` | `unexplained` at `MutationSyscallStarting`, written although the directory's lines were held for the listing |

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
  (`formal/trace_tests/`, no guest): the begin-line origins, how
  recovery may forget a clean directory's dentries, and the reply's errno
  (step 12.7b: a failed create's `EEXIST` is valid, a reply of success
  after it is rejected at the reply line, and an unlink's `ENOENT` from its
  resolve is valid with errno 0).

A crash during recovery (steps 12.6, 12.6b):
`//dcfs:trace_crash_during_recovery_test` runs the harness built so that
the first `RecoverDirty` fails before it writes anything, as if the daemon
died there (`dcfs/testonly/recover_dirty_fails_once.cc`), and the first
probe of a recovered row fails, leaving it as a crash before that probe
would (`probe_fails_once.cc`, both by `-Wl,--wrap`; the target defines
`DCFS_CRASH_DURING_RECOVERY`, under which the test checks that both faults
fired), on the scenario written for it, `CrashDuringRecoveryRecoversAgain`:
a crash between an unlink's syscall and its phase 3; a start that dies in
recovery; a start whose probe of the file's row fails (the row stays,
unprobed and dirty); a start that probes it (the row goes); lookups
answered from the backing filesystem while the recovered rows are still
dirty; the first sync point, which takes them out. Its traces must be
valid (the root's: crash, restart, crash, restart, recover, start_run,
child_fill, recovery_done, crash, restart, recover, start_run, child_fill,
recovery_done, two lookups, a sync point). Before step 12.6b the second
start emptied the dirty set and the third probed nothing ("Value of:
cache::GetAttr(ctx_, f).status() ... Actual: OK"); in its first version
the start took the probed rows out of the dirty set with no syncfs
("Value of: cache::ListDirty(ctx_) Expected: ... an element is equal to
1"). `trace_tests/recover_crash.log` keeps the root's trace:
`//formal:trace_recover_crash_test` checks it against the real model
(valid) and `//formal:trace_recover_crash_split_test` against
`known_bugs/recover_clears_dirty_first` as the model (`--trace-cfg`,
`trace_tests/recover_clears_dirty_first.cfg`, a definition override), which
must reject it at the first `recover` line, a recovery the code does in one
transaction.

### Action coverage

Which model actions the valid traces took, and in which tests (the test
logs print the counts: states that matched an event; this table is from
the run of 2026-10-06). `Arrive` is split by request kind.

| Model action | Traces that take it |
|---|---|
| `Arrive`: lookup, unlink, rename (`T_ArriveLookup`) | harness, crash, power, rename, create |
| `Arrive`: readdir, readdirplus (`T_ArriveReaddir`) | harness, crash, power, rename, create |
| `Arrive`: getattr (`T_ArriveGetattr`) | harness, crash, power, rename, create |
| `Arrive`: create (`T_ArriveCreate`; linkcreate in the harness) | harness, crash, power, create |
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
| `Interrupt` (`T_Interrupt`) | harness (the cancellation tests: a lookup before its population, a population at its first probe batch, an unlink and a mkdir before their syscalls) |

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
  (`formal:trace_recover_forgets_*_test`). The new behaviours are a
  superset of the old (`RecoverForgetting(d, {}) = RecoverDirty(d)`), so
  more states are reachable (`small.cfg` now reaches 687,731 distinct
  states, more than before) and no invariant or `Bug*` configuration is
  weakened: every old behaviour is still checked. Forgetting more is
  always safe (unknown is), and `CrashSafe` still checks the least
  recovery forgets (`RecoverDirty`). No known-bug variant: the old
  recovery is not a bug of the model's, only narrower than the code's.
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
  Every step that replies calls `Reply(p, v)` with what the code replies
  (step 12.7b), and the request's queries go in `BAns`, so that
  `ReplyObservable` can check the reply.
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
- A change to what dcfs reuses across a change of the backing state (the
  shared backing fd and its access mode, the write fd, the GETFLAGS
  re-check, what the kernel's permission check reads, a cached answer a
  backing change could invalidate) updates `reval.tla` instead, with its
  `FileOpened`/`FileReleased` events and `RevalTrace.tla` (see
  [The revalidation model](#the-revalidation-model)).
- A change to what dcfs keeps for a nodeid and when it goes (lookup
  counting, FORGET and FORGET_MULTI, removed records, the written-file set
  and its held descriptors, row retirement, DESTROY, the start's sweep of
  unnamed rows) updates `lifetime.tla`, with its `LifetimeChanged` events
  and `LifetimeTrace.tla` (see [The lifetime model](#the-lifetime-model)).
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

## The revalidation model

`reval.tla` is a second, separate model for one class of bug: dcfs reuses
something it holds across a change of the backing filesystem's state that
should have invalidated it. Phase 23 found two: a writable open reused a
read-write shared backing descriptor opened before `chattr +i` (the open
was granted and wrote), and `chattr +F` through dcfs made a directory
case-insensitive under dcfs's byte-keyed cached entries
(`docs/plan/audits/review-2026-10-07-phase23.md`, M1; L-b in step 23.7).
The invariant: nothing dcfs holds grants more than the backing filesystem
would grant at the moment of use, except where POSIX grandfathers it (an
open descriptor keeps the rights it was opened with).

How it relates to `dcfs.tla`: it shares no module and no state. `dcfs.tla`
is one directory's cached records under the write-through protocol, with
interleaving and crashes; it has no files, no descriptors and no
permission state, and its backing directory changes only through dcfs.
`reval.tla` has no crashes and no interleaving (each request is one step,
at its effect point: a request concurrent with another may be ordered
before or after it, as on a local filesystem), and instead has what the
other leaves out: one file F with its permission state, dcfs's shared
backing fd, write fd and open handles of it, dcfs's cached mode and the
kernel's cached attributes, and one directory D whose lookups can become
case-insensitive. The two never need each other's state: what
`dcfs.tla` checks (that a cached record is unknown or right) is
`reval.tla`'s starting assumption (`Init`: whatever is cached is right).

```sh
bazel test //formal:reval_test //formal:reval_oob_test //formal:reval_liveness_test
bazel test //formal:known_bug_reval_no_recheck_test    # and the other known_bug_reval_*
bazel test //formal:limitation_out_of_band_mode_test   # and the other limitation_*
bazel test //formal:trace_reval_chattr_through_dcfs_test  # and the other trace_reval_*
```

By hand, as above, with `MCreval` (or a variant module) instead of `MC`:
`-config MC_reval.cfg MCreval`.

### What is in it

| Variable | Meaning | In dcfs |
|---|---|---|
| `bF` | F's permission state on the backing filesystem: `w` (its mode bits and ACL let the caller write), `imm`, `app` (`FS_IMMUTABLE_FL`, `FS_APPEND_FL`) | the backing file |
| `cF` | dcfs's cached attributes of F: `valid`, and `w` | `inodes.attrs_valid`, the cached mode and owner |
| `kAttr` | the kernel's cached attributes of F (model-only: no event observes them) | the FUSE inode's attributes, for `--attr_timeout_sec` |
| `sfd` | F's shared backing fd: none, `"rw"` (opened `O_RDWR`) or `"ro"`; `chg` is only for `BugRecheckOnlyIfChanged` | `DirCacheFS::BackingFile::fd`, `writable` |
| `wfd` | the write fd beside a read-only shared fd: none, `"plain"`, `"append"` | `BackingFile::write_fd`, `write_fd_appends` |
| `hs` | dcfs's open handles of F: mode (`"r"`, `"w"`, `"wa"`: with `O_APPEND`), and whether the backing filesystem's flags (`fl`) and mode (`ml`) allowed it when it was granted | `open_files_` |
| `lastOpen`, `writeErr` | history: the last OPEN's outcome (who refused it, if anyone, and what the backing filesystem would have said at that moment), and whether a write through dcfs went wrong | (checking only) |
| `bD`, `cD` | D on the backing filesystem (its casefold flag, the names stored in it: `"a"` and `"A"`, which casefold makes one), and dcfs's cached answer for each name (a complete listing records the names it did not list as absent) | the backing directory; `dentries`, `children_complete` |
| `changes` | changes of the backing state so far (the bound) | (checking only) |

| Action | What it does | dcfs |
|---|---|---|
| `Open(h, m)` | The kernel's `default_permissions` check (its cached attributes, or a GETATTR that dcfs answers from its cache or a fresh statx); then a fresh shared fd (`O_RDWR` as root, `O_RDONLY` if the flags refuse it), or the existing one: if read-write, the GETFLAGS re-check (EPERM on immutable, or append-only without `O_APPEND`); if read-only, a reopen with the open's mode, which becomes the write fd (the first writer's, replaced only by one without `O_APPEND`). A writable open marks F's attributes unknown | `DirCacheFS::Open`, `MakeBackingFile`, `BeginWriting` |
| `Release(h)` | The last writable release records F's attributes from the fd and drops the write fd; the last release closes the shared fd | `DirCacheFS::Release`, `EndWriting`, `RecordWrittenAttrs` |
| `Write(h)` | dcfs writes for an open through `WriteFd()` (EBADF with none; an appending one for an open without `O_APPEND` writes at the end) | `Write` (fallback), `Fallocate`, `CopyFileRange` |
| `Getattr` | A GETATTR when the kernel has nothing cached | `Getattr`, `FreshAttr` |
| `Chmod(v)` | A chmod through dcfs (refused by the backing filesystem on an immutable or append-only file); the reply goes into the kernel's cache | `Setattr` |
| `SetFlags(imm, app)` | `FS_IOC_SETFLAGS` of F through dcfs | `Ioctl` |
| `Expire`, `EvictF`, `EvictD(x)` | The kernel's attribute timeout passes; dcfs forgets F's attributes or a name's answer | `cache::InvalidateInode` |
| `DirStep(x)`, `ListD` | A lookup of an unknown name (a probe), a create or unlink through dcfs, a complete listing | `ResolveName`, `CreateChild`, `RemoveChild`, `PopulateDirectory` |
| `DirChange(v)` | `FS_IOC_SETFLAGS` of D changing `FS_CASEFOLD_FL`: refused by dcfs (EOPNOTSUPP), so it happens only with `BugCasefoldPassedThrough` | `Ioctl` (step 23.7) |
| `OutOfBandChange` | If `OutOfBand`: any change of F's mode and flags, or `chattr +F`/`-F` of an empty D, behind dcfs's back | |

| Property | Kind | Says |
|---|---|---|
| `OpenExact` | invariant | A dcfs OPEN succeeds exactly when the backing filesystem would let the caller open F in that mode at that moment |
| `OpenFlagsExact` | invariant | Once past the kernel's check, an open is granted exactly when F's flags allow it (what a shared fd could otherwise grant past) |
| `OpenModeExact`, `OpenModeNeverGrantsMore` | invariant | The kernel's check, which reads cached attributes, refuses exactly when F's mode does; never grants what the mode refuses |
| `HeldFlagsLegit`, `HeldModeLegit` | invariant | Every writable open dcfs holds was allowed when it was granted |
| `WriteFdHeld`, `WriteFdKeepsOffsets`, `WritesUseAWritableFd` | invariant | With a writable open, dcfs has a descriptor that can carry its writes, one that does not append if some writable open does not; every write went through one |
| `WriteFdOnlyBesideWriters` | invariant | The write fd exists only beside writable opens over a read-only shared fd (it goes with the last writable release) |
| `DirCacheNeverWrong` | invariant | A cached answer for a name of D agrees with a lookup on the backing directory |
| `CachedModeCurrent`, `KernelModeCurrent` | invariant | dcfs's and the kernel's cached mode agree with F's |
| `NothingGrantsMore` | invariant | `OpenExact`, `HeldFlagsLegit`, `HeldModeLegit`, `WritesUseAWritableFd` and `DirCacheNeverWrong`: the phase's invariant |
| `CachedDecisionsConverge` | temporal | Once the changes stop, the kernel's permission decision and every cached answer agree with the backing filesystem from some point on |

Only write permission is modelled: read and execute go through the same
check of the same cached mode, so they would add states and no new way to
go wrong.

`OutOfBand` is a constant. dcfs requires exclusive access
(`docs/design.md`, "Assumptions"), so the real configuration has it
FALSE; with it TRUE, the model says what breaks without that rule. The
flags part does not break (`MC_reval_oob.cfg`): the GETFLAGS re-check and
the reopen ask the backing file, so they see a `chattr` made by anyone.
The mode part and D's cached answers do (`limitations/`).

### Configurations

Distinct states from TLC's report (run of 2026-10-07 on russ's machine, 2
workers; a violating run's count varies a little with the workers'
order). `MCreval.tla` adds the symmetry of the handles, for safety
checking only.

| Configuration | Test (tier) | Checks | States | Time |
|---|---|---|---|---|
| `MC_reval.cfg` | `reval_test` (medium) | 2 handles, 3 changes, through dcfs only; every invariant | 69,104 | ~15 s |
| `MC_reval_oob.cfg` | `reval_oob_test` (medium) | as above with `OutOfBand`; the flags part: `OpenFlagsExact`, `HeldFlagsLegit`, `WriteFd*`, `WritesUseAWritableFd` | 442,736 | ~45-70 s |
| `MC_reval_liveness.cfg` | `reval_liveness_test` (medium) | 2 changes, no symmetry; `CachedDecisionsConverge` and `NothingGrantsMore`. A smoke check: without `OutOfBand` the safety invariants already force the property in every state, so it holds trivially here; its content is `limitations/out_of_band_stale` | 67,632 | ~20 s |
| `known_bugs/reval_recheck_if_changed_exclusive.cfg` | `reval_recheck_if_changed_exclusive_test` (medium) | `BugRecheckOnlyIfChanged` without `OutOfBand`: no violation | 69,104 | ~15 s |

Coverage (`-coverage 1` on `MC_reval.cfg`): every action fires. `Write`,
`Getattr`, `LookupD` and `ListD` add no distinct state there (a write
changes nothing when it succeeds, and the others reach states `Init`
already has); `DirChange` and `OutOfBandChange` are disabled (they need a
bug constant or `OutOfBand`).

### Known bugs and limitations

Each is a test that passes only if TLC reports the expected violation
(small tier, a few seconds each).

| Variant | Bug | Expected | The counterexample |
|---|---|---|---|
| `known_bugs/reval_no_recheck` | a writable open sharing a read-write fd took the fd's mode (before step 23.7; review L-b) | `OpenFlagsExact` | open `O_WRONLY` (shared fd `O_RDWR`); `chattr +i` through dcfs; open `O_WRONLY` granted |
| `known_bugs/reval_recheck_if_changed` | the declined fix: re-check only after a flag change through dcfs; with `OutOfBand` | `OpenFlagsExact` | open `O_WRONLY`; `chattr +a` (and `chmod a-w`) behind dcfs's back; open `O_WRONLY` granted |
| `known_bugs/reval_casefold_passed_through` | `SETFLAGS` changing `FS_CASEFOLD_FL` forwarded (review M1) | `DirCacheNeverWrong` | `a` cached absent; `chattr +F` of the empty D; create `A`; `a` still absent while the backing finds `A` |
| `known_bugs/reval_no_write_fd` | no write fd beside a read-only shared fd (review L1) | `WriteFdHeld` | open `O_WRONLY \| O_APPEND` of an append-only file: the shared fd falls back to `O_RDONLY`, the open is allowed, nothing can carry its writes |
| `known_bugs/reval_write_fd_last_writer` | each writable open replaced the write fd (review L-a) | `WriteFdKeepsOffsets` | a read-only shared fd; open `O_WRONLY` (write fd plain); open `O_WRONLY \| O_APPEND`: the write fd appends, for both |
| `known_bugs/reval_write_fd_never_dropped` | not historical: the write fd kept past the last writable release (before `WriteFdOnlyBesideWriters`, such a model passed every configuration; only trace validation caught it) | `WriteFdOnlyBesideWriters` | a read-only shared fd; open `O_WRONLY` (a write fd); release it with the read-only open still there: the write fd stays |
| `limitations/out_of_band_mode` | `OutOfBand`: a chmod behind dcfs's back | `OpenModeNeverGrantsMore` | the kernel has F's attributes cached (writable); `chmod a-w` on the backing file; open `O_WRONLY` granted (dcfs's own cached mode does the same once the kernel asks it, and dcfs opens as root) |
| `limitations/out_of_band_casefold` | `OutOfBand`: `chattr +F` behind dcfs's back | `DirCacheNeverWrong` | as the casefold bug, with the flag set directly on the backing directory |
| `limitations/out_of_band_stale` | `OutOfBand`: nothing revalidates a cached record | `CachedDecisionsConverge` (liveness) | a change behind dcfs's back, after which nothing has to happen that invalidates the stale cached mode |

### Trace validation of files

`RevalTrace.tla` validates one file's trace against `reval.tla`, as
`Trace.tla` does a directory's against `dcfs.tla` (same validator, same
depth rule). The recorder writes files' traces only when made with `files`
(the forged-request harness's `StartTrace()` does; the guests' recorder
does not yet), on lines `DCFS-REVAL <trace> <file> <json>`. A file's trace
begins at an open that finds no shared backing fd (the model's `Init` has
none), or at a create; a file first seen shared is not traced. A create's
open is mapped to `OpenF` although the kernel checks a creating open
against the parent directory, not the file's mode; harmless, since a
trace leaves `bF.w` free.

| Line | From | Model step (`RevalTrace.tla`) |
|---|---|---|
| `open` (`mode`, `shared`, `errno`, `fd`) | `FileOpened` | `T_Open`: `OpenF` of some closed handle; `shared` must be whether the model has a shared fd, errno 0 must be a grant and EPERM a refusal by the re-check or the reopen (the kernel's own EACCES never reaches dcfs); any other errno is a `failed` cut |
| `release` (`writable`, `fd`) | `FileReleased` | `T_Release`: `ReleaseF` of an open handle that may write iff `writable` (dcfs's own record of the open: a RELEASE's flags are not the open's) |
| `setflags` (`errno`, `imm`, `app`) | `RequestEnd` of an IOCTL `FS_IOC_SETFLAGS` (its argument: `Request::ioctl_arg`), or `FS_IOC_FSSETXATTR` (values free) | `T_SetFlags`: `SetFlagsF`, or `FailedAttrChangeF` if it failed |
| `getflags` | `RequestEnd` of an IOCTL `FS_IOC_GETFLAGS` | `T_GetFlags`: no change |
| `chmod` (`errno`) | `RequestEnd` of a SETATTR of the mode, uid or gid (`Request::flags` is setattr's `to_set`) | `T_Chmod`: `ChmodF` (its value free: the trace does not know the caller), or `FailedAttrChangeF` |
| `write` (`errno`) | `RequestEnd` of a WRITE, FALLOCATE or COPY_FILE_RANGE into the file | `T_Write`: `WriteF`; errno 0 must find a descriptor that can write, EBADF none; another errno is a `failed` cut |
| `oob` | the test (`TraceRecorder::NoteOutOfBand`), after changing the backing file directly | `T_OutOfBand`: any change of F's mode and flags |

Each `open` and `release` line carries the shared backing fd the code left
(`fd`: none, `rw` or `ro`, the write fd, the outstanding opens and writable
opens), which the model's must equal. What no event observes, and stays
model-only: the kernel's cached attributes and every permission decision
it takes from them, dcfs's cached mode (a file's `attrs_valid` changes at
many steps this projection does not follow), the caller (so the mode's
value), and D (no directory event is mapped onto `reval.tla`; `dcfs.tla`'s
traces cover D's records, but not casefold, which dcfs refuses).

The traces: `//dcfs:dir_cache_fs_trace_test` validates the files' traces
of every recording test, among them
`WritableOpenOfAnImmutableFileIsRefused` (`chattr +i` behind dcfs's back),
`WritableOpenAfterChattrThroughDcfsIsRefused`,
`WritableOpenOfAnAppendOnlyFileNeedsOAppend`,
`WritableOpenAfterChattrMinusIWritesThroughItsFd`,
`AnAppendingWriterKeepsTheFirstWritersFd` and
`ChmodAndChattrOfAnOpenFileMatchTheRevalModel` and
`RefusedAndFsxattrFlagChangesMatchTheRevalModel` (the run of 2026-10-07:
eleven files' traces, all valid). Every `T_*` action is taken, and every
branch of each (a granted and a refused open, a fresh and a shared one; a
SETFLAGS with its flags, an FSSETXATTR with them free, and a refused one;
a chmod that succeeds and one refused), except `T_Write`'s EBADF branch,
which the fixed code cannot reach (`WriteFdHeld`).

One gap in what the events see: an error a handler replies itself
(`req.ReplyErrno`) reaches `RequestEnd` as OK, so a SETFLAGS that dcfs
refuses that way (the casefold refusal, EOPNOTSUPP) is recorded as a
successful `setflags` with the flags it asked for. Where those equal the
file's (as `chattr` sends them) the model's state is still right; where
they do not, validation fails later, never accepts wrongly. No traced test
does it.
`formal/trace_tests/reval_*.log` keep four of them, and the
`//formal:trace_reval_*_test` targets check them against the real model
and against known-bug variants as the model (`--reval-cfg`,
`trace_tests/reval_<bug>.cfg`):

| Test | Log, model | Expected |
|---|---|---|
| `trace_reval_chattr_through_dcfs_test` | `chattr +i` through dcfs, real model | valid |
| `trace_reval_chattr_through_dcfs_no_recheck_test` | the same, `BugNoRecheck` | rejected at the refused `O_WRONLY` open after `setflags` (`imm`) |
| `trace_reval_chattr_through_dcfs_recheck_if_changed_test` | the same, `BugRecheckOnlyIfChanged` | valid (the change went through dcfs) |
| `trace_reval_chattr_out_of_band_test` | `chattr +i` behind dcfs's back, real model | valid |
| `trace_reval_chattr_out_of_band_{no_recheck,recheck_if_changed}_test` | the same, either variant | rejected at the refused open |
| `trace_reval_chmod_and_chattr_test` | chmod, `chattr +a`, a refused chmod, the opens, real model | valid |
| `trace_reval_chmod_of_append_only_test` | the same with the refused chmod edited to succeed | rejected there |
| `trace_reval_appending_writer_test` | two writers beside a read-only fd, real model | valid |
| `trace_reval_appending_writer_write_fd_last_writer_test` | the same, `BugWriteFdLastWriter` | rejected at the `O_APPEND` open (the code kept the write fd plain) |

For example, against `BugNoRecheck`:

```
trace_validate.sh: rejected: reval/t@2: the model explains 3 of 10 events; the first it cannot (event 4):
  {"i":5,"c":"FileOpened","ev":"open","mode":"w","shared":true,"errno":1,"fd":{"sfd":"rw","wfd":"none","refs":1,"wrefs":1}}
```

## The lifetime model

`lifetime.tla` is a third, separate model, of nodeids: which ones the
kernel holds, what dcfs keeps for each, and when each thing goes. The
kernel counts the entry replies that hand a nodeid out and gives them back
with `FORGET` (or a `FORGET_MULTI` batch), the last one when it evicts the
inode; dcfs counts the same replies (`DirCacheFS::lookups_`) and keeps,
besides the row, an in-memory removed record for an object it removed
while the kernel holds its nodeid (`removed_`, step 23.2), and for a file
written in this run an entry in `written_` with a held `O_PATH`
descriptor from its last close to its last `FORGET` (step 23.6,
`docs/design.md` "mmap after close"). The invariant: a nodeid the kernel
holds always resolves to the object it was handed out for (never to
another, and not to `ESTALE` while the kernel's reference keeps the object
alive), and nothing dcfs keeps for a nodeid outlives the kernel's
references or goes before them.

How it relates to the other two: it shares no module and no state.
`dcfs.tla` is one directory's cached entries under the write-through
protocol, with interleaving and crashes that keep any prefix of each
disk's writes; it has no nodeids, no lookup counts, no files and no
descriptors. `reval.tla` is one file's descriptors and permission state.
`lifetime.tla` has what both leave out: the kernel's lookup counts and open
files per nodeid, dcfs's count, row (and the row's nlink column), removed
record, `written_` entry and held descriptor, the backing directory's names
and whether each object is still allocated (freed once nothing names it
and no descriptor of dcfs's holds it). It takes each request as one step,
except an unlink's or rename's removal, which is two (the backing syscall,
then phase 3's settling) so that a crash can fall between them; a crash is
a daemon crash (the database keeps what it committed; a power loss's
rollback is `dcfs.tla`'s).

```sh
bazel test //formal:lifetime_test //formal:lifetime_stubs_test
bazel test //formal:known_bug_lifetime_nonfinal_forget_drops_held_test  # and the other known_bug_lifetime_*
bazel test //formal:trace_life_nonfinal_forget_test                     # and the other trace_life_*
```

By hand, as above, with `MClifetime` (or a variant module) instead of
`MC`: `-config MC_lifetime.cfg MClifetime`.

### What is in it

Per nodeid (`st[i]`, a record; nodeids are rows' ids `1..MaxId`, handed out
in order and never twice, and stubs' `StubIds`):

| Field | Meaning | In dcfs |
|---|---|---|
| `k` | the kernel's lookup count: entry replies minus `FORGET`s | the FUSE inode's `nlookup` (model-only) |
| `ko` | the object (or refused name) the kernel's lookups were handed out for, since `k` last rose from 0 | (checking only) |
| `op` | open files of it | `open_files_`, `BackingFile::refs` |
| `wo` | ... of which may write | `BackingFile::writable_refs` |
| `wrote` | a writable open since `k` last rose from 0, in this run | (checking only) |
| `lk` | dcfs's count of the kernel's lookups | `lookups_` |
| `row`, `nl0` | its row exists; the row's nlink column is 0 | `inodes` |
| `rec` | a removed record answers for it | `removed_` |
| `wr` | no `written_` entry, one without a held descriptor (`nofd`: the cap, or `EMFILE`), or one with (`held`) | `written_` |

Besides: `obj[i]` (the object row `i` was made for), `nextId`
(`AUTOINCREMENT`), `stub[m]` (a refused name's stub nodeid), `bName` (the
backing directory: names `Names` to objects `Objs`), `bState` (each object
`unborn`, `alive` or `dead`), `pend` (a removal between its syscall and its
phase 3, and whether `HoldForRemoval` held the object), `run`, `clean` (the
clean-shutdown flag), `crashes`, `forgetErr` (a `FORGET` forgot more than
dcfs counted).

| Action | What it does | dcfs |
|---|---|---|
| `Lookup(n)` | the row of n's object, found or made (a new id), handed out | `Lookup`, a `READDIRPLUS` entry (`ReplyEntry`) |
| `Create(n, w)`, `Tmpfile` | a new object, its new row (`Tmpfile`'s with nlink 0 and no name), handed out and open | `Create`, `Tmpfile` (`RecordTmpfile`) |
| `Link(i, n)` | a new name for a nodeid the kernel holds (an `O_TMPFILE` file's first: nlink no longer 0), handed out | `Link` |
| `Open(i, w)` | an open of a nodeid the kernel holds, of its row or its removed record; a writable one puts it in `written_` (not for a removed record) | `Open`, `BeginWriting` |
| `Release(i, w)` | the last release of a row with no link left retires it (into a removed record if dcfs counts lookups); that of a written file otherwise takes the held descriptor, or none (the cap) | `Release`, `RetireRemoved` |
| `Remove(n, src)`, `Settle` | an unlink of n (or a rename of src over it): the resolve, `HoldForRemoval` if dcfs counts lookups, the syscall; then phase 3: an open object keeps its row (nlink 0 if no link is left), otherwise the row goes if no link is left, into a removed record if held, and the `written_` entry with it | `RemoveChild`, `RefreshAfterRename`, `SettleUnlinkedFile`, `RetireRemoved` |
| `Forget(i, n)`, `ForgetMulti(f)` | the kernel gives back n lookups (all of them only with no file open); dcfs takes them off, and at its last ends the removed record and the `written_` entry (a batch: each entry's count, in one step) | `Forget`, `ForgetMulti`, `DropLookups`, `ReconcileWritten` |
| `Destroy` | unmount: the kernel holds nothing (no `FORGET`s), dcfs's memory is cleared; the shutdown is clean unless a row still has a writable open (it keeps the row durably dirty, and `FinishRun` leaves the flag unset then). With `DestroyWithOpens`, files may still be open | `Destroy`, `FinishRun` |
| `Crash`, `Restart`, `ProbeRow(i)`, `ProbesDone`, `SyncPoint` | the daemon dies (also between a removal's two steps, and during the start's probe): the kernel's and dcfs's memory go, the database stays; the next start sweeps rows with nlink 0 and no name and, after an unclean shutdown, lists the recovered rows (the row of a removal the crash cut), which stay dirty; it probes each listed row by handle (it goes once its object is freed) and serves; a sync point takes the row out of the dirty set (step 12.6b) | `StartRun`, `ProbeRecoveredRows`, `cache::ForgetUnnamedRows`, `SyncBacking` |
| `Refuse(m)`, `LookupStub(m)`, `StubGone(m)` | a refused name's stub (the next nodeid up from the highest live stub's), handed out; with `OutOfBand`, a stub going mid-run | `cache::SetRefused`, `StubEntry` |

| Property | Says |
|---|---|
| `NodeidStable` | (1) a nodeid the kernel holds resolves to the object it was handed out for, or to `ESTALE`, never to another (stubs' nodeids included) |
| `ReferencedServed` | ... and never to `ESTALE`: the kernel's reference keeps a removed object reachable, as on a local filesystem (step 23.2); not for a stub that went after an out-of-band change (`ESTALE` is its answer) |
| `NotRetiredWhileReferenced` | (2) an object's row or removed record goes only when the kernel holds no lookup, no file is open and no held descriptor is left; a removed record never stands beside a row |
| `HeldOnlyWhileWritten` | (3) a `written_` entry (so a held descriptor) exists only for a file with a writable open since `k` last rose from 0, whose last `FORGET` has not come, and which still has its row (dcfs's own removal of its last link drops it) |
| `WrittenUntilLastForget` | ... and such a file keeps it until its last `FORGET`, the only later event at which a store through a mapping after close can be seen |
| `ForgetKnown` | (4) no `FORGET` forgets a lookup dcfs did not count (`DropLookups`'s error path is unreachable) |
| `LookupsExact`, `NothingLeaks` | (5) dcfs's count is the kernel's; a removed record and a `written_` entry exist only while the kernel holds the nodeid |
| `KernelForgotAfterCrash`, `UnnamedRowsSwept` | (6) after a crash the kernel holds nothing and dcfs's memory is empty; once started, no row with nlink 0 and no name is left that nothing has open |
| `RowsNameLiveObjects` | stronger than `UnnamedRowsSwept`: no row of a freed object (since step 12.4b's probe of the recovered rows) |
| `RecoveryIdempotent` | (7) recovery may crash and start again (step 12.6): every row the start still has to probe is still in the dirty set, so a crash during the probe leaves it to the next start (since step 12.6b) |

Two of them hold by construction and are there to say so: `NodeidStable`
for rows (ids come from a counter that never goes back, `AUTOINCREMENT`,
and a crash resets the kernel), and `KernelForgotAfterCrash` (`Crash` and
`Destroy` reset every nodeid). Their content is the stubs' nodeids
(`known_bugs/lifetime_stub_nodeid_reused`) and the variants that break the
counting.

### Configurations

Distinct states from TLC's report (run of 2026-10-07 on russ's machine, 2
workers; a loaded machine takes up to three times as long).

| Configuration | Test (tier) | Checks | States | Time |
|---|---|---|---|---|
| `MC_lifetime.cfg` | `lifetime_test` (medium) | names a, b (o1, o2), o3 to create, 2 row ids, lookups to 2, 2 opens per nodeid, 1 crash; every property | 317,992 | ~70 s |
| `MC_lifetime_destroy_opens.cfg` | `lifetime_destroy_opens_test` (medium) | as above with 1 open per nodeid and `DestroyWithOpens`; every property | 107,281 | ~30 s |
| `MC_lifetime_stubs.cfg` | `lifetime_stubs_test` (medium) | 1 row id, 2 refused names, 2 stub nodeids, stubs going after out-of-band relistings (`OutOfBand`; `ReferencedServed` then excludes stubs, whose answer after going is `ESTALE`) | 134,770 | ~40 s |
| `MC_lifetime_recovery.cfg` | `lifetime_recovery_test` (medium) | as `MC_lifetime.cfg` with 1 lookup and 1 open per nodeid and 2 crashes (one can come during the start's probe: steps 12.6, 12.6b); every property, `RecoveryIdempotent` included (every configuration checks it) | 23,082 | ~10 s |

Coverage (`-coverage 1`): on `MC_lifetime.cfg` every action fires except
the stubs' (no refused names there); `MC_lifetime_stubs.cfg` takes `Refuse`
and `LookupStub` (its one row id leaves `Create` nothing to make).
`StubGone` needs `OutOfBand`, which `MC_lifetime_stubs.cfg` has.

### Known bugs and findings

Each is a test that passes only if TLC reports the expected violation (small
tier, a few seconds each).

| Variant | Bug | Expected | The counterexample |
|---|---|---|---|
| `known_bugs/lifetime_nonfinal_forget_drops_held` | a `FORGET` that is not the last ends the `written_` entry and its held descriptor (the destroy_test investigation's hypothesis, false in the code) | `WrittenUntilLastForget` | two lookups of a; a writable open; a `FORGET` of one lookup: the entry is gone while the kernel holds the nodeid (a store after it would never be reconciled) |
| `known_bugs/lifetime_nonfinal_forget_drops_rec` | a `FORGET` that is not the last ends the removed record | `ReferencedServed` | two lookups of a; unlink a (held; phase 3 makes the record); a `FORGET` of one: nothing pins the object, and the nodeid the kernel holds resolves to `ESTALE` |
| `known_bugs/lifetime_tmpfile_row_survives_crash` | no sweep of unnamed rows at the start, nor a probe of the recovered rows (before step 23.7, review L5) | `UnnamedRowsSwept` | `TMPFILE`; crash; start: its row stays, nothing open |
| `known_bugs/lifetime_forget_multi_counted_as_one` | each `FORGET_MULTI` entry takes off one lookup, not its nlookup | `LookupsExact` | two lookups of a; a `FORGET_MULTI` of both: dcfs counts one left, the kernel none (and a record or held descriptor would outlive the last `FORGET`) |
| `known_bugs/lifetime_stub_nodeid_reused` | found by this model (formerly a finding), fixed in step 12.4b: a stub's nodeid was the next up from the highest live stub's (`MAX(id) + 1`), so once the highest stub went, the next refused name got its nodeid again, which the kernel could still hold. Stubs went on any out-of-band change detected in their parent (`ForgetNegativeDentries` deleted every refused dentry and the trigger its stub; the relisting minted them again in listing order, so two boundaries could swap nodeids), and the kernel's revalidation then marked the old inode bad (`EIO`). Fixed by `cache_state.last_stub_id` (a persisted high-water mark; not `AUTOINCREMENT`: stub ids are negative) and by keeping a stub while its refusal is only forgotten | `NodeidStable` (with `OutOfBand`) | refuse m1 (stub 11); look it up; m1's stub goes; refuse m2: stub 11 again, which the kernel holds for m1 |
| `known_bugs/lifetime_crash_before_settle` | found by this model (formerly a finding), fixed in step 12.4b: a crash between an unlink's (rmdir's, rename's) backing syscall and its phase 3 left the row of an object with no name left, its nlink column still not 0, so the start's sweep kept it. Fixed by `ProbeRecoveredRows` (`backing.cc`), which `backing::Startup` runs after `InitRoot` and `StartupPurge` (the mount fds): at a start after an unclean shutdown, every row recovery found dirty is probed by handle, and goes if its object is gone or has no link left, directories included | `RowsNameLiveObjects` | unlink a (nothing held); crash; start: a's row stays, its object freed |
| `known_bugs/lifetime_probe_list_in_memory` | found by this model (step 12.6, formerly `findings/lifetime_crash_during_probe`), fixed in step 12.6b: the start's probe worked from a list in memory, `RecoverDirty` having emptied the dirty set, so a crash during the probe lost the rows not yet probed (`Restart <- RestartForgetsProbeList`) | `RowsNameLiveObjects` (with two crashes; `RecoveryIdempotent` already at the first unclean start) | unlink a (nothing held); crash before its phase 3; the start lists a's row (no longer dirty); crash before the probe; the next start probes nothing |
| `known_bugs/lifetime_destroy_with_open_files` | found by this model (formerly a finding), fixed in step 12.4b: DESTROY with a file still open (SIGTERM, a lazy unmount) and a clean shutdown (no writable open left: one keeps its row durably dirty); the start swept unnamed rows only after an unclean shutdown, so the row of an unlinked file open only for reading (nlink 0 since its phase 3), or of an `O_TMPFILE` file reopened read-only after its writable descriptor closed, stayed. Fixed by sweeping at every start, through the partial index `inodes_unlinked` (`nlink = 0`) | `UnnamedRowsSwept` (with `DestroyWithOpens`) | look a up; open it read-only; unlink it (phase 3 keeps the row, nlink 0); DESTROY; the start (clean, no sweep) |

### Findings of the lifetime model

The model found three gaps in the code (step 12.4), each first kept as a
configuration in `findings/` whose test expected the violation. Step 12.4b
fixed all three and moved them into the real model; they are the last
three known-bug variants above (`lifetime_stub_nodeid_reused`,
`lifetime_crash_before_settle`, `lifetime_destroy_with_open_files`).

Step 12.6 (recovery idempotence) found a fourth, kept as
`findings/lifetime_crash_during_probe` until step 12.6b fixed it: the
start's probe of the recovered rows worked from a list in memory.
`StartRun` read the dirty set, `RecoverDirty` emptied it and `StartRun`'s
kSync commit made that durable before `backing::Startup` probed the listed
rows, so a crash during the probe lost the rows not yet probed, and the
next start (clean flag unset, dirty set empty) probed nothing: the row of
an object a cut removal freed stayed (identity stayed safe: its handle is
stale). The model takes the probe as its own steps (`Restart` lists the
rows, `ProbeRow(i)` probes one, `ProbesDone` ends the probe, `Crash` may
come between, `SyncPoint` clears the dirty row) and checks
`RecoveryIdempotent` (every row still to be probed is still dirty) in every
configuration; `MC_lifetime_recovery.cfg` has the two crashes that reach a
crash during the probe. The fix: `RecoverDirty` keeps the dirty set, and
only a sync point (syncfs, then `ClearDirty`) removes the rows (a first
version cleared them after the probe, with no syncfs since the crashed
run's backing syscalls: `known_bugs/recovery_clears_dirty`).
`RowsNameLiveObjects` assumes a probe never fails: one that does (an I/O
error) leaves its row, dirty, until an access finds it gone. `known_bugs/lifetime_probe_list_in_memory` puts
the old start back (`Restart <- RestartForgetsProbeList`). `findings/` is
empty again.

### Trace validation of nodeids

`LifetimeTrace.tla` validates one nodeid's trace against `lifetime.tla`, as
`RevalTrace.tla` does a file's against `reval.tla` (same validator, same
depth rule), through the same per-nodeid operators the model's actions
apply (`AfterLookup`, `AfterRelease`, ...), so a `Bug*` constant changes
both. The recorder writes nodeids' traces only when made with `lifetimes`
(the forged-request harness's `StartTrace()` does; the guests' recorder
does not), on lines `DCFS-LIFE <trace> <nodeid> <json>`. A trace begins at
the entry reply dcfs counts first (with what dcfs kept before it), or at the
`CREATE` or `TMPFILE` that made the nodeid; one first seen with lookups
counted is not traced, and neither is a stub's.

| Line | From | Model step (`LifetimeTrace.tla`) |
|---|---|---|
| `lookup` (`via`: `lookup`, `dot`, `link`) | `LifetimeChanged` `kLookup`: an entry reply; `via` from the request it answered (a `LOOKUP` of `.` or `..`, a `LINK`, or by a name) | `T_LifeLookup` (`Lookup`: the object must have a name; its row found or made), `T_LifeLookupDot` (the row must exist), `T_LifeLink` (`Link`) |
| `create` (`w`), `tmpfile` | `kCreated`, `kTmpfile` | `T_LifeCreate`, `T_LifeTmpfile` (a fresh nodeid) |
| `open` (`w`), `release` (`w`) | `kOpened` (a successful `Open`), `kReleased` (the end of `Release`, after a retirement) | `T_LifeOpen`, `T_LifeRelease` (the held descriptor's cap free) |
| `removed` (`held`) | `kRemoved`: phase 3 of `RemoveChild`, or of a rename over the object, ended | `T_LifeRemoved`: `Remove` then `Settle`; `held` must be whether dcfs counted a lookup; whether a name is left is free |
| `forget` (`n`, `batch`) | `kForgot`, `kForgotInBatch` (after the batch's reconciliation) | `T_LifeForget`: `Forget` or a `ForgetMulti` entry; the kernel must hold n lookups |
| `destroy` | `Destroyed` | `T_LifeDestroy` |
| `crash`, `restart`; `start` | `RunStarting` (by the clean-shutdown flag); `RunStarted` (after the sweep) | `T_LifeCrash` (whether the object has a name is free again: a removal the crash cut leaves no line), `T_LifeRestart`; `T_LifeStart` (the sweep, at every start) |
| `probe` (`gone`) | `LifetimeChanged` `kProbed`: `backing::Startup`'s probe of a recovered row, after an unclean shutdown | `T_LifeProbe`: the row goes iff the object has no name (after a crash: is freed) |

Each line carries what dcfs kept after the step (`st`: `lk`, `rec`, `wr`,
`refs` from `DirCacheFS::LifetimeOf`, and `row` and `nl0` from the
database; the run's lines only the row), which the model's state must
equal. Why the two events were added: nothing else observes a lookup count
(no event carried a nodeid's count, and `FORGET`/`FORGET_MULTI` had no event
at all) or a retirement (a row's deletion shows as `InodeForgotten`, but the
removed record, the `written_` entry and its held descriptor are
`DirCacheFS`'s memory). What stays model-only: the kernel's own count and
the object it holds the nodeid for (a trace's `k` is rebuilt from the lookup
lines, so a `FORGET` of more than dcfs counted is rejected), whether the
object still has a name on the backing filesystem, the held descriptor's
cap, the other nodeids, and every request's reply (`Resolve`: no event says
what a request on a nodeid was answered with).

The traces: `//dcfs:dir_cache_fs_trace_test` validates every recording
test's nodeids (the run of 2026-10-07: 32 nodeids' traces, all valid),
among them the scenarios written for this model:
`NonFinalForgetKeepsTheHeldDescriptor`,
`RemovedFileIsServedUntilItsLastForget`,
`ForgetMultiTakesOffEachEntrysCount`,
`RenameOverAnOpenFileRetiresItAtTheLastRelease`,
`DestroyLetsGoOfEveryNodeid` and `TmpfileRowGoesAtTheStartAfterACrash`.
Every `T_Life*` action is taken (`T_LifeRestart` by
`DestroyLetsGoOfEveryNodeid`, which runs `FinishRun` and `StartRun` after
the DESTROY: a clean shutdown, no writable open left). `formal/trace_tests/life_*.log` keep four
of them, and the `//formal:trace_life_*_test` targets check each against the
real model (valid) and against its known-bug variant as the model
(`--life-cfg`, `trace_tests/life_<bug>.cfg`), which must reject it:

| Test | Log, model | Expected |
|---|---|---|
| `trace_life_nonfinal_forget_test` | two lookups, a writable open and release (held), `FORGET` 1, `FORGET` 1; real model | valid |
| `trace_life_nonfinal_forget_drops_held_test` | the same, `BugNonFinalForgetDropsHeld` | rejected at the first `FORGET` (the code still holds the descriptor) |
| `trace_life_removed_test`, `..._drops_rec_test` | two lookups, an unlink (a removed record), `FORGET` 1, `FORGET` 1; real model, `BugNonFinalForgetDropsRec` | valid; rejected at the first `FORGET` |
| `trace_life_forget_multi_test`, `..._counted_as_one_test` | two lookups, an unlink, a `FORGET_MULTI` entry of 2; real model, `BugForgetMultiCountsOne` | valid; rejected at the batch entry |
| `trace_life_tmpfile_crash_test`, `..._no_sweep_test` | `TMPFILE`, crash, start; real model, `BugNoUnnamedSweep` with `BugNoRecoveredProbe` (before step 23.7: neither) | valid; rejected at the start |

For example, against `BugNonFinalForgetDropsHeld`:

```
trace_validate.sh: rejected: life/t@2: the model explains 4 of 6 events; the first it cannot (event 5):
  {"i":6,"c":"LifetimeChanged","ev":"forget","n":1,"batch":false,"st":{"lk":1,"rec":false,"wr":"held","refs":0,"row":true,"nl0":false}}
```

The gaps: the guest recorder writes no nodeids' traces yet, so the real
kernel's `FORGET` counts are not validated (only the harness's forged
ones; `removed_test` checks in a guest that no `FORGET` exceeds dcfs's
count). A new daemon process would also need to know which nodeids' traces
the killed one had begun, as it does for directories.

## The identity model

`ident.tla` is a fourth, separate model, of identity: what a nodeid and its
FUSE generation stand for, how dcfs mints them, what a request and an NFS
client's handle resolve to, and what is left of that after the backing
filesystem recycles an inode number, after a daemon crash, a power loss or
a cache wipe, and after changes behind dcfs's back. The kernel puts
`(nodeid, generation)` in an NFS file handle (`fuse_encode_fh`); decoding
one (`fuse_get_dentry`) uses the kernel's own inode if its generation
matches, and otherwise sends `LOOKUP(nodeid, ".")` and returns `ESTALE`
unless the reply carries the handle's generation (an `ENOENT` reply becomes
`ESTALE` too). An entry reply with another generation than the inode the
kernel holds for that nodeid makes the kernel mark that inode bad
(`fuse_iget`): its users get `EIO`, not `ESTALE`. Requests on an inode
carry only the nodeid, so the generation protects only the decode.

The invariant, from `docs/design.md` ("Safe NFS export", "Identity
model"): a nodeid and generation never stand for two objects; a nodeid the
kernel holds, or a handle it accepts, resolves to the object it was handed
out for or to `ESTALE`, never to another and never to `EIO`; a handle of
an object that still exists is served, one of an object that is gone gets
`ESTALE`; a recycled inode number is caught at every entry point that
resolves identity.

The model has both identities, by a constant:

- **Today** (`Target = FALSE`): a nodeid is a row id (`AUTOINCREMENT`),
  its generation a random draw (`cache::UpsertInode`); the row records the
  object's inode number, generation (`FS_IOC_GETVERSION`, 0 when it cannot
  be read), handle and birth time, read by one probe through one `O_PATH`
  descriptor (`ProbeObject`). A request that reaches the backing
  filesystem reopens the handle (`open_by_handle_at`) and compares the
  result with the row (`VerifyBackingIdentity`): `ESTALE`, and the row
  goes, if the handle is stale or reaches another object. A probe of a
  name invalidates the rows of the old objects of its inode number
  (`UpsertInode`). `LOOKUP(nodeid, ".")` is answered from the row. After an
  unclean shutdown the start probes the dirty set's rows by handle
  (`ProbeRecoveredRows`).
- **Phase 14's target** (`Target = TRUE`, `docs/plan/phases/14-*.md`): the
  nodeid is the backing inode number and the generation the one in the
  backing handle; rows are keyed by inode number; a request for a nodeid
  without a row opens the inode by number (any generation), and
  `LOOKUP(nodeid, ".")` replies with the generation it finds, which the
  kernel compares.

How it relates to the others: it shares no module and no state.
`lifetime.tla` owns the kernel's lookup counts, the open files and the
held descriptor, and when rows and removed records go; it takes a row's
handle to reach its object while the object is allocated. `ident.tla`
takes that handle apart: what dcfs can see of an object (`Evidence`), how
the decode and the identity check decide, what a power loss rolls back
and what a wipe loses, and the NFS client's handles, which outlive the
mount and the daemon. It keeps only what it needs of the lifetime: a
kernel inode per nodeid (made by an entry reply, gone at eviction or
with the mount) and a removed record from an unlink until the eviction.
`dcfs.tla` is where a cached dentry is shown to name the object a probe
would find; here every lookup probes.

```sh
bazel test //formal:ident_test //formal:ident_oob_test //formal:ident_power_test \
  //formal:ident_btime_test //formal:ident_stubs_test //formal:ident_target_test
bazel test //formal:known_bug_ident_skip_identity_statx_test   # and the other known_bug_ident_*
bazel test //formal:limitation_ident_wipe_test                 # and the other limitation_ident_*
```

By hand, as above, with `MCident` (or a variant module) instead of `MC`:
`-config MC_ident.cfg MCident`.

### What is in it

The backing directory: names `Names`, objects `Objs`, each with a fixed
inode number (`InoOf`; two objects with one number are a recycling: `o3`
gets `o1`'s once `o1` is freed) and a generation and birth time unique to
it (`GenOf`). What dcfs can see of an object besides its inode number is
`Evidence`: the generation in the handle (`handle_gen`: ext4, xfs and
btrfs), `FS_IOC_GETVERSION` (`getversion`: their regular files and
directories), the birth time (`btime`); 0 is "unknown", as in the code.

| Variable | Meaning | In dcfs |
|---|---|---|
| `bName`, `bState` | the object at each name; each object `unborn`, `alive` or `dead` (freed once nothing names it and no removed record holds it) | the backing filesystem |
| `rows[r]` | nodeid `r`'s row: the inode number, handle (inode number and generation), generation and birth time it recorded, its FUSE generation `fgen`, whether it is a stub (and its name); `obj`, the object (or a stub's name) it was made for, is for checking only | `inodes`, `stubs` |
| `nextId`, `stubHigh` | the next row id; the highest stub nodeid handed out | `AUTOINCREMENT`, `cache_state.last_stub_id` |
| `dirty`, `dur` | the dirty set; the database as of the last durable commit, which a power loss rolls back to | `dirty`; WAL with `synchronous=NORMAL`, kSync phase 1s |
| `rec[r]` | the object a removed record answers for | `removed_` |
| `kin[r]` | the kernel's inode for nodeid `r` in this mount: its generation, and the object of the reply that made it (checking only) | the kernel (model-only) |
| `nfs` | the NFS client's handles `[id, gen, obj]` (`obj` for checking) | the client (model-only) |
| `mints` | random generations drawn: each is fresh | `Context::rng` |
| `probe` | a probe by name in flight (only with `ProbeByName`, not the code) | |
| `run`, `clean`, `crashes`, `powers`, `wipes`, `badMade` | the run; the clean-shutdown flag; the bounds' counts; dcfs made the kernel mark an inode bad (history) | |

| Action | What it does | dcfs |
|---|---|---|
| `Lookup(n)` | the probe of n (one step: one descriptor); the stub of n goes; `UpsertInode`: the row that matches (inode number, generations agreeing, handle bytes, birth times agreeing), else the stale rows of that inode number go and a new row takes the next id and a fresh generation (Phase 14: the row of the inode number); the entry reply | `LookupOrPopulate`, `ResolveName`, `ProbeObject`, `UpsertInode`, `ReplyEntry` |
| `Refuse(n)` | a refused name's stub: the one it has, else the next id up from `last_stub_id` with a fresh generation | `cache::SetRefused` |
| `Create(n)` | a new object, maybe on a recycled inode number, recorded and replied | `RecordNewChild` |
| `Unlink(n)`, `Rename(n, m)` | phase 1 marks the row dirty (a durable commit unless it already was); an unlink's removed record holds the object if the kernel holds the nodeid, and phase 3 deletes the row; a rename keeps the row and nodeid | `RemoveChild`, `HoldForRemoval`, `Rename` |
| `Access(r)` | a request on a nodeid the kernel or a handle names reaches the backing filesystem and resolves to `ESTALE`: the row goes | `OpenNode`, `VerifyBackingIdentity`, `ForgetStale` (and through `OpenNode`: `ParentOf`, refreshes, opens) |
| `DotLookup(h)` | the client presents a handle the kernel has no matching inode for: `LOOKUP(id, ".")`, answered from the row (Phase 14 without a row: by inode number); the kernel makes its inode and compares generations | `Lookup` of `"."`, `EntryFor` |
| `NfsTake(r)`, `Evict(r)` | the client takes a handle of an inode the kernel holds; the kernel evicts an inode (its removed record goes) | `fuse_encode_fh`; `Forget` |
| `Sync`, `Persist` | a sync point empties the dirty set (not durable by itself); a durable commit (a phase 1 elsewhere, a WAL checkpoint) | `SyncBacking`, `ClearDirty` |
| `OobUnlink(n)`, `OobCreate(n)`, `OobRename(n, m)` | with `OutOfBand`, changes behind dcfs's back, running or not (a create may recycle an inode number, or mount a filesystem at a name) | |
| `Crash`, `PowerLoss`, `Stop`, `Wipe`, `Start` | the daemon dies (the database keeps its commits) or the power goes (it rolls back to `dur`; the backing filesystem keeps what it did); a clean stop; the database deleted while stopped; the start, which after an unclean shutdown probes the dirty rows by handle | `StartRun`, `ProbeRecoveredRows`, `FinishRun` |
| `ProbeStat(n)`, `ProbeRead` | with `ProbeByName` (not the code): the statx, then the handle and the generation in `ProbeOrder`, each read resolving the name again | |

| Property | Says |
|---|---|
| `OneHandleOneObject` | (1) no two references the kernel or the client holds with one `(nodeid, generation)` stand for different objects |
| `HeldResolvesToItsObject` | ... a nodeid the kernel holds resolves to its object or to `ESTALE` |
| `HandlesResolveToTheirObject` | ... and so does a handle the kernel accepts |
| `ServedWhileLive` | (2) a handle of an object that still exists is accepted and reaches it |
| `GoneIsStale` | ... and one of an object that is gone is not served (not for a stub whose boundary went behind dcfs's back: answered from its row until its parent is listed again) |
| `ReuseDetected` | (3) a row whose object is gone resolves to `ESTALE` at every entry point (a reopen by handle, and through it `ParentOf` and the start's probe), and no probe of a live object matches it (`UpsertInode` does not take it over) |
| `NoBadInode` | (4) dcfs never replies with another generation than the kernel's inode for that nodeid has (`fuse_iget` would mark it bad: `EIO`) |

Point (4) across a restart is (1) for the client's handles: the nodeid in a
handle is re-attached to the same object (its row survived) or answered
`ESTALE` (no row, or a row of a reissued id with a fresh generation, which
the kernel compares). Point (5), a filesystem without generations, is
`Evidence`: with only the handle's generation, or only the birth time,
everything holds (`MC_ident_power.cfg`, `MC_ident_btime.cfg`); with
neither, `limitations/ident_no_generations` shows what breaks.

Abstractions: a random generation is fresh (the design's 2^-32 per
reissued nodeid taken as zero); every request and every probe is one step
(the probe reads everything through one descriptor, which pins the inode:
no recycling can come between its reads); a power loss rolls back only the
database (`dcfs.tla`'s crash states cover the backing filesystem's side);
a cached dentry answers as a probe would (`dcfs.tla`'s
`CacheNeverWrong`); the root is not modelled; btrfs's identical handle
after its own power loss (audit F6, which the birth time catches) is not
modelled: every object has its own generation.

### Configurations

Distinct states from TLC's report (run of 2026-10-07 on russ's machine, 2
workers, other lanes running).

| Configuration | Test (tier) | Checks | States | Time |
|---|---|---|---|---|
| `MC_ident.cfg` | `ident_test` (medium) | today's identity, ext4's evidence, names a, b (o1, o2), o3 recycling o1's inode number, 3 row ids, 2 handles, 1 crash; every property | 11,351 | ~15 s |
| `MC_ident_oob.cfg` | `ident_oob_test` (medium) | as above with changes behind dcfs's back; every property | 49,833 | ~35 s |
| `MC_ident_power.cfg` | `ident_power_test` (medium) | a power loss and a crash, the handle's generation the only evidence; every property but `ServedWhileLive` | 193,781 | ~70 s |
| `MC_ident_btime.cfg` | `ident_btime_test` (medium) | the birth time the only evidence, changes behind dcfs's back; every property | 49,833 | ~25 s |
| `MC_ident_stubs.cfg` | `ident_stubs_test` (medium) | filesystems mounted at a and b, 2 stub ids, mounts and unmounts behind dcfs's back, a power loss; every property but `ServedWhileLive` | 217,366 | ~70 s |
| `MC_ident_target.cfg` | `ident_target_test` (medium) | Phase 14's identity, a crash, a power loss and a cache wipe; every property | 145,136 | ~60 s |

Coverage (`-coverage 1`): every action fires where its constants allow it
(`Refuse` needs boundaries, `PowerLoss` and `Persist` a power loss, `Wipe`
a wipe, the `Oob*` actions `OutOfBand`, `ProbeStat` and `ProbeRead`
`ProbeByName`); `Access` fires only where a row can outlive its object
(out-of-band changes, or Phase 14 after a power loss), which under
exclusive access today it cannot. `DotLookup` adds no state the lookups
did not reach already (it makes the same kernel inode).

### Known bugs and limitations

Each is a test that passes only if TLC reports the expected violation (small
tier, a few seconds each).

| Variant | Bug | Expected | The counterexample |
|---|---|---|---|
| `known_bugs/ident_skip_identity_statx` | `OpenNode` without `VerifyBackingIdentity`, on a filesystem whose handles carry no generation | `HeldResolvesToItsObject` | look a up (o1); behind dcfs's back, unlink a and create o3 on o1's inode number: the nodeid the kernel holds for o1 resolves to o3 |
| `known_bugs/ident_probe_by_name_handle_first` | a probe reading by name (statx, handle, generation), not through one descriptor; no birth time | `HeldResolvesToItsObject` | the statx of a reads o1; a is replaced by o3 on o1's inode number; the handle and generation read o3's: the row made for o1 matches o3 |
| `known_bugs/ident_probe_by_name_gen_first` | the same with the generation read first | `HeldResolvesToItsObject` | the same behavior |
| `known_bugs/ident_generation_from_counter` | a row's generation following from its id (a counter) | `OneHandleOneObject` | look a up (row 1, generation 1); the client takes its handle; a power loss loses the row; look b up: row 1, generation 1, for o2 |
| `known_bugs/ident_stub_generation_reused` | a stub's generation not drawn afresh | `OneHandleOneObject` | refuse a (stub 11); the client takes its handle; a power loss rolls back the stub and `last_stub_id`; refuse b: stub 11 with the same generation |
| `known_bugs/ident_rowid_from_max` | row ids as `MAX(id) + 1`, not `AUTOINCREMENT` (the rows' half of step 12.4b's stub fix) | `NoBadInode` | look a up (row 1); unlink a; look b up: row 1 again with another generation while the kernel holds row 1's inode |
| `limitations/ident_wipe` | today: a cache wipe | `ServedWhileLive` | look a up; the client takes its handle; stop; wipe; start: `ESTALE` for a live object (README: handles do not survive deleting the database) |
| `limitations/ident_power_loss` | today: a power loss | `ServedWhileLive` | look a up (its row not yet durable); the client takes its handle; power loss; start: `ESTALE` for a live object (README: handles of objects first recorded since the last durable commit) |
| `limitations/ident_no_generations` | no generation in the handle, no `FS_IOC_GETVERSION`, no birth time | `ReuseDetected` | look a up; unlink a; the kernel evicts it; create o3 on o1's inode number; power loss (the deletion of a's row is lost, the dirty mark is not); the start's probe reaches o3 and keeps the row for it |
| `limitations/ident_target_out_of_band` | Phase 14 with a recycling behind dcfs's back | `NoBadInode` | look a up (inode 1, o1's generation); a unlinked behind dcfs's back; create b: o3 gets inode 1, and the reply's generation makes the kernel mark the old inode bad (`EIO`). Today's identity gives o3 a new nodeid (`MC_ident_oob.cfg`) |
| `limitations/ident_target_uncached_nodeid` | Phase 14 with a recycling behind dcfs's back, a held nodeid whose row went | `HeldResolvesToItsObject` | look a up; a replaced behind dcfs's back by o3 on the same inode number; a request finds the row stale (`ESTALE`, the row goes); the next request opens inode 1 by number and reaches o3. Only `LOOKUP(".")` has the kernel compare generations: Phase 14 must compare the generation it handed out in this mount (or answer `ESTALE`) for every other request on a nodeid without a row |

### The order of the handle and the generation (step 26.3's question)

`docs/design.md`'s population policy listed the generation before
`name_to_handle_at`; the code (`ProbeObject`, `ParentOf`, `Probe`) reads
the handle first. Neither order matters, because both reads, and the
statx before them, go through one `O_PATH` descriptor opened on the name:
`name_to_handle_at(fd, "", AT_EMPTY_PATH)` encodes the inode the
descriptor's dentry pins (`fs/fhandle.c`, `do_sys_name_to_handle` ->
`exportfs_encode_fh`), and `FS_IOC_GETVERSION` reads `i_generation` of
the same inode through a reopen of `/proc/self/fd/N`, which resolves to
the descriptor's own path, not to the name. While the descriptor is open
the inode cannot be evicted, so its number cannot be freed and handed to
another object (ext4 frees an unlinked inode's number in
`ext4_evict_inode`; xfs and btrfs likewise at the last reference). The
only thing that changes `i_generation` of a live inode is ext4's
`EXT4_IOC_SETVERSION`, which needs the owner or `CAP_FOWNER` and is
refused with `metadata_csum` (mkfs's default); between the two reads,
either order is then still fail-safe: the handle's generation and the one
recorded disagree, and the decode (`ext4_nfs_get_inode` compares the
handle's generation) or `VerifyBackingIdentity` answers `ESTALE`. The
known-bug variants above show what the descriptor buys: with reads by
name, a recycling between the statx and both identity reads goes
unnoticed in either order (only the birth time, from the statx, would
catch it); one between the two identity reads is caught in either order.
The design doc now gives the code's order and this reason.

### Trace validation of identities

`IdentTrace.tla` validates one nodeid's identity trace against
`ident.tla`, as `LifetimeTrace.tla` does its lifetime (same validator,
same depth rule). The recorder writes these traces only when made with
`identities` (the forged-request harness's `StartTrace()` does; the
guests' recorder does not), on lines `DCFS-IDENT <trace> <nodeid> <json>`.
A trace begins at the entry reply dcfs counts first; a stub's nodeid is not
traced.

| Line | From | Model step (`IdentTrace.tla`) |
|---|---|---|
| `reply` (`via`, `fgen`) | `LifetimeChanged` `kLookup`, `kCreated`, `kTmpfile`: an entry reply; `fgen`, the generation it carried, is the row's (read from the database: nothing writes it between `EntryFor` and the event) | `T_IdReply`: the row exists and never went in this trace (no nodeid is handed out twice: today's `AUTOINCREMENT` identity; under Phase 14, where the nodeid is the inode number, a nodeid comes back after its row went and this must be relaxed), and every reply carries the same generation (`NoBadInode`) |
| `forget` | `kForgot`, `kForgotInBatch` | `T_IdForget` |
| `resolve` (`outcome`; `ino`, `gen`, `bt`) | `IdentityResolved`, the event added for this model: `OpenNode`'s decision (`served`, `stale_handle`, `mismatch`), and whether the inode number, generation and birth time of the object reached are the row's (`same`), another (`other`), or `unknown` (0 on either side) | `T_IdResolve`: `served` exactly when `ident.tla`'s identity check (`SameObject`, or anything with `BugSkipVerify`) says the reached object is the row's; a stale or mismatched reopen makes the row go next |
| `gone` | `InodeForgotten` | `T_IdGone` |
| `destroy`, `crash`, `restart`, `start` | `Destroyed`, `RunStarting`, `RunStarted` | `T_IdRun`: the kernel holds no inode any more; the row stays |

Every line carries whether the nodeid has a row (`st.row`) and, at a
reply or `FORGET`, dcfs's lookup count (`lk`), which the model's state must
equal. Why the event was added: nothing else observed a reopen's outcome.
A stale or mismatched reopen deletes the row (`InodeForgotten`), but that
event does not say why, and a served reopen had no event at all. The rest
maps onto existing events. What stays model-only: the backing objects and
their recycling (a stale handle's object freed or recycled is free in the
trace), the generations themselves (the trace sees only whether two are
equal), the NFS client's handles and the kernel's generation comparison,
power losses and wipes (no harness test makes one).

A scenario whose invalidation of a row behind the other models' backs
would end their traces (a step neither `dcfs.tla` nor `lifetime.tla` nor
`reval.tla` has) records only identity traces
(`StartTrace(/*identities_only=*/true)`: the recorder made without
`directories`, `files` and `lifetimes`). The scenarios written for this
model are `IdentityCheckRefusesAnotherObjectBehindTheHandle` (the row's
recorded generation made not the object's, as a recycling behind dcfs's
back looks where the handle carries no generation: the reopen is
`mismatch`, the row goes, the nodeid's `LOOKUP(".")` gets `ESTALE`, the
name a new nodeid and generation) and
`OutOfBandReplacementGetsEstaleFromItsHandle` (the file unlinked and
created again behind dcfs's back: `stale_handle`). `formal/trace_tests/`
keeps them as `ident_mismatch.log` and `ident_stale_handle.log`
(`//formal:trace_ident_stale_handle_test`: valid), and
`//formal:trace_ident_mismatch_test` checks it against the real model
(valid) and `//formal:trace_ident_mismatch_skip_identity_statx_test`
against `known_bugs/ident_skip_identity_statx` as the model
(`--ident-cfg`, `trace_tests/ident_skip_identity_statx.cfg`), which must
reject it at the `mismatch` reopen:

```
trace_validate.sh: rejected: ident/t@2: the model explains 2 of 5 events; the first it cannot (event 3):
  {"i":4,"c":"IdentityResolved","ev":"resolve","outcome":"mismatch","ino":"same","gen":"other","bt":"same","st":{"row":true}}
```

The gaps: the guest recorder writes no identity traces, so the real
kernel's `LOOKUP(".")` reconnections and its NFS handles are checked only
by `handles_test` and `nfs_test`, not against the model; no traced run has
a power loss, a wipe or a recycling the handle does not show (the harness
fakes one by changing the row's recorded generation).
