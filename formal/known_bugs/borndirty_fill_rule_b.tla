----------------------- MODULE borndirty_fill_rule_b ------------------------
(***************************************************************************)
(* Not the code (step 23.11): the audit's second candidate rule for G5, a  *)
(* row a fill inserts born dirty when its parent's fill is refused         *)
(* (FillMarks <- FillMarksIfDirNotOk). Expected: CrashSafe is violated, as *)
(* in borndirty_fill_unmarked_after_crash: after the restart the parent is *)
(* no longer in flight. The rule chosen marks a row a fill inserts while   *)
(* its parent is dirty (FillMarks in dcfs.tla).                            *)
(***************************************************************************)
EXTENDS MC
=============================================================================
