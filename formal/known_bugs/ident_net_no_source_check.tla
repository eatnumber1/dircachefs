\* SPDX-License-Identifier: MIT
--------------------- MODULE ident_net_no_source_check ---------------------
(***************************************************************************)
(* Step 14.10: a start that does not compare the backing's identity with   *)
(* the one the database recorded (main.cc's source-identity check, and for *)
(* a backing without a UUID the name and the root's handle). A network     *)
(* backing replaced while dcfs was down by one whose objects present       *)
(* byte-identical handles (two filesystems exported under one fsid         *)
(* number, measured by nfs_backing_test; a replica) is then served from    *)
(* the old backing's rows.                                                 *)
(*                                                                         *)
(* Put in by BugNoSourceCheck, with MC_ident_net.cfg's backing.            *)
(* Expected: HandlesResolveToTheirObject is violated: look a up (o1, row   *)
(* 1); the client takes its handle; stop; the backing is swapped (o4 at a, *)
(* o1's inode number and generation); start without the check; the        *)
(* client's handle is accepted (LOOKUP(1, ".") answers row 1's generation) *)
(* and its handle decodes to o4.                                           *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
