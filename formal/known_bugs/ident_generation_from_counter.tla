------------------- MODULE ident_generation_from_counter -------------------
(***************************************************************************)
(* A row's FUSE generation that follows from its id (a counter, so that    *)
(* id - generation is constant) instead of a random draw                   *)
(* (cache::InsertNewRow). Under synchronous=NORMAL a power loss can roll   *)
(* back a row insert whose (nodeid, generation) an NFS client already      *)
(* holds; AUTOINCREMENT then hands the id out again, with the same         *)
(* generation (docs/design.md, "Generations").                             *)
(*                                                                         *)
(* Put in by BugGenFromId, with a power loss. Expected: OneHandleOneObject *)
(* is violated: look a up (o1: row 1, generation 1); the NFS client takes  *)
(* its handle; a power loss loses the row; after the start, look b up: o2  *)
(* gets row 1 and generation 1, the client's handle for o1.                *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
