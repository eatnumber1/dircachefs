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
(* (a kSync commit, an fsync of the WAL), and `oldest` is the age of the   *)
(* first of them, in ticks of a discrete clock.                            *)
(*                                                                         *)
(* What makes them durable besides a kSync commit (a mutation's phase 1)   *)
(* is the cache's durable point (DirCacheFS::MakeCacheDurable). The        *)
(* periodic sync point starts at the first request once Interval ticks     *)
(* have passed since the last one (DirCacheFS::MaybeSyncNow: the backing   *)
(* half), and the request ends with the durable point, after its reply and *)
(* before the next request is served (DirCacheFS::FinishSyncPoint). An     *)
(* FSYNC or FSYNCDIR is no sync point, but makes the durable point before  *)
(* its reply (Fsync). There is no timer (docs/style.md, "No timers"): an   *)
(* idle daemon runs nothing, so the bound is at requests                   *)
(* (LostWithinInterval, the same bound at every moment, does not hold:     *)
(* limitations/durability_idle.cfg).                                       *)
(* BoundAtRequestEnd: a request that starts while a commit a power loss    *)
(* may lose is an interval old ends with every commit durable.             *)
(* LostOnlySinceSync, the reason: between requests, every commit a power   *)
(* loss may lose came after the last sync point. FsyncedDurable: once an   *)
(* FSYNC has replied, a power loss loses nothing committed before it.      *)
(*                                                                         *)
(* A durable point that fails (an I/O error of the cache disk): Linux      *)
(* reports a writeback error to one fsync and returns 0 to later ones,     *)
(* while the pages may never reach the disk, and SQLite's recovery stops   *)
(* at the first missing WAL frame (README.md, "The durability model"). So  *)
(* no later durable point repairs it: `failed` stays set until the restart *)
(* and every durable point after it leaves `oldest` as it is (Durable).    *)
(* The three properties hold while no durable point has failed in the run. *)
(* Step 25.28d replaces this with a poison: a failed durable point ends    *)
(* the run with the cache discarded.                                       *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - Commits may happen at any moment, not only inside requests, and in   *)
(*    any number (a request may run for many intervals on a slow backing). *)
(*    That only adds behaviors. Requests do not overlap: dcfs serves one   *)
(*    at a time, and the durable point comes before it serves the next.    *)
(*  - The clock saturates: `since` and `oldest` at Interval stand for      *)
(*    "Interval ticks or more".                                            *)
(*  - SQLite's autocheckpoint (every 1000 pages) may make everything       *)
(*    durable at any moment: it only shortens the lag.                     *)
(*                                                                         *)
(* The Bug* constants re-introduce the code before this step, or a code    *)
(* change the properties must catch; every real configuration sets them    *)
(* FALSE.                                                                  *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    Interval,                     \* opts_.sync_interval, in ticks (>= 1)
    \* Before step 25.28: no sync point makes the cache durable (then: no
    \* sync point checkpoints its WAL); only kSync commits and the
    \* autocheckpoint make a normal commit durable.
    BugNeverCheckpoint,
    \* Step 25.28's: a sync point that clears a dirty set resets the clock
    \* without a durable point (its MaybeCheckpointWal ran only when no sync
    \* point was due, on a clock each sync point reset).
    BugSyncPointSkipsCheckpoint,
    \* Not the code: an FSYNC that replies without the cache's durable point.
    BugFsyncSkipsDurablePoint

ASSUME /\ Interval \in Nat \ {0}
       /\ BugNeverCheckpoint \in BOOLEAN
       /\ BugSyncPointSkipsCheckpoint \in BOOLEAN
       /\ BugFsyncSkipsDurablePoint \in BOOLEAN

None == -1  \* `oldest` when a power loss can lose nothing

VARIABLES
    since,    \* ticks since the last sync point (DirCacheFS::last_sync_)
    oldest,   \* ticks since the first commit a power loss may lose, or None
    dirty,    \* the dirty set may be non-empty (DirCacheFS::SyncDue)
    busy,     \* a request is being served
    due,      \* its sync point's durable point is due (durable_point_due_)
    failed,   \* a durable point failed since the start
    owed,     \* the request started with a losable commit an interval old
    ended,    \* the last step ended a request (for BoundAtRequestEnd)
    fsynced   \* an FSYNC replied OK and nothing committed since (history)

vars == <<since, oldest, dirty, busy, due, failed, owed, ended, fsynced>>

Min(a, b) == IF a < b THEN a ELSE b

TypeOK ==
    /\ since \in 0..Interval
    /\ oldest \in {None} \cup 0..Interval
    /\ \A b \in {dirty, busy, due, failed, owed, ended, fsynced} :
         b \in BOOLEAN

-----------------------------------------------------------------------------
(* Commits. *)

\* A normal commit (a fill, a phase 3, a sync point's ClearDirty): losable
\* until the next durable point, from now if nothing before it is.
NormalCommit(o) == IF o = None THEN 0 ELSE o

\* What a durable point leaves losable: nothing, unless one failed before
\* it in the run.
Durable(o) == IF failed THEN o ELSE None

\* A fill or a mutation's phase 3.
Commit ==
    /\ oldest' = NormalCommit(oldest) /\ fsynced' = FALSE
    /\ UNCHANGED <<since, dirty, busy, due, failed, owed>> /\ ended' = FALSE

\* A mutation's phase 1: a kSync commit, which makes it and every commit
\* before it durable, of a row it marks dirty.
Phase1 ==
    /\ oldest' = Durable(NormalCommit(oldest)) /\ dirty' = TRUE
    /\ fsynced' = FALSE
    /\ UNCHANGED <<since, busy, due, failed, owed>> /\ ended' = FALSE

\* SQLite's autocheckpoint.
AutoCheckpoint ==
    /\ oldest' = Durable(oldest)
    /\ UNCHANGED <<since, dirty, busy, due, failed, owed, fsynced>>
    /\ ended' = FALSE

-----------------------------------------------------------------------------
(* The sync point. SyncNow, inside the request (MaybeSyncNow): the         *)
(* backing half, if the dirty set may be non-empty (syncfs, then           *)
(* ClearDirty, a normal commit), and the durable point left due.           *)
(* FinishSyncPoint, after the reply: the durable point, if anything        *)
(* committed since the last one, makes every commit so far durable, or     *)
(* fails.                                                                  *)

DurablePointDue(wasDirty) ==
    /\ ~BugNeverCheckpoint
    /\ ~(BugSyncPointSkipsCheckpoint /\ wasDirty)

SyncNow ==
    /\ since' = 0 /\ dirty' = FALSE
    /\ oldest' = IF dirty THEN NormalCommit(oldest) ELSE oldest
    /\ fsynced' = (fsynced /\ ~dirty)
    \* (The bugs' sync point, whose request ends with no durable point.)
    /\ due' = DurablePointDue(dirty)

\* A request starts: the periodic sync point first, once Interval ticks
\* have passed since the last (DirCacheFS::MaybeSyncNow).
Start ==
    /\ ~busy /\ busy' = TRUE
    /\ owed' = (oldest >= Interval)
    /\ IF since >= Interval THEN SyncNow
       ELSE UNCHANGED <<since, oldest, dirty, due, fsynced>>
    /\ UNCHANGED failed /\ ended' = FALSE

\* An FSYNC or FSYNCDIR (step 25.28b): no sync point (the clock and the
\* dirty set stay), but the cache's durable point before its reply.
Fsync ==
    /\ busy
    /\ oldest' = IF BugFsyncSkipsDurablePoint THEN oldest ELSE Durable(oldest)
    /\ fsynced' = TRUE
    /\ UNCHANGED <<since, dirty, busy, due, failed, owed>> /\ ended' = FALSE

\* The same, with a durable point that fails: the request replies EIO.
FsyncFails ==
    /\ busy /\ failed' = TRUE /\ fsynced' = FALSE
    /\ UNCHANGED <<since, oldest, dirty, busy, due, owed>> /\ ended' = FALSE

\* The request ends: its reply, then the durable point if one is due.
Finish ==
    /\ busy /\ busy' = FALSE
    /\ oldest' = IF due THEN Durable(oldest) ELSE oldest
    /\ due' = FALSE /\ ended' = TRUE
    /\ UNCHANGED <<since, dirty, failed, owed, fsynced>>

\* The same, with a durable point that fails.
DurablePointFails ==
    /\ busy /\ due /\ busy' = FALSE
    /\ failed' = TRUE
    /\ due' = FALSE /\ ended' = TRUE
    /\ UNCHANGED <<since, oldest, dirty, owed, fsynced>>

-----------------------------------------------------------------------------
(* Time, and a power loss. *)

Tick ==
    /\ since' = Min(since + 1, Interval)
    /\ oldest' = IF oldest = None THEN None ELSE Min(oldest + 1, Interval)
    /\ UNCHANGED <<dirty, busy, due, failed, owed, fsynced>> /\ ended' = FALSE

\* The commits since the last durable point are lost; the next process
\* starts (StartRun's kSync commit; DirCacheFS's clock starts at its
\* construction) with whatever dirty rows recovery kept.
PowerLossAndRestart ==
    /\ since' = 0 /\ oldest' = None /\ dirty' \in BOOLEAN
    /\ busy' = FALSE /\ due' = FALSE /\ failed' = FALSE /\ owed' = FALSE
    /\ ended' = FALSE /\ fsynced' = FALSE

-----------------------------------------------------------------------------

Init ==
    /\ since = 0 /\ oldest = None /\ dirty = FALSE
    /\ busy = FALSE /\ due = FALSE /\ failed = FALSE /\ owed = FALSE
    /\ ended = FALSE /\ fsynced = FALSE

Next ==
    \/ Commit \/ Phase1 \/ AutoCheckpoint
    \/ Start \/ Fsync \/ FsyncFails \/ Finish \/ DurablePointFails
    \/ Tick \/ PowerLossAndRestart

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties. Each but the last two holds while no durable point has      *)
(* failed in the run (`failed`; see the module comment).                   *)

\* The bound: a request that starts while a commit a power loss may lose is
\* an interval old ends with every commit durable.
BoundAtRequestEnd == ended /\ owed /\ ~failed => oldest = None

\* Why: between requests, every commit a power loss may lose came after the
\* last sync point.
LostOnlySinceSync == ~busy /\ oldest # None /\ ~failed => oldest <= since

\* An FSYNC's durable point: once it has replied OK, and until the next
\* commit, a power loss loses nothing (the inode rows an NFS client's
\* handles name included).
FsyncedDurable == fsynced /\ ~failed => oldest = None

\* The bound without its exception: what a failed durable point costs
\* (limitations/durability_durable_point_fails.cfg expects it violated).
BoundIgnoringFailures == ended /\ owed => oldest = None

\* The bound at every moment, not only at requests: what an idle daemon
\* costs (limitations/durability_idle.cfg expects it violated: a commit,
\* then time passing with no request).
LostWithinInterval == oldest = None \/ oldest < Interval
=============================================================================
