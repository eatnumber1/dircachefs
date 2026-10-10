------------------------ MODULE probes_wait_for_clean ------------------------
(***************************************************************************)
(* Not the code (step 12.12a, a premise for RecoveryTerminates): the       *)
(* start's probe ends only once the dirty set is empty (ProbesDone <-      *)
(* ProbesDoneWaits), which nothing empties while the daemon is not        *)
(* serving (only a sync point does). The wait is a stutter, not a missing  *)
(* step, so that TLC reports the liveness violation rather than a         *)
(* deadlock. Expected: RecoveryTerminates is violated after a crash that  *)
(* leaves a dirty row.                                                     *)
(***************************************************************************)
EXTENDS MC
\* (dcfs.tla's ProbesDone, which the configuration replaces, so it is
\* repeated here; with nothing dirty there is no row for it to delete.)
ProbesDoneWaits ==
    /\ mode = "probe"
    /\ IF ~dbCur.dirty /\ dbCur.fDirty = "no"
       THEN /\ mode' = "up"
            /\ UNCHANGED <<bCur, bSeq, dbCur, dbOpts, seq, inflight, durableD,
                           fm, running, ps, servedWrong, stamp, muts, crashes>>
       ELSE UNCHANGED vars
=============================================================================
