------------------ MODULE lifetime_destroy_with_open_files ------------------
(***************************************************************************)
(* Found by this model (formerly findings/), fixed in step 12.4b: DESTROY  *)
(* with a file still open (SIGTERM, or a lazy unmount: libfuse aborts the  *)
(* connection) and a clean shutdown (no writable open left: one keeps its  *)
(* row durably dirty, and a TMPFILE still open for writing made the next   *)
(* start unclean, and swept). The start swept unnamed rows only after an   *)
(* unclean shutdown, so the row of an unlinked file open only for reading  *)
(* (nlink 0 since its phase 3), or of an O_TMPFILE file reopened read-only *)
(* after its writable descriptor closed, stayed, its object freed. The     *)
(* fix: the sweep runs at every start, reading only the rows of the        *)
(* partial index inodes_unlinked (nlink = 0).                              *)
(*                                                                         *)
(* Put in by BugSweepOnlyUnclean, with DestroyWithOpens. Expected:         *)
(* UnnamedRowsSwept is violated: look a up; open it read-only; unlink it   *)
(* (phase 3 keeps the row, nlink 0); DESTROY; the start (clean, no sweep). *)
(***************************************************************************)
EXTENDS MClifetime
=============================================================================
