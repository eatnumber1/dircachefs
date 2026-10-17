\* SPDX-License-Identifier: MIT
--------------------- MODULE ident_net_forget_on_error ---------------------
(***************************************************************************)
(* Step 14.10: a row forgotten on any error from the backing, not only on  *)
(* an ESTALE the parent check confirms (backing.cc's StaleOrUnreadable).   *)
(* During a server outage a request reaching the backing fails (EIO, or a  *)
(* transient ESTALE the check cannot confirm); forgetting the row then     *)
(* loses a live object's identity.                                         *)
(*                                                                         *)
(* Put in by BugForgetOnError, with a server outage. Expected:             *)
(* ServedWhileLive is violated: look a up (o1, row 1); the client takes    *)
(* its handle; the server goes down; a request on row 1 fails and the row  *)
(* goes; the server comes back; the client's handle of o1, which still     *)
(* exists, resolves to ESTALE (no row; today's identity cannot reopen a    *)
(* nodeid without one).                                                    *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
