--------------------------------- MODULE MC ---------------------------------
(***************************************************************************)
(* The model-checking root module: the configurations (MC_small.cfg,      *)
(* MC_large.cfg, MC_nolock.cfg, MC_liveness.cfg, known_bugs/*.cfg) check  *)
(* this module and bind dcfs's constants. See README.md for what each     *)
(* configuration checks and how long it takes.                            *)
(***************************************************************************)
EXTENDS dcfs

\* Every request kind, and subsets of them: for known-bug configurations
\* that leave out a request whose bug (when it was open) would otherwise
\* have stopped TLC first.
\* The attribute change of D (step 12.11) is checked by the medium
\* configurations (WithAttrChanges: MC_small, MC_recovery, MC_liveness,
\* MC_interrupt) and trace validation; the large ones and the known bugs
\* keep the request kinds they had (AllRequests), whose state spaces it
\* would multiply for a request that interleaves only with getattrs and
\* syncs under the kernel's lock.
WithAttrChanges == AllKinds
AllRequests == AllKinds \ {"attrchange"}
AllButRename == AllRequests \ {"rename"}
AllButReaddirplus == AllRequests \ {"readdirplus"}
AllButRenameAndReaddirplus == AllRequests \ {"rename", "readdirplus"}

(***************************************************************************)
(* View: what TLC uses to tell states apart (the VIEW in a .cfg). Two      *)
(* database states a crash may leave that recovery turns into the same     *)
(* cache are the same choice: a dirty state's rows and attributes are      *)
(* forgotten by RecoverDirty anyway. CrashImage keeps exactly what the     *)
(* rest of the specification reads of such a state (what RecoverDirty      *)
(* makes of it, and its dirty and clean flags), so merging them loses no   *)
(* behavior; it cuts the state space severalfold. Not used with liveness   *)
(* checking (TLC does not support a VIEW there).                           *)
(***************************************************************************)
CrashImageD(s) ==
    IF s.dirty THEN [RecoverD(s) EXCEPT !.dirty = TRUE, !.epoch = s.epoch]
    ELSE s
\* F's attributes likewise, when its row is dirty (whatever the reason,
\* which is kept: PowerLoss and the invariants read it).
CrashImage(s) ==
    IF s.fDirty # "no"
    THEN [CrashImageD(s) EXCEPT !.fValid = FALSE, !.fAttr = 0]
    ELSE CrashImageD(s)

\* F's guard clock (step 23.8) is compared only with the snapshots the
\* slots took of it (CanFillF, S2), and a snapshot is never above it, so
\* all the view keeps of it is which snapshots are still current: F's
\* concurrent held fills can touch it without bound (each one that finds
\* another's touch marks the attributes unknown and touches in turn), and
\* its raw value would make the state space infinite.
\*
\* Only at the steps that read a snapshot (FSnapPcs: S1 and FStat write it
\* first) is it current (1) or stale (2); everywhere else the view keeps 0.
\* Step 23.8b: the first version normalized every slot's `fsnap`,
\* including the 0 of a slot that has taken no snapshot, which read as
\* current exactly while the clock was 0. With the clock zeroed in the
\* view, a state at clock 0 and one at clock k (otherwise equal, and with
\* the same future: nothing but the guards reads the clock or a
\* snapshot) were one state, but a request arriving in either got
\* different views ("1" and "2"), so the view was not a congruence: which
\* of those successors TLC reached depended on which state it had kept,
\* and the distinct-state count on the number of workers (MC_atime.cfg:
\* 263,723 with one worker, 263,890, 263,796 and 263,848 with four).
FSnapPcs == {"S2", "FR_fill", "FG_fill", "FG_rfill", "FS_fill"}
FSnapView(r) ==
    [r EXCEPT !.fsnap = IF r.pc \notin FSnapPcs THEN 0
                        ELSE IF r.fsnap = fm.seq THEN 1 ELSE 2]

(***************************************************************************)
(* An idle slot keeps the reply of the last request that held it (`rep`,   *)
(* `rb`: the reply ghost, step 12.7b). Only ReplyObservable, at the step   *)
(* that writes it, and trace validation's T_Reply (no VIEW) read it, so    *)
(* idle slots that differ only there are one state: no step's outcome      *)
(* depends on them.                                                        *)
(***************************************************************************)
IdleView(r) == IF r.pc = "idle" THEN IdleProc ELSE r

View == <<bCur, bSeq,
          IF mode \in {"down", "recover"} THEN CrashImage(dbCur) ELSE dbCur,
          {CrashImage(s) : s \in dbOpts},
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> IdleView(FSnapView(ps[q]))], servedWrong,
          [fm EXCEPT !.seq = 0], stamp, muts, crashes>>
=============================================================================
