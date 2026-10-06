-------------------- MODULE crash_f1_phase1_not_durable --------------------
(***************************************************************************)
(* Known bug: crash audit F1 (docs/plan/audits/crash.md). Phase 1 of a    *)
(* mutation committed at normal durability, so a power loss could keep    *)
(* the backing syscall and lose phase 1 ("cache behind": a removed name   *)
(* still cached, a created name still cached absent). Fixed by the        *)
(* durable dirty set: cache::BeginMutation commits with                   *)
(* sqlite3::Durability::kSync before the syscall.                         *)
(*                                                                         *)
(* Re-introduced by BugPhase1NotDurable (BeginMutation never commits      *)
(* kSync). Only CacheNeverWrong is checked (not CrashSafe, which finds it *)
(* sooner), so that the counterexample shows an actual crash, recovery    *)
(* and the wrong cache being served from. Expected: CacheNeverWrong is    *)
(* violated.                                                              *)
(***************************************************************************)
EXTENDS MC
=============================================================================
