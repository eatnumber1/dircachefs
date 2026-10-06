----------------- MODULE crash_f3_create_keeps_parent_attrs -----------------
(***************************************************************************)
(* Known bug: crash audit F3 (docs/plan/audits/crash.md; also races F6,   *)
(* tri-state F5). CreateChild's phase 1 did not mark the parent's         *)
(* attributes unknown, so between the create syscall and the parent's     *)
(* refresh (or forever, after a crash there) the parent's old mtime,      *)
(* ctime and nlink were served as current. Fixed: cache::BeginCreate      *)
(* marks them unknown.                                                    *)
(*                                                                         *)
(* Re-introduced by BugCreateKeepsParentAttrs. Expected: TriState is      *)
(* violated (D's attributes read valid while a create of D is in flight). *)
(***************************************************************************)
EXTENDS MC
=============================================================================
