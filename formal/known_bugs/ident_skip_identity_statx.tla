--------------------- MODULE ident_skip_identity_statx ---------------------
(***************************************************************************)
(* OpenNode without VerifyBackingIdentity: whatever open_by_handle_at      *)
(* reaches is served as the row's object. On ext4, xfs and btrfs the       *)
(* handle carries the generation and the decode itself refuses a recycled  *)
(* inode, so the check is what stands between a recycled inode and the     *)
(* old nodeid wherever the handle does not (a filesystem whose handles     *)
(* carry no generation, btrfs's identical handle after its own power loss, *)
(* audit F6).                                                              *)
(*                                                                         *)
(* Put in by BugSkipVerify, with handles without a generation (Evidence:   *)
(* FS_IOC_GETVERSION and the birth time) and OutOfBand. Expected:          *)
(* HeldResolvesToItsObject is violated: look a up (o1, row 1); behind      *)
(* dcfs's back, unlink a and create o3, which gets o1's inode number; the  *)
(* nodeid the kernel holds for o1 resolves to o3.                          *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
