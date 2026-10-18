\* SPDX-License-Identifier: MIT
------------------------- MODULE interrupt_ignored --------------------------
(***************************************************************************)
(* Not a historical bug (step 22.5): interruption points that never see    *)
(* the interrupt (CheckInterrupted not asking Interrupts::Interrupted, or  *)
(* asking a source that never says yes): every request runs to its end.    *)
(* Phase 22's model allowed this, since its Interrupt was a step that may  *)
(* happen, never one that must.                                            *)
(*                                                                         *)
(* Put in by overriding Sees with SeesNothing from the configuration, and  *)
(* checked for InterruptActedOn alone, under FairSpec and with no VIEW.    *)
(* Expected: the temporal property is violated: a lookup (or another       *)
(* request) reaches an interruption point after its interrupt arrived,     *)
(* goes on to its backing syscall, and replies its answer: never EINTR.    *)
(***************************************************************************)
EXTENDS MC

\* An interruption point that never looks.
SeesNothing(r) == FALSE
=============================================================================
