------------------------- MODULE reval_no_write_fd -------------------------
(***************************************************************************)
(* Known bug: review L1 of Phase 23, fixed in step 23.7. A writable open   *)
(* that shares a read-only backing fd (the file was immutable when it was *)
(* opened) and passes the backing filesystem's check kept no descriptor    *)
(* that can write, so what dcfs writes itself (fallback writes, fallocate, *)
(* copy_file_range) went through the read-only fd and got EBADF. Fixed:    *)
(* that open's reopened descriptor is kept as the write fd.                *)
(*                                                                         *)
(* Re-introduced by BugNoWriteFd. Expected: WriteFdHeld is violated: an   *)
(* open O_WRONLY|O_APPEND of an append-only file (the shared fd falls back *)
(* to O_RDONLY, the reopen with the open's mode is allowed) is granted     *)
(* with nothing that can carry its writes (L1's second scenario; its first *)
(* is a read-only open of an immutable file, chattr -i, a writable open).  *)
(***************************************************************************)
EXTENDS MCreval
=============================================================================
