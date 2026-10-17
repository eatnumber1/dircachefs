\* SPDX-License-Identifier: MIT
----------------------------- MODULE durability -----------------------------
(***************************************************************************)
(* The cache's durability lag (step 25.28b): how old a cache commit a      *)
(* power loss can take away. README.md ("The durability model") explains   *)
(* it; this comment says what is in it.                                    *)
(*                                                                         *)
(* dcfs.tla abstracts the cache database's durability as two kinds of      *)
(* commit (step 12.14, checked against SQLite by sqlite_durability_test):  *)
(* a normal one (synchronous=NORMAL: in the WAL, not fsynced) may be lost  *)
(* to a power loss, a kSync one may not, and neither may any commit before *)
(* a kSync one; a crash leaves the last synced commit or a later one       *)
(* (dcfs.tla's dbOpts). It has no clock, so it cannot say how long a       *)
(* normal commit stays losable. Here the same abstraction gets one: the    *)
(* commits a power loss may lose are those since the last durable point    *)
(* (a kSync commit, a checkpoint), and `oldest` is the age of the first of *)
(* them, in ticks of a discrete clock.                                     *)
(*                                                                         *)
(* What makes them durable besides a kSync commit (a mutation's phase 1)   *)
(* is the sync point's checkpoint. The periodic sync point starts at the   *)
(* first request once Interval ticks have passed since the last one        *)
(* (DirCacheFS::MaybeSyncNow, SyncNow: the backing half), an FSYNC or      *)
(* FSYNCDIR starts one too, and the request ends with the checkpoint,      *)
(* after its reply and before the next request is served                   *)
(* (DirCacheFS::FinishSyncPoint). There is no timer (docs/style.md, "No    *)
(* timers"): an idle daemon runs nothing, so the bound is at requests      *)
(* (LostWithinInterval, the same bound at every moment, does not hold:     *)
(* limitations/durability_idle.cfg).                                       *)
(* BoundAtRequestEnd: a request that starts while a commit a power loss    *)
(* may lose is an interval old ends with every commit durable, unless its  *)
(* checkpoint failed (CheckpointFails: an I/O error of the cache disk,     *)
(* logged at WARNING; the commits then wait for the next sync point).      *)
(* LostOnlySinceSync, the reason: between requests, every commit a power   *)
(* loss may lose came after the last sync point, unless its checkpoint     *)
(* failed.                                                                 *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - Commits may happen at any moment, not only inside requests, and in   *)
(*    any number (a request may run for many intervals on a slow backing). *)
(*    That only adds behaviors. Requests do not overlap: dcfs serves one   *)
(*    at a time, and the checkpoint comes before it serves the next.       *)
(*  - The clock saturates: `since` and `oldest` at Interval stand for      *)
(*    "Interval ticks or more".                                            *)
(*  - SQLite's autocheckpoint (every 1000 pages) may make everything       *)
(*    durable at any moment: it only shortens the lag.                     *)
(*                                                                         *)
(* The Bug* constants re-introduce the code before this step; every real   *)
(* configuration sets them FALSE.                                          *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    Interval,                     \* opts_.sync_interval, in ticks (>= 1)
    \* Before step 25.28: no sync point checkpoints; only kSync commits and
    \* the autocheckpoint make a normal commit durable.
    BugNeverCheckpoint,
    \* Step 25.28's: a sync point that clears a dirty set resets the clock
    \* without a checkpoint (its MaybeCheckpointWal ran only when no sync
    \* point was due, on a clock each sync point reset).
    BugSyncPointSkipsCheckpoint

ASSUME /\ Interval \in Nat \ {0}
       /\ BugNeverCheckpoint \in BOOLEAN
       /\ BugSyncPointSkipsCheckpoint \in BOOLEAN

None == -1  \* `oldest` when a power loss can lose nothing

VARIABLES
    since,    \* ticks since the last sync point (DirCacheFS::last_sync_)
    oldest,   \* ticks since the first commit a power loss may lose, or None
    dirty,    \* the dirty set may be non-empty (DirCacheFS::SyncDue)
    busy,     \* a request is being served
    due,      \* its sync point's checkpoint is due (checkpoint_due_)
    failed,   \* the last sync point's checkpoint failed
    owed,     \* the request started with a losable commit an interval old
    ended     \* the last step ended a request (for BoundAtRequestEnd)

vars == <<since, oldest, dirty, busy, due, failed, owed, ended>>

Min(a, b) == IF a < b THEN a ELSE b

TypeOK ==
    /\ since \in 0..Interval
    /\ oldest \in {None} \cup 0..Interval
    /\ \A b \in {dirty, busy, due, failed, owed, ended} : b \in BOOLEAN

-----------------------------------------------------------------------------
(* Commits. *)

\* A normal commit (a fill, a phase 3, a sync point's ClearDirty): losable
\* until the next durable point, from now if nothing before it is.
NormalCommit(o) == IF o = None THEN 0 ELSE o

\* A fill or a mutation's phase 3.
Commit ==
    /\ oldest' = NormalCommit(oldest)
    /\ UNCHANGED <<since, dirty, busy, due, failed, owed>> /\ ended' = FALSE

\* A mutation's phase 1: a kSync commit, which makes it and every commit
\* before it durable, of a row it marks dirty.
Phase1 ==
    /\ oldest' = None /\ dirty' = TRUE
    /\ UNCHANGED <<since, busy, due, failed, owed>> /\ ended' = FALSE

\* SQLite's autocheckpoint.
AutoCheckpoint ==
    /\ oldest' = None
    /\ UNCHANGED <<since, dirty, busy, due, failed, owed>> /\ ended' = FALSE

-----------------------------------------------------------------------------
(* The sync point. SyncNow, inside the request: the backing half, if the   *)
(* dirty set may be non-empty (syncfs, then ClearDirty, a normal commit),  *)
(* and the checkpoint left due. FinishSyncPoint, after the reply: the      *)
(* checkpoint, if anything committed since the last one, makes every       *)
(* commit so far durable, or fails.                                        *)

Checkpoints(wasDirty) ==
    /\ ~BugNeverCheckpoint
    /\ ~(BugSyncPointSkipsCheckpoint /\ wasDirty)

SyncNow ==
    /\ since' = 0 /\ dirty' = FALSE
    /\ oldest' = IF dirty THEN NormalCommit(oldest) ELSE oldest
    \* (The bugs' sync point, whose request ends with no checkpoint.)
    /\ due' = Checkpoints(dirty)

\* A request starts: the periodic sync point first, once Interval ticks
\* have passed since the last (DirCacheFS::MaybeSyncNow).
Start ==
    /\ ~busy /\ busy' = TRUE
    /\ owed' = (oldest >= Interval)
    /\ IF since >= Interval THEN SyncNow
       ELSE UNCHANGED <<since, oldest, dirty, due>>
    /\ UNCHANGED failed /\ ended' = FALSE

\* An FSYNC or FSYNCDIR: a sync point inside its request.
Fsync ==
    /\ busy /\ SyncNow
    /\ UNCHANGED <<busy, failed, owed>> /\ ended' = FALSE

\* The request ends: its reply, then the checkpoint if one is due.
Finish ==
    /\ busy /\ busy' = FALSE
    /\ oldest' = IF due THEN None ELSE oldest
    /\ failed' = (~due /\ failed)
    /\ due' = FALSE /\ ended' = TRUE
    /\ UNCHANGED <<since, dirty, owed>>

\* The same, with a checkpoint that fails.
CheckpointFails ==
    /\ busy /\ due /\ busy' = FALSE
    /\ failed' = TRUE
    /\ due' = FALSE /\ ended' = TRUE
    /\ UNCHANGED <<since, oldest, dirty, owed>>

-----------------------------------------------------------------------------
(* Time, and a power loss. *)

Tick ==
    /\ since' = Min(since + 1, Interval)
    /\ oldest' = IF oldest = None THEN None ELSE Min(oldest + 1, Interval)
    /\ UNCHANGED <<dirty, busy, due, failed, owed>> /\ ended' = FALSE

\* The commits since the last durable point are lost; the next process
\* starts (StartRun's kSync commit; DirCacheFS's clock starts at its
\* construction) with whatever dirty rows recovery kept.
PowerLossAndRestart ==
    /\ since' = 0 /\ oldest' = None /\ dirty' \in BOOLEAN
    /\ busy' = FALSE /\ due' = FALSE /\ failed' = FALSE /\ owed' = FALSE
    /\ ended' = FALSE

-----------------------------------------------------------------------------

Init ==
    /\ since = 0 /\ oldest = None /\ dirty = FALSE
    /\ busy = FALSE /\ due = FALSE /\ failed = FALSE /\ owed = FALSE
    /\ ended = FALSE

Next ==
    \/ Commit \/ Phase1 \/ AutoCheckpoint
    \/ Start \/ Fsync \/ Finish \/ CheckpointFails
    \/ Tick \/ PowerLossAndRestart

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties. *)

\* The bound: a request that starts while a commit a power loss may lose is
\* an interval old ends with every commit durable, unless its checkpoint
\* failed.
BoundAtRequestEnd == ended /\ owed /\ ~failed => oldest = None

\* Why: between requests, every commit a power loss may lose came after the
\* last sync point, unless that sync point's checkpoint failed.
LostOnlySinceSync == ~busy /\ oldest # None /\ ~failed => oldest <= since

\* The bound without its exception: what a failed checkpoint costs
\* (limitations/durability_checkpoint_fails.cfg expects it violated).
BoundIgnoringFailures == ended /\ owed => oldest = None

\* The bound at every moment, not only at requests: what an idle daemon
\* costs (limitations/durability_idle.cfg expects it violated: a commit,
\* then time passing with no request).
LostWithinInterval == oldest = None \/ oldest < Interval
=============================================================================
