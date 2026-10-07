---------------- MODULE lifetime_tmpfile_row_survives_crash -----------------
(***************************************************************************)
(* The bug before step 23.7 (review L5): an O_TMPFILE file's row, which    *)
(* its last release deletes, survived a crash (no release ever came), and  *)
(* nothing ever deleted it: nothing can reach it by name, and its handle   *)
(* no longer decodes.                                                      *)
(*                                                                         *)
(* Put in by BugNoUnnamedSweep (the start does not run                     *)
(* cache::ForgetUnnamedRows). Expected: UnnamedRowsSwept is violated: a    *)
(* TMPFILE (a row with nlink 0 and no name, open); a crash; the start: the *)
(* row is still there, with nothing open.                                  *)
(***************************************************************************)
EXTENDS MClifetime
=============================================================================
