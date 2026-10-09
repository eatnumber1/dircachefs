----------------------- MODULE borndirty_fill_rule_a ------------------------
(***************************************************************************)
(* Not the code (step 23.11): the audit's first candidate rule for G5, a   *)
(* child's attributes recorded only if its parent's fill is allowed too    *)
(* (ChildFilled <- ChildFilledIfDirOk; the row still unmarked). Expected:  *)
(* CrashSafe is violated, as in borndirty_fill_unmarked_after_crash: after *)
(* the restart the parent's fill is allowed. (Without a crash it keeps     *)
(* CrashSafe but not BornDirty: the row exists clean.)                     *)
(***************************************************************************)
EXTENDS MC
NoFillMark(newRow, dirOk) == FALSE
=============================================================================
