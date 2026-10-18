\* SPDX-License-Identifier: MIT
----------------- MODULE interruption_point_removed_unlink ------------------
(***************************************************************************)
(* Not a historical bug (step 22.5): the interruption point before an      *)
(* unlink's backing syscall removed (the CheckInterrupted call in          *)
(* DirCacheFS::RemoveChild deleted). Before the bound was per request      *)
(* kind (InterruptBound, Run) this went unseen: the unlink's run, its      *)
(* syscall and its refresh, stayed within a create's 2.                    *)
(*                                                                         *)
(* Put in by overriding InterruptPcs with InterruptPcsWithoutUnlink from   *)
(* the configuration. Expected: InterruptBound is violated: an unlink      *)
(* resolves its name; phase 1; its interrupt arrives; with no interruption *)
(* point to stop it, it takes its syscall and the refresh of D's           *)
(* attributes: 2 backing syscalls after the arrival, one more than an      *)
(* unlink's Run of 1.                                                      *)
(***************************************************************************)
EXTENDS MC

\* dcfs.tla's InterruptPcs without the unlink's syscall (U_sys). (Written
\* out: an override cannot refer to what it overrides.)
InterruptPcsWithoutUnlink ==
    {"RN_probe", "PD_read", "PD_commit", "C_sys", "R_sys",
     "A_sys", "GA_stat", "FC_sys", "FS_sys", "W_sys",
     "FG_stat", "FG_rstat"}
=============================================================================
