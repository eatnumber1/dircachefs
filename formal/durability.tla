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
(* is the sync point's checkpoint (DirCacheFS::SyncNow, CheckpointCache):  *)
(* the periodic one runs at the first request once Interval ticks have     *)
(* passed since the last sync point (DirCacheFS::MaybeSyncBacking, at the  *)
(* start of every request), an FSYNC or FSYNCDIR runs one too, and each    *)
(* checkpoints the WAL whenever anything committed since the last          *)
(* checkpoint. There is no timer (docs/style.md, "No timers"): an idle     *)
(* daemon runs nothing, so the bound is at requests. AtRequestStart: when  *)
(* a request starts, every commit a power loss may lose is younger than    *)
(* one interval. LostOnlySinceSync, the reason: every commit a power loss  *)
(* may lose came after the last sync point.                                *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - Commits may happen at any moment, not only inside requests, and in   *)
(*    any number between two request starts (a request may run for many   *)
(*    intervals on a slow backing, and under coroutines several overlap).  *)
(*    That only adds behaviors.                                            *)
(*  - The clock saturates: `since` and `oldest` at Interval stand for      *)
(*    "Interval ticks or more".                                            *)
(*  - The checkpoint succeeds. One that fails is the cache disk failing    *)
(*    (an I/O error), which leaves nothing durable anyway; dcfs logs it    *)
(*    and the next sync point checkpoints again.                           *)
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
    started   \* the last step was a request's start (for AtRequestStart)

vars == <<since, oldest, dirty, started>>

Min(a, b) == IF a < b THEN a ELSE b

TypeOK ==
    /\ since \in 0..Interval
    /\ oldest \in {None} \cup 0..Interval
    /\ dirty \in BOOLEAN
    /\ started \in BOOLEAN

-----------------------------------------------------------------------------
(* Commits. *)

\* A normal commit (a fill, a phase 3, a sync point's ClearDirty): losable
\* until the next durable point, from now if nothing before it is.
NormalCommit(o) == IF o = None THEN 0 ELSE o

\* A fill or a mutation's phase 3.
Commit ==
    /\ oldest' = NormalCommit(oldest)
    /\ UNCHANGED <<since, dirty>> /\ started' = FALSE

\* A mutation's phase 1: a kSync commit, which makes it and every commit
\* before it durable, of a row it marks dirty.
Phase1 ==
    /\ oldest' = None /\ dirty' = TRUE
    /\ UNCHANGED since /\ started' = FALSE

\* SQLite's autocheckpoint.
AutoCheckpoint ==
    /\ oldest' = None
    /\ UNCHANGED <<since, dirty>> /\ started' = FALSE

-----------------------------------------------------------------------------
(* The sync point (DirCacheFS::SyncNow). Its backing half, if the dirty    *)
(* set may be non-empty: syncfs, then ClearDirty, a normal commit. Then    *)
(* the checkpoint, if anything committed since the last one: every commit  *)
(* so far is durable.                                                      *)

Checkpoints(wasDirty) ==
    /\ ~BugNeverCheckpoint
    /\ ~(BugSyncPointSkipsCheckpoint /\ wasDirty)

SyncPoint ==
    /\ since' = 0
    /\ dirty' = FALSE
    /\ oldest' = IF Checkpoints(dirty) THEN None
                 ELSE IF dirty THEN NormalCommit(oldest) ELSE oldest

\* A request starts: the periodic sync point first, once Interval ticks
\* have passed since the last (DirCacheFS::MaybeSyncBacking).
Start ==
    /\ IF since >= Interval THEN SyncPoint
       ELSE UNCHANGED <<since, oldest, dirty>>
    /\ started' = TRUE

\* An FSYNC or FSYNCDIR: a sync point inside its request.
Fsync == SyncPoint /\ started' = FALSE

-----------------------------------------------------------------------------
(* Time, and a power loss. *)

Tick ==
    /\ since' = Min(since + 1, Interval)
    /\ oldest' = IF oldest = None THEN None ELSE Min(oldest + 1, Interval)
    /\ UNCHANGED dirty /\ started' = FALSE

\* The commits since the last durable point are lost; the next process
\* starts (StartRun's kSync commit; DirCacheFS's clock starts at its
\* construction) with whatever dirty rows recovery kept.
PowerLossAndRestart ==
    /\ since' = 0 /\ oldest' = None /\ dirty' \in BOOLEAN /\ started' = FALSE

-----------------------------------------------------------------------------

Init == since = 0 /\ oldest = None /\ dirty = FALSE /\ started = FALSE

Next ==
    \/ Commit \/ Phase1 \/ AutoCheckpoint
    \/ Start \/ Fsync
    \/ Tick \/ PowerLossAndRestart

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties. *)

\* The bound: when a request starts, a power loss can lose only cache
\* commits younger than one sync interval.
AtRequestStart == started => oldest < Interval

\* Why: every commit a power loss may lose came after the last sync point.
LostOnlySinceSync == oldest # None => oldest <= since
=============================================================================
