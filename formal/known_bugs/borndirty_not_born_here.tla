---------------------- MODULE borndirty_not_born_here -----------------------
(***************************************************************************)
(* Not the code (step 23.11): phase 3 makes F durably dirty even when a    *)
(* fill had inserted its row before (BornHere <- AlwaysBornHere).          *)
(* Expected: CrashSafe is violated, without the kernel's lock: a lookup    *)
(* inserts F's row (born dirty) between the create's syscall and its phase *)
(* 3; a sync point clears that mark (the syncfs covered the create); phase *)
(* 3 marks the row, records F's attributes and takes F as durably dirty,   *)
(* but a crash may still leave the row clean; a write's phase 1 takes the  *)
(* fast path: a power loss may keep the write and lose phase 1.            *)
(***************************************************************************)
EXTENDS MC
AlwaysBornHere(r) == TRUE
=============================================================================
