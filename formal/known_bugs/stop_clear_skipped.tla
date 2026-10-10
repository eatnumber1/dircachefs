------------------------- MODULE stop_clear_skipped -------------------------
(***************************************************************************)
(* Not the code (step 12.12a, a premise for CleanMeansNoDirty): the clean  *)
(* shutdown's ClearDirty is skipped (FinishRun's sync point keeps the     *)
(* dirty rows; StopClear <- StopClearSkipped), so StopFlag commits         *)
(* clean_shutdown = 1 over a non-empty dirty set. Expected:                *)
(* CleanMeansNoDirty is violated after a mutation and a shutdown.          *)
(***************************************************************************)
EXTENDS MC
StopClearSkipped ==
    /\ mode = "stop_clear"
    /\ durableD' = FALSE /\ mode' = "stop_ckpt"
    /\ fm' = [fm EXCEPT !.durable = FALSE]
    /\ UNCHANGED <<bCur, bSeq, dbCur, dbOpts, seq, inflight, running, ps,
                   servedWrong, stamp, muts, crashes>>
=============================================================================
