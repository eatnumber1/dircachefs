\* SPDX-License-Identifier: MIT
---------------- MODULE durability_sync_point_skips_checkpoint ----------------
(***************************************************************************)
(* Known bug: step 25.28's periodic checkpoint. It ran at a request's      *)
(* start only when no sync point was due (nothing dirty), on a clock of    *)
(* its own that every sync point reset without checkpointing. So under     *)
(* writes, with a dirty set at every sync point, it never ran: the fills   *)
(* and ClearDirty commits between two mutations stayed losable for as long *)
(* as the writes went on.                                                  *)
(*                                                                         *)
(* Re-introduced by BugSyncPointSkipsCheckpoint. Expected:                 *)
(* BoundAtRequestEnd is violated: a mutation (phase 1, then phase 3's      *)
(* normal commit), an interval's ticks, and a request whose sync point     *)
(* clears the dirty set ends with phase 3's commit still losable.          *)
(***************************************************************************)
EXTENDS durability
=============================================================================
