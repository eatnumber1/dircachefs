-------------------- MODULE borndirty_recover_keeps_file --------------------
(***************************************************************************)
(* Not the code (step 23.11): recovery forgets a dirty directory's records *)
(* but not a dirty file's (RecoverF <- RecoverFKeeping). No VIEW: MC.tla's *)
(* CrashImage assumes the real recovery. Used by                           *)
(* known_bugs/premise_recovery_forgets_dirty.cfg.                          *)
(***************************************************************************)
EXTENDS MC
RecoverFKeeping(d) == [d EXCEPT !.fDirty = "no"]
=============================================================================
