---------------------- MODULE effect_before_syscall ----------------------
(***************************************************************************)
(* An effect point too early (step 12.7): a mutation's phase 1 that        *)
(* records the outcome it expects (the names absent) instead of marking    *)
(* them unknown, so other requests see an unlink before its unlinkat has   *)
(* happened (and still see it if the unlinkat fails). Put in by overriding *)
(* MarkUnknown with dcfs.tla's MarkAbsentEarly from the configuration.     *)
(*                                                                         *)
(* Expected: EffectAtSyscall is violated: a lookup records a present; an   *)
(* unlink of a resolves it and runs phase 1, which makes a read absent     *)
(* while the backing filesystem still has it: what other requests see      *)
(* changed at a step that is not the syscall.                              *)
(***************************************************************************)
EXTENDS MC
=============================================================================
