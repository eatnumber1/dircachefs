-------------------------------- MODULE MCview --------------------------------
(***************************************************************************)
(* Step 23.8b: the View of MC.tla with one component removed at a time,   *)
(* to find which one merges states that are not equivalent (a VIEW that    *)
(* merges only equivalent states gives the same distinct-state count       *)
(* whatever the number of TLC's workers). The configurations are           *)
(* view_bisect/*.cfg; the targets atime_view_*_test in BUILD.bazel.       *)
(***************************************************************************)
EXTENDS MC

\* FSnapView's premise: no slot's snapshot of F's guard is ahead of it (so
\* only whether it is current matters, and the clock can be zeroed).
FSnapNotAhead == \A p \in Procs : ps[p].fsnap <= fm.seq

\* Without FSnapView: the slots' snapshots of F's guard and the clock
\* itself kept as they are.
ViewNoFSnap == <<bCur, bSeq,
          IF mode \in {"down", "recover"} THEN CrashImage(dbCur) ELSE dbCur,
          {CrashImage(s) : s \in dbOpts},
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> IdleView(ps[q])], servedWrong,
          fm, stamp, muts, crashes>>

\* FSnapView, but F's guard clock itself kept (not zeroed).
ViewFSnapRawSeq == <<bCur, bSeq,
          IF mode \in {"down", "recover"} THEN CrashImage(dbCur) ELSE dbCur,
          {CrashImage(s) : s \in dbOpts},
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> IdleView(FSnapView(ps[q]))], servedWrong,
          fm, stamp, muts, crashes>>

\* Without IdleView: idle slots keep their last reply.
ViewNoIdle == <<bCur, bSeq,
          IF mode \in {"down", "recover"} THEN CrashImage(dbCur) ELSE dbCur,
          {CrashImage(s) : s \in dbOpts},
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> FSnapView(ps[q])], servedWrong,
          [fm EXCEPT !.seq = 0], stamp, muts, crashes>>

\* Only FSnapView and the zeroed clock: no IdleView, no CrashImage.
ViewOnlyFSnap == <<bCur, bSeq, dbCur, dbOpts,
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> FSnapView(ps[q])], servedWrong,
          [fm EXCEPT !.seq = 0], stamp, muts, crashes>>

\* Without CrashImage: the database states as they are.
ViewNoImage == <<bCur, bSeq, dbCur, dbOpts,
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> IdleView(FSnapView(ps[q]))], servedWrong,
          [fm EXCEPT !.seq = 0], stamp, muts, crashes>>
=============================================================================
