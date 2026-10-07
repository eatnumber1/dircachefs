--------------------------- MODULE reval_no_recheck ---------------------------
(***************************************************************************)
(* Known bug: a writable open that shares a read-write backing fd took the *)
(* fd's mode as its answer (before step 23.7; found by copy_test in step   *)
(* 23.4, review L-b). DirCacheFS::Open made the shared fd O_RDWR if the    *)
(* backing filesystem allowed it and reused it for every later open, so    *)
(* after a chattr +i (through dcfs or not) a writable open of a file       *)
(* already open read-write was granted, and wrote, where a fresh open      *)
(* would get EPERM. Fixed: every writable open sharing a read-write fd     *)
(* does one FS_IOC_GETFLAGS on it and refuses as may_open would.           *)
(*                                                                         *)
(* Re-introduced by BugNoRecheck. Expected: OpenFlagsExact is violated:    *)
(* open O_WRONLY (the shared fd is O_RDWR); chattr +i through dcfs; open   *)
(* O_WRONLY again: granted, while the backing file's flags refuse it.      *)
(***************************************************************************)
EXTENDS MCreval
=============================================================================
