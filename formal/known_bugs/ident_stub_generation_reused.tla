-------------------- MODULE ident_stub_generation_reused --------------------
(***************************************************************************)
(* A boundary stub's generation not drawn afresh (cache::SetRefused).      *)
(* last_stub_id is rolled back by a power loss like any commit since the   *)
(* last durable one, so a stub's nodeid can be handed out again, to        *)
(* another refused name; only a fresh generation then makes the old        *)
(* handle stale. (A name refused again keeps its stub, nodeid and          *)
(* generation, on purpose: the stub stands for the name.)                  *)
(*                                                                         *)
(* Put in by BugStubGenFixed, with a power loss. Expected:                 *)
(* OneHandleOneObject is violated: look a up (refused: stub 11); the NFS   *)
(* client takes its handle; a power loss loses the stub row and            *)
(* last_stub_id; after the start, look b up: stub 11 again, with the same  *)
(* generation, for b.                                                      *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
