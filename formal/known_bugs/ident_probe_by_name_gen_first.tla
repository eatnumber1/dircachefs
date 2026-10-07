------------------- MODULE ident_probe_by_name_gen_first -------------------
(***************************************************************************)
(* As ident_probe_by_name_handle_first, with the generation read before    *)
(* the handle (the order docs/design.md's population policy gave before    *)
(* step 12.5). Expected: HeldResolvesToItsObject is violated, by the same  *)
(* behavior: reading by name is unsound in either order.                   *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
