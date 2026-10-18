\* SPDX-License-Identifier: MIT
------------------------- MODULE quiet_pc_counted --------------------------
(***************************************************************************)
(* Not a bug of the code (step 22.5): a premise of QuietStepsUncounted.    *)
(* QuietPcs, the pcs at which CanArriveWhereItMatters lets no interrupt    *)
(* arrive, with a pc whose step is a backing syscall added (C_stat, a      *)
(* create's refresh of D after its phase 3). Put in by overriding QuietPcs *)
(* with QuietPcsWithCreateStat from the configuration. Expected: the       *)
(* action property is violated: an interrupted create's C_stat step        *)
(* counts a syscall.                                                       *)
(***************************************************************************)
EXTENDS MC

\* dcfs.tla's QuietPcs and C_stat. (Written out: an override cannot refer
\* to what it overrides.)
QuietPcsWithCreateStat ==
    {"LK", "RN_commit", "RD", "RDP_fill", "GA_fill", "C_rec", "C_fill",
     "C_fail", "U1", "U3", "U_fill", "U_fail", "R0", "R1", "R3", "R_fill",
     "R_fail", "R_fail2", "A3", "A_fill", "S2", "W_reply", "FR_fill",
     "FG_fill", "FG_rfill", "FS_end", "FS_fill", "FC_rec", "FC_mark",
     "FC_fail", "FL_commit", "C_stat"}
=============================================================================
