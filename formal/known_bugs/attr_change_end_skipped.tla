--------------------- MODULE attr_change_end_skipped ---------------------
(***************************************************************************)
(* An attribute change that never Ends (step 12.11; 8.2's mutation         *)
(* survivors: a deleted Mutation::End after copy_file_range, fallocate or  *)
(* setattr). Put in by overriding AttrChangeEnd with dcfs.tla's            *)
(* AttrChangeEndSkipped from the configuration: the attribute change's    *)
(* syscall is followed by its refresh with the fill guard still raised.   *)
(*                                                                         *)
(* Expected: GuardsBalanced is violated: an attribute change of D runs     *)
(* phase 1 (in flight on D) and its syscall; at its end nothing lowers    *)
(* FillGuards::inflight, so D's guard counts a mutation no request is in. *)
(* Its refresh then cannot fill (CanFill), and no later fill of D can.     *)
(***************************************************************************)
EXTENDS MC
=============================================================================
