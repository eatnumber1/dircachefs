\* SPDX-License-Identifier: MIT
---------------- MODULE durability_sync_point_skips_checkpoint ----------------
(***************************************************************************)
(* Known bug: step 25.28's periodic checkpoint. It ran at a request's      *)
(* start only when no sync point was due (nothing dirty), on a clock of    *)
(* its own that every sync point reset without checkpointing. So under     *)
(* writes, with a dirty set at every sync point, it never ran: the fills   *)
(* and ClearDirty commits between two mutations stayed losable for as long *)
(* as the writes went on (the review of 25.28, which 25.28b replaced).     *)
(*                                                                         *)
(* Re-introduced by BugSyncPointSkipsCheckpoint. Expected: AtRequestStart  *)
(* is violated: a mutation (phase 1, then phase 3's normal commit), an     *)
(* interval's ticks, and a request starts whose sync point clears the      *)
(* dirty set but leaves phase 3's commit losable.                          *)
(***************************************************************************)
EXTENDS durability
=============================================================================
