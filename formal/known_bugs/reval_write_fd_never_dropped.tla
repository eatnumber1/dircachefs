-------------------- MODULE reval_write_fd_never_dropped --------------------
(***************************************************************************)
(* A bug the code does not have, for the model's own sake (review of step  *)
(* 12.3): the write fd kept after the last writable open is released. The  *)
(* code drops it there (BackingFile::DropWriteFd, review L-a), so a later  *)
(* writer writes through a descriptor of its own open, not one left over   *)
(* from an earlier writer's. Without this variant                          *)
(* and WriteFdOnlyBesideWriters, a model that never dropped it passed      *)
(* every configuration (trace validation caught it; model checking did     *)
(* not).                                                                   *)
(*                                                                         *)
(* Put in by BugWriteFdNeverDropped. Expected: WriteFdOnlyBesideWriters is *)
(* violated: a read-only shared fd; open O_WRONLY (a write fd); release    *)
(* it, with the read-only open still there: the write fd stays.            *)
(***************************************************************************)
EXTENDS MCreval
=============================================================================
