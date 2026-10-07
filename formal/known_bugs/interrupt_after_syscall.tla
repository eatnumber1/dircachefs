---------------------- MODULE interrupt_after_syscall -----------------------
(***************************************************************************)
(* Not a historical bug (Phase 22): a mutation interruptible after its     *)
(* backing syscall, cancelled by putting its resolved name back. The       *)
(* backing change exists by then; dcfs has no checkpoint between the       *)
(* syscall and phase 3, and replies success.                               *)
(*                                                                         *)
(* Put in by BugInterruptAfterSyscall. Expected: CacheNeverWrong is        *)
(* violated: a rename of a over b resolves a; phase 1; renameat2;          *)
(* interrupted before phase 3, it puts a back, which the backing           *)
(* filesystem no longer has.                                               *)
(***************************************************************************)
EXTENDS MC
=============================================================================
