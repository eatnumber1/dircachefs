----------------------- MODULE reval_recheck_if_changed -----------------------
(***************************************************************************)
(* A fix the code declined (russ, step 23.7): re-check a writable open     *)
(* that shares a read-write fd only if the flags changed through dcfs      *)
(* since the fd was opened (a flags_changed bit, no syscall otherwise).    *)
(* Under dcfs's exclusive-access rule that is enough:                      *)
(* reval_recheck_if_changed_exclusive.cfg (OutOfBand = FALSE) finds no     *)
(* violation of any invariant. With changes behind dcfs's back it is not:  *)
(* reval_recheck_if_changed.cfg (OutOfBand = TRUE) expects OpenFlagsExact  *)
(* to be violated: open O_WRONLY (the shared fd is O_RDWR); chattr +a      *)
(* directly on the backing file; open O_WRONLY again: granted, although    *)
(* the file is append-only and the open has no O_APPEND. The code's        *)
(* unconditional GETFLAGS costs one ioctl per shared writable open and     *)
(* passes MC_reval_oob.cfg.                                                *)
(*                                                                         *)
(* Put in by BugRecheckOnlyIfChanged (sfd.chg is the bit it would keep).   *)
(***************************************************************************)
EXTENDS MCreval
=============================================================================
