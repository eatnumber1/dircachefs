\* SPDX-License-Identifier: MIT
---------------------- MODULE durability_never_checkpoint ----------------------
(***************************************************************************)
(* Known bug: the code before step 25.28. No sync point checkpointed the   *)
(* cache's WAL; a normal commit (a fill, a phase 3, a sync point's         *)
(* ClearDirty) became durable only at the next kSync commit (a mutation's  *)
(* phase 1) or at SQLite's autocheckpoint, every 1000 pages. On a          *)
(* read-mostly server neither comes for a long time, and a power loss took *)
(* away hours of fills, which recovery then read again from the backing    *)
(* disks.                                                                  *)
(*                                                                         *)
(* Re-introduced by BugNeverCheckpoint. Expected: AtRequestStart is        *)
(* violated: a fill, an interval's ticks, and a request starts with the    *)
(* fill still losable.                                                     *)
(***************************************************************************)
EXTENDS durability
=============================================================================
