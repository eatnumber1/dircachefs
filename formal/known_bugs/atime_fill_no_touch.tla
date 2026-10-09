------------------------- MODULE atime_fill_no_touch -------------------------
(***************************************************************************)
(* Not the code (step 23.8): a record of F's attributes by another path    *)
(* than a held fill while F is open (a refresh by handle: a population's   *)
(* probe, a mutation's refresh; cache::UpdateAttr's MarkIfOpen) marks the  *)
(* row dirty but does not advance F's guard.                               *)
(*                                                                         *)
(* Put in by FillF <- FillFNoTouch. Expected: CrashSafe is violated: F's   *)
(* row is in a sync point's snapshot with F not open; F is opened, read    *)
(* and recorded by a refresh (no touch), then released with nothing new   *)
(* to record (the held fill finds the row unchanged and writes nothing);  *)
(* the sync point clears the row: its syncfs came before the read.         *)
(***************************************************************************)
EXTENDS MC

FillFNoTouch(r) ==
    IF CanFillF(r.fsnap)
    THEN /\ Commit([dbCur EXCEPT !.fValid = TRUE, !.fAttr = r.rdVer,
                                 !.fDirty = IF fm.held > 0
                                            THEN AtimeDirty(dbCur)
                                            ELSE dbCur.fDirty], FALSE)
         /\ fm' = [fm EXCEPT !.lost = FALSE]
    ELSE UnchangedDB /\ UNCHANGED fm
=============================================================================
