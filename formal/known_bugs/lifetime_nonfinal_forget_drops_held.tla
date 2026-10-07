---------------- MODULE lifetime_nonfinal_forget_drops_held -----------------
(***************************************************************************)
(* A bug the code does not have: a FORGET that is not the last (the kernel *)
(* gives back part of a nodeid's lookups, as for a READDIRPLUS entry it    *)
(* could not link) ends the file's written_ entry and closes its held      *)
(* descriptor. The destroy_test investigation (step 23.7) suspected it;    *)
(* the code reconciles only when DropLookups says the count reached 0.     *)
(* With it, a store through a mapping after that FORGET is never seen: the *)
(* last FORGET finds no entry and reconciles nothing.                      *)
(*                                                                         *)
(* Put in by BugNonFinalForgetDropsHeld. Expected: WrittenUntilLastForget  *)
(* is violated: two lookups of a; a writable open of it (in written_); a   *)
(* FORGET of one lookup: the entry is gone while the kernel still holds    *)
(* the nodeid.                                                             *)
(***************************************************************************)
EXTENDS MClifetime
=============================================================================
