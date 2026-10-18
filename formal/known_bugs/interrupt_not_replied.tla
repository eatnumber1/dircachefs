\* SPDX-License-Identifier: MIT
----------------------- MODULE interrupt_not_replied ------------------------
(***************************************************************************)
(* Not a historical bug (step 22.5): an interruption point that stops the  *)
(* interrupted request, which then never replies (its EINTR lost on the    *)
(* way to fuse_reply_err). The kernel keeps waiting for the reply, even    *)
(* for a SIGKILLed caller (request_wait_answer in fs/fuse/dev.c), so the   *)
(* caller hangs for good.                                                  *)
(*                                                                         *)
(* Put in by overriding InterruptReplies with InterruptNeverReplies from   *)
(* the configuration, and checked for InterruptedReplies alone, under      *)
(* FairSpec and with no VIEW. Expected: the temporal property is violated: *)
(* a request reaches an interruption point after its interrupt arrived,    *)
(* and no step of it is enabled from then on.                              *)
(***************************************************************************)
EXTENDS MC

\* An interruption point's EINTR that never reaches the kernel.
InterruptNeverReplies == FALSE
=============================================================================
