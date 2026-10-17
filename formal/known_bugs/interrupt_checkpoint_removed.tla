\* SPDX-License-Identifier: MIT
-------------------- MODULE interrupt_checkpoint_removed --------------------
(***************************************************************************)
(* Not a historical bug (step 22.5): the checkpoint before a create's     *)
(* backing syscall removed (the Checkpoint call in DirCacheFS::CreateChild *)
(* deleted). The create is still sound (nothing it records is wrong), but *)
(* an interrupt that arrives before its syscall is not acted on until the *)
(* request is done.                                                       *)
(*                                                                         *)
(* Put in by overriding InterruptPcs with InterruptPcsWithoutCreate from   *)
(* the configuration. Expected: InterruptBound is violated: a create's     *)
(* phase 1; its interrupt arrives; with no checkpoint to stop it, it takes *)
(* its syscall, the probe of its new name and the refresh of D's          *)
(* attributes: 3 backing syscalls after the arrival, one more than the    *)
(* bound's 2.                                                             *)
(***************************************************************************)
EXTENDS MC

\* dcfs.tla's InterruptPcs without the create's syscall (C_sys).
InterruptPcsWithoutCreate ==
    {"RN_probe", "PD_read", "PD_commit", "U_sys", "R_sys", "A_sys"}
=============================================================================
