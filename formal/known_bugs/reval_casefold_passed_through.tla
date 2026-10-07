-------------------- MODULE reval_casefold_passed_through --------------------
(***************************************************************************)
(* Known bug: review M1 of Phase 23, fixed in step 23.7. DirCacheFS::Ioctl *)
(* forwarded FS_IOC_SETFLAGS whatever it changed, so chattr +F of an empty *)
(* directory on an ext4 with the casefold feature made it case-insensitive *)
(* under dcfs's cache of byte names: a name created afterwards left the    *)
(* cached negative entry (or complete listing) of its other case answering *)
(* absent, while the backing directory finds it. Fixed: a SETFLAGS that    *)
(* changes FS_CASEFOLD_FL gets EOPNOTSUPP.                                 *)
(*                                                                         *)
(* Re-introduced by BugCasefoldPassedThrough. Expected: DirCacheNeverWrong *)
(* is violated: "a" cached absent; chattr +F of the empty directory; a     *)
(* create of "A"; "a" still answers absent, and the backing finds "A".     *)
(***************************************************************************)
EXTENDS MCreval
=============================================================================
