---------------------- MODULE borndirty_trusts_any_row ----------------------
(***************************************************************************)
(* Not the code (step 23.11): BeginWriting (the model's fset) takes the    *)
(* fast path for any existing row, not only a born-dirty one (FSetSync <-  *)
(* FSetSyncTrustRow). Expected: CrashSafe is violated: F exists with a     *)
(* clean row; a getattr records its attributes; a write's phase 1 at       *)
(* normal durability, its syscall: a power loss may keep the write and     *)
(* lose phase 1, and F's attributes are served from before the write       *)
(* (cache behind: the ghost fs).                                           *)
(***************************************************************************)
EXTENDS MC
FSetSyncTrustRow == ~(fm.durable \/ dbCur.fRow)
=============================================================================
