\* SPDX-License-Identifier: MIT
--------------------------- MODULE InterruptReach ---------------------------
(***************************************************************************)
(* Step 22.5: evidence that every interruption point is reached by an      *)
(* interrupted request, so that InterruptBound and InterruptActedOn are    *)
(* not vacuous there. For each pc of InterruptPcs, an invariant that no    *)
(* slot is ever at it with its interrupt arrived;                          *)
(* interrupt_reach_<pc>_test checks it on a configuration whose requests   *)
(* reach that pc (D's on MC_interrupt_liveness.cfg, F's on                 *)
(* MC_interrupt_files.cfg and MC_interrupt_borndirty.cfg) and expects its  *)
(* violation.                                                              *)
(***************************************************************************)
EXTENDS MC

NeverInterruptedAt(pc) == \A p \in Procs : ~(ps[p].intr /\ ps[p].pc = pc)

NeverInterruptedAt_RN_probe == NeverInterruptedAt("RN_probe")
NeverInterruptedAt_PD_read == NeverInterruptedAt("PD_read")
NeverInterruptedAt_PD_commit == NeverInterruptedAt("PD_commit")
NeverInterruptedAt_C_sys == NeverInterruptedAt("C_sys")
NeverInterruptedAt_U_sys == NeverInterruptedAt("U_sys")
NeverInterruptedAt_R_sys == NeverInterruptedAt("R_sys")
NeverInterruptedAt_A_sys == NeverInterruptedAt("A_sys")
NeverInterruptedAt_GA_stat == NeverInterruptedAt("GA_stat")
NeverInterruptedAt_FC_sys == NeverInterruptedAt("FC_sys")
NeverInterruptedAt_FS_sys == NeverInterruptedAt("FS_sys")
NeverInterruptedAt_W_sys == NeverInterruptedAt("W_sys")
NeverInterruptedAt_FG_stat == NeverInterruptedAt("FG_stat")
NeverInterruptedAt_FG_rstat == NeverInterruptedAt("FG_rstat")

\* Every pc above is one of InterruptPcs, and the other way round: a new
\* interruption point needs its reachability test too.
ASSUME InterruptPcs = {"RN_probe", "PD_read", "PD_commit", "C_sys", "U_sys",
                       "R_sys", "A_sys", "GA_stat", "FC_sys", "FS_sys",
                       "W_sys", "FG_stat", "FG_rstat"}
=============================================================================
