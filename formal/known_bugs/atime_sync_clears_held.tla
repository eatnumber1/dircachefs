---------------------- MODULE atime_sync_clears_held ----------------------
(***************************************************************************)
(* Not the code (step 23.8): a sync point clears the dirty row of a file   *)
(* dcfs holds open (cache::ClearDirty without SyncSnapshot::open_files and *)
(* Context::open_files in its keep set). The kernel may read the file      *)
(* after the syncfs; the access time of those reads is in no record yet.   *)
(*                                                                         *)
(* Put in by SyncKeepsHeld <- SyncKeepsNotHeld. Expected: FileExact is     *)
(* violated: F's attributes recorded (valid), F opened (its row dirty), a  *)
(* sync point clears the row, a read moves F's access time, the daemon     *)
(* dies before any held fill: the restart finds the row valid and not      *)
(* dirty, and serves the access time from before the read (cache behind,   *)
(* with no power loss to explain it).                                      *)
(***************************************************************************)
EXTENDS MC

SyncKeepsNotHeld(r) == FALSE
=============================================================================
