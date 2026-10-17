\* SPDX-License-Identifier: MIT
-------------------- MODULE ident_net_unconfirmed_estale --------------------
(***************************************************************************)
(* Step 14.10's review (H1): a row forgotten on an ESTALE the parent check *)
(* did not confirm. Today's StaleOrUnreadable (backing.cc) takes a        *)
(* parent's ESTALE as "that directory is gone"; on an NFS client a        *)
(* parent's open_by_handle_at can succeed from the client's inode cache    *)
(* while the identity statx after it gets a transient ESTALE (the export  *)
(* not loaded yet), so at depth two or more an object that exists is      *)
(* forgotten. 14.10b fixes it: a parent's ESTALE counts as gone only if   *)
(* the parent's own row was confirmed gone.                                *)
(*                                                                         *)
(* The model's directory has one level, so the parent's half is abstracted *)
(* into Confirmed(r): the check answers only while the server does.       *)
(* BugUnconfirmedEstale drops that condition. Expected: ServedWhileLive is *)
(* violated: look a up (o1, row 1); the client takes its handle; the      *)
(* server goes out answering ESTALE (TransientEstale); a request on row 1 *)
(* forgets the row; the server comes back: the client's handle of the     *)
(* live o1 is ESTALE.                                                      *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
