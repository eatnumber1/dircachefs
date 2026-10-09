----------------------- MODULE atime_open_not_dirty -----------------------
(***************************************************************************)
(* Not the code (step 23.8): a cold read-only open does not mark the       *)
(* file's row dirty (DirCacheFS::OpenInode without cache::MarkAtimeDirty). *)
(*                                                                         *)
(* Put in by OpenMark <- OpenMarkNotDirty. Expected: FileExact is          *)
(* violated: F's attributes recorded (valid, not dirty), F opened, a read  *)
(* moves its access time, the daemon dies before any held fill: the        *)
(* restart serves the access time from before the read.                    *)
(***************************************************************************)
EXTENDS MC

OpenMarkNotDirty ==
    /\ UNCHANGED <<dbCur, dbOpts>>
    /\ fm' = [fm EXCEPT !.opens = @ + 1, !.held = @ + 1]
=============================================================================
