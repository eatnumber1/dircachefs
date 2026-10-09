------------ MODULE borndirty_fill_unmarked_after_failed_phase3 -------------
(***************************************************************************)
(* The code today (step 23.11, the review's path B), with the kernel's     *)
(* lock and no crash: a create's phase 3 fails after its syscall           *)
(* (RecordNewChild's transaction; CreatedButNotCompleted replies EEXIST,   *)
(* and the kernel drops its negative dentry), F's name stays unknown and D *)
(* dirty; the next lookup of F's name records F's row clean and valid      *)
(* (nothing is in flight), as after a daemon crash                         *)
(* (borndirty_fill_unmarked_after_crash). Expected: CrashSafe is violated: *)
(* the create is not durable yet, and a power loss may lose it while the   *)
(* database keeps the row (FillMarks <- NoFillMark, Phase3CanFail <-       *)
(* CanFail).                                                               *)
(***************************************************************************)
EXTENDS MC
NoFillMark(newRow, dirOk) == FALSE
=============================================================================
