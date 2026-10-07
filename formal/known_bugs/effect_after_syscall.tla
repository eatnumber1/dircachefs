---------------------- MODULE effect_after_syscall ----------------------
(***************************************************************************)
(* An effect point too late (step 12.7): crash F3's create, whose phase 1  *)
(* leaves D's cached attributes valid (BugCreateKeepsParentAttrs). The    *)
(* create's syscall changes D's mtime/ctime on the backing filesystem but  *)
(* other requests are still answered the old attributes from the cache, so *)
(* what they see changes only later, when phase 3 records the new ones.    *)
(*                                                                         *)
(* Expected: EffectAtSyscall is violated: a getattr records D's            *)
(* attributes; a create of a: phase 1 (attributes still valid), the        *)
(* syscall (others still see the cached attributes), the probe, phase 3,   *)
(* the refresh's statx; its fill records the new attributes: what others  *)
(* see changes there, after the syscall.                                   *)
(***************************************************************************)
EXTENDS MC
=============================================================================
