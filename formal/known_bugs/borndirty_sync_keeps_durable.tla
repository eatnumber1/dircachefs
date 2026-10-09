-------------------- MODULE borndirty_sync_keeps_durable --------------------
(***************************************************************************)
(* Not the code (step 23.11): a sync point clears F's born-dirty mark but  *)
(* keeps F in Context::dirty.durable (ClearDirty's durable.clear()         *)
(* dropped; SyncKeepsDurable <- KeepDurable). Expected: CrashSafe is       *)
(* violated: F created (born dirty, durable, its attributes recorded); a   *)
(* sync point clears its mark; a write's phase 1 at normal durability, its *)
(* syscall: a power loss may keep the write and lose phase 1 (cache        *)
(* behind).                                                                *)
(***************************************************************************)
EXTENDS MC
KeepDurable == TRUE
=============================================================================
