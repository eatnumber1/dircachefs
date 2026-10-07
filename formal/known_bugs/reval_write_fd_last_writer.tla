--------------------- MODULE reval_write_fd_last_writer ---------------------
(***************************************************************************)
(* Known bug: review L-a of step 23.7 (fixed there). Every allowed         *)
(* writable open beside a read-only shared fd replaced the write fd, so a  *)
(* later O_APPEND open took it from an earlier one without: that open's    *)
(* copy_file_range then failed (the kernel refuses an O_APPEND             *)
(* destination) and its fallback writes landed at the end of the file.     *)
(* Fixed: the first writer's fd is kept, and gives way only to one without *)
(* O_APPEND.                                                               *)
(*                                                                         *)
(* Re-introduced by BugWriteFdLastWriter. Expected: WriteFdKeepsOffsets is *)
(* violated: a read-only shared fd; open O_WRONLY (write fd "plain");      *)
(* open O_WRONLY|O_APPEND: the write fd now appends, for both.             *)
(***************************************************************************)
EXTENDS MCreval
=============================================================================
