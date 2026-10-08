------------------- MODULE reply_after_failed_syscall --------------------
(***************************************************************************)
(* A mutation result the backing filesystem never gave (step 12.7b): a     *)
(* create whose syscall failed with EEXIST re-resolves its name, as        *)
(* ReresolveAfterFailure does, and then replies success instead of the     *)
(* syscall's error. Put in by overriding FailedReply with dcfs.tla's       *)
(* FailedReplyOK from the configuration. Every record stays right (the     *)
(* re-resolve records what is there), so no invariant sees it.             *)
(*                                                                         *)
(* Expected: ReplyObservable is violated: a already exists (o1); a create  *)
(* of a runs phase 1 and its syscall, which fails with EEXIST; the create  *)
(* ends and re-resolves a (the listing); it replies success, while the     *)
(* backing filesystem answered EEXIST at the create's only effect point.   *)
(***************************************************************************)
EXTENDS MC
=============================================================================
