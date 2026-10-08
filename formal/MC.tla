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
CrashImage(s) ==
    IF s.dirty THEN [RecoverDirty(s) EXCEPT !.dirty = TRUE, !.epoch = s.epoch]
    ELSE s

(***************************************************************************)
(* An idle slot keeps the reply of the last request that held it (`rep`,   *)
(* `rb`: the reply ghost, step 12.7b). Only ReplyObservable, at the step   *)
(* that writes it, and trace validation's T_Reply (no VIEW) read it, so    *)
(* idle slots that differ only there are one state: no step's outcome      *)
(* depends on them.                                                        *)
(***************************************************************************)
IdleView(r) == IF r.pc = "idle" THEN IdleProc ELSE r

View == <<bCur, bOpts,
          IF mode \in {"down", "recover"} THEN CrashImage(dbCur) ELSE dbCur,
          {CrashImage(s) : s \in dbOpts},
          mode, seq, inflight, durableD, running,
          [q \in Procs |-> IdleView(ps[q])], servedWrong, stamp,
          muts, crashes>>
=============================================================================
