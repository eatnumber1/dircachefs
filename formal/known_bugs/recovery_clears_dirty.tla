------------------------ MODULE recovery_clears_dirty ------------------------
(***************************************************************************)
(* The start takes the recovered rows out of the dirty set when its probe  *)
(* ends (step 12.6b's first version, cache::ClearDirtyRows), with no       *)
(* syncfs since the crashed run's backing syscalls. After a daemon crash   *)
(* those syscalls may still be in the backing filesystem's page cache      *)
(* only: a power loss later can lose them while the database, no longer    *)
(* dirty, keeps what the new run recorded since (a lookup's negative       *)
(* entry). The code keeps the rows until the first sync point's syncfs     *)
(* and ClearDirty.                                                         *)
(*                                                                         *)
(* Put in by ProbesDone <- ProbesDoneClearing. Expected: CrashSafe is      *)
(* violated: an unlink of a (phase 1, syscall), a daemon crash (the        *)
(* unlink not yet durable), the start, its probe clears D's dirty row, a   *)
(* lookup records a absent: a power loss now may bring a back while D is   *)
(* not dirty, and the cache would say absent.                              *)
(***************************************************************************)
EXTENDS MC
=============================================================================
