----------------------- MODULE borndirty_two_commits ------------------------
(***************************************************************************)
(* Not the code (step 23.11): the create's phase 3 as two transactions,    *)
(* the row (with its attributes and dentry) and then its dirty mark        *)
(* (Phase3Split <- SplitPhase3). Expected: CrashSafe is violated: a crash  *)
(* may keep the first transaction and lose the second and the create       *)
(* itself, leaving a clean row with the attributes of an object the        *)
(* backing filesystem lost (the lost mark).                                *)
(* known_bugs/premise_born_dirty.cfg checks the same variant for           *)
(* BornDirty.                                                              *)
(***************************************************************************)
EXTENDS MC
SplitPhase3 == TRUE
=============================================================================
