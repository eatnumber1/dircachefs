--------------------------- MODULE interrupt_undo ---------------------------
(***************************************************************************)
(* Not a historical bug (Phase 22): an interrupt before the syscall that   *)
(* puts the resolved name back instead of leaving phase 1's unknown. The   *)
(* backing filesystem is unchanged then, but without the kernel's lock     *)
(* another mutation of the name may have begun since: its phase 1 said     *)
(* unknown, and the undo overwrites that while it is in flight.            *)
(*                                                                         *)
(* Put in by BugInterruptUndo, without the kernel's lock. Expected:        *)
(* TriState is violated: a rename of a over b, phase 1; a create of a,     *)
(* phase 1 (a unknown, the create in flight); the rename is interrupted    *)
(* before its syscall and puts a back while the create is in flight.       *)
(***************************************************************************)
EXTENDS MC
=============================================================================
