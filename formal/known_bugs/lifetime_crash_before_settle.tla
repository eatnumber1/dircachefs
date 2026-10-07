-------------------- MODULE lifetime_crash_before_settle --------------------
(***************************************************************************)
(* Found by this model (formerly findings/), fixed in step 12.4b: a crash  *)
(* between an unlink's (or rmdir's, or rename's) backing syscall and its   *)
(* phase 3 left the row of an object with no name left, its nlink column   *)
(* still not 0, so the start's sweep of unnamed rows kept it: a row of a   *)
(* freed object, deleted only once something reached it by handle. The     *)
(* fix: at a start after an unclean shutdown, backing::Startup probes      *)
(* every row recovery found dirty by handle (ProbeRecoveredRows) and       *)
(* deletes those whose object is gone or has no link left, directories     *)
(* included.                                                               *)
(*                                                                         *)
(* Put in by BugNoRecoveredProbe. Expected: RowsNameLiveObjects is         *)
(* violated: unlink a (nothing held); crash; start: a's row stays, its     *)
(* object freed.                                                           *)
(***************************************************************************)
EXTENDS MClifetime
=============================================================================
