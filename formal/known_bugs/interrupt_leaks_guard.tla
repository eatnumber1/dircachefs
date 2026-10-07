----------------------- MODULE interrupt_leaks_guard ------------------------
(***************************************************************************)
(* Not a historical bug (Phase 22): an interrupted mutation that returns   *)
(* without Mutation::End, leaving its fill guard raised                    *)
(* (FillGuards::inflight): every later fill of D is refused as concurrent  *)
(* with a mutation, and every later phase-1 verification of an unlink or   *)
(* rename fails until a restart.                                           *)
(*                                                                         *)
(* Put in by BugInterruptLeaksGuard. Expected: GuardsBalanced is violated: *)
(* a create's phase 1; interrupted before its syscall, it replies without  *)
(* End.                                                                    *)
(***************************************************************************)
EXTENDS MC
=============================================================================
