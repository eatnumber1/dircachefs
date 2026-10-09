--------------------- MODULE atime_held_fill_not_dirty ---------------------
(***************************************************************************)
(* Not the code (step 23.8): the held fill (backing::FillHeldAttrs)       *)
(* records what its statx read without marking F's row dirty and without  *)
(* advancing F's guard. The backing filesystem writes a read's access     *)
(* time back lazily; only a later syncfs makes it durable.                 *)
(*                                                                         *)
(* Put in by HeldFillMark <- HeldFillMarkNotDirty. Expected: CrashSafe is  *)
(* violated: F's row is dirty (here: a crash while F was open, which the   *)
(* start recovers and keeps dirty); a sync point takes its snapshot with   *)
(* F not open; F is opened (warm: the row is dirty already), read, and     *)
(* released, the held fill recording the read's access time with no mark  *)
(* and no touch; the sync point, seeing F neither open nor touched since   *)
(* its snapshot, clears the row. Its syncfs came before the read: a power  *)
(* loss now may take the backing filesystem back to before the read while  *)
(* the cache keeps its access time, not dirty (cache ahead).               *)
(***************************************************************************)
EXTENDS MC

HeldFillMarkNotDirty(r, ok) ==
    /\ Commit([dbCur EXCEPT !.fValid = ok,
                            !.fAttr = IF ok THEN r.rdVer ELSE 0], FALSE)
    /\ fm' = [fm EXCEPT !.held = @ - r.frel, !.lost = FALSE]
=============================================================================
