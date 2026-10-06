-------------------- MODULE tristate_f1_unguarded_fills --------------------
(***************************************************************************)
(* Known bug: tri-state audit F1 (docs/plan/audits/tristate.md). No fill  *)
(* was guarded against concurrent mutations: a fill that read the backing *)
(* filesystem before a mutation's syscall could commit after the          *)
(* mutation's phase 3, recording the old state as current forever. Fixed  *)
(* by the fill guards (cache::BeginFill, CanFill; FillGuards in           *)
(* dcfs/context.h) and the epoch compare-and-set in PopulateDirectory.    *)
(*                                                                         *)
(* Re-introduced by BugUnguardedFills (CanFill always true; the epoch     *)
(* check stays). Expected: TriState is violated: a getattr fill that      *)
(* read D's attributes before a create's syscall records them as current  *)
(* while the create is in flight. (With only CacheNeverWrong checked, TLC *)
(* reports that one step later, once the create's syscall has changed D:  *)
(* the stale attributes are then served as current.)                      *)
(***************************************************************************)
EXTENDS MC
=============================================================================
