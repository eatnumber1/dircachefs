---------------- MODULE borndirty_fill_unmarked_after_crash -----------------
(***************************************************************************)
(* The code today (step 23.11): the same rule as borndirty_fill_unmarked,  *)
(* with the kernel's lock: a daemon crash between the create's syscall and *)
(* its phase 3, the restart (D stays dirty: its mark is kept until a sync  *)
(* point), and a lookup of F's name inserts F's row clean and valid        *)
(* (nothing is in flight now). Expected: CrashSafe is violated: the create *)
(* is still not durable, and a power loss may lose it while the database   *)
(* keeps the row. The start does not sweep it (ForgetUnnamedRows takes     *)
(* rows with nlink 0 only) nor probe it (it is not dirty).                 *)
(***************************************************************************)
EXTENDS MC
NoFillMark(newRow, dirOk) == FALSE
=============================================================================
