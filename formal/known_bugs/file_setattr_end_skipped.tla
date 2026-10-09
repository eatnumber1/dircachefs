---------------------- MODULE file_setattr_end_skipped ----------------------
(***************************************************************************)
(* Not the code (step 23.11, the audit's G17): a setattr or write of F     *)
(* whose Mutation::End is skipped (FSEnd <- FSEndSkipped): F's guard stays *)
(* raised. Expected: GuardsBalanced is violated (its F clause).            *)
(***************************************************************************)
EXTENDS MC
FSEndSkipped(p) ==
    /\ At(p, "FS_end")
    /\ Syscall(p, [ps[p] EXCEPT !.pc = "FS_stat"])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>
=============================================================================
