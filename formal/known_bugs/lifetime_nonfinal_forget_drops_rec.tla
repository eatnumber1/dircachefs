----------------- MODULE lifetime_nonfinal_forget_drops_rec -----------------
(***************************************************************************)
(* A bug the code does not have: a FORGET that is not the last ends the    *)
(* removed record (removed_.erase on every FORGET rather than at the       *)
(* last). The kernel still holds the nodeid of an object dcfs removed, and *)
(* its next request gets ESTALE instead of the object's attributes         *)
(* (removed_test's working directory and O_PATH descriptor).               *)
(*                                                                         *)
(* Put in by BugNonFinalForgetDropsRec. Expected: ReferencedServed is      *)
(* violated: two lookups of a; an unlink of a, which holds it (the kernel  *)
(* holds the nodeid) and whose phase 3 makes the removed record; a FORGET  *)
(* of one lookup: the record goes, nothing pins the object, and the nodeid *)
(* the kernel still holds resolves to ESTALE.                              *)
(***************************************************************************)
EXTENDS MClifetime
=============================================================================
