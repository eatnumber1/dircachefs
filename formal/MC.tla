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
AllRequests == AllKinds
AllButRename == AllKinds \ {"rename"}
AllButReaddirplus == AllKinds \ {"readdirplus"}
AllButRenameAndReaddirplus == AllKinds \ {"rename", "readdirplus"}

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

View == <<bCur, bOpts,
          IF mode \in {"down", "recover"} THEN CrashImage(dbCur) ELSE dbCur,
          {CrashImage(s) : s \in dbOpts},
          mode, seq, inflight, durableD, running, ps, servedWrong, stamp,
          muts, crashes>>
=============================================================================
