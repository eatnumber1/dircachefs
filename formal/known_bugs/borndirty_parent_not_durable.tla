-------------------- MODULE borndirty_parent_not_durable --------------------
(***************************************************************************)
(* Not the code (step 23.11): the create's phase 1, D's mark, not durable  *)
(* before its syscall (BugPhase1NotDurable, crash F1, for the create of    *)
(* F). Expected: CrashSafe is violated: a lookup records F's name absent;  *)
(* the create's phase 1 at normal durability, its syscall; a power loss    *)
(* may keep the create and lose phase 1, and recovery has nothing to       *)
(* forget: F is served absent.                                             *)
(***************************************************************************)
EXTENDS MC
=============================================================================
