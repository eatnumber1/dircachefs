------------------------------- MODULE Trace --------------------------------
(***************************************************************************)
(* Trace validation: is a recorded run of dcfs a behavior of the model?    *)
(*                                                                         *)
(* The method of Cirstea, Kuppe and Merz ("Validating traces of           *)
(* distributed programs against TLA+ specifications", 2024): the trace is *)
(* a sequence of events recorded by the code (dcfs/testonly/              *)
(* trace_recorder.cc), and this spec takes one step per event, each step  *)
(* the model action (or, for a few events, the short sequence of model    *)
(* actions) that the event stands for, constrained by what the event      *)
(* observed. Every event also carries the directory's cached state after  *)
(* it, which the model's must equal: an action that matches but leaves    *)
(* the cache differently is no match. TLC explores every behavior that    *)
(* matches the events so far; the trace is valid iff some behavior        *)
(* matches all of them. The depth TLC reports then is the number of       *)
(* events plus one; otherwise it is the index of the first event no       *)
(* behavior can explain (formal/trace_validate.sh reports it).            *)
(*                                                                         *)
(* One trace is one directory's projection of a run (formal/README.md,    *)
(* "Trace validation", explains the projection and why it is sound). It   *)
(* is read from the newline-delimited JSON file $DCFS_TRACE: the first    *)
(* line is the directory's state when its trace begins, every other line  *)
(* one event. $DCFS_TRACE_LOCK says whether the run kept the kernel's     *)
(* directory lock (KernelDirLock): a guest run does, the forged-request   *)
(* harness, which runs requests inside other requests' syscalls, does not.*)
(***************************************************************************)
EXTENDS dcfs, Json, IOUtils, Naturals, Sequences, FiniteSets, TLC

-----------------------------------------------------------------------------
(* The trace *)

Log == ndJsonDeserialize(IOEnv.DCFS_TRACE)
Header == Log[1]
Events == SubSeq(Log, 2, Len(Log))
NumEvents == Len(Log) - 1

Has(e, f) == f \in DOMAIN e

\* An event's cached state: the value of name x in it ("none": no row).
ObsVal(db, x) ==
    LET is == {i \in DOMAIN db.dent : db.dent[i][1] = x}
    IN IF is = {} THEN "none" ELSE db.dent[CHOOSE i \in is : TRUE][2]
IsKey(v) == v \notin {"none", "unknown", "absent", "refused"}

\* Every name the trace mentions (the model's Names).
EvNames(e) ==
    (IF Has(e, "n") THEN {e.n} ELSE {})
    \cup (IF Has(e, "db") THEN {e.db.dent[i][1] : i \in DOMAIN e.db.dent}
          ELSE {})
    \cup (IF Has(e, "listing")
          THEN {e.listing[i][1] : i \in DOMAIN e.listing} ELSE {})
    \cup (IF Has(e, "req") THEN {e.req.n, e.req.m} \ {""} ELSE {})
    \cup (IF Has(e, "names") THEN {e.names[i] : i \in DOMAIN e.names}
          ELSE {})
AllNames == UNION {EvNames(Log[i]) : i \in DOMAIN Log}
\* (The model needs a name; a directory whose trace has none gets one that
\* nothing ever does anything to.)
TraceNames == IF AllNames = {} THEN {"~"} ELSE AllNames

\* The request slots the trace uses, and p0, which it never does: the one a
\* whole getattr (GetattrWhole) runs in.
SlotOf(e) == IF Has(e, "p") THEN {e.p} ELSE {}
TraceProcs == {"p0"} \cup UNION {SlotOf(Events[i]) : i \in DOMAIN Events}

\* Upper bounds the trace cannot exceed (the model's MaxMutations,
\* MaxCrashes).
TraceBound == Len(Log)

TraceLock == IOEnv.DCFS_TRACE_LOCK = "true"

\* An order of the names, for the model's InitObj (overriding NameOrder, a
\* CHOOSE over all functions, which TLC cannot evaluate for many names).
RECURSIVE OrderOf(_)
OrderOf(S) == IF S = {} THEN <<>>
              ELSE LET x == CHOOSE y \in S : TRUE IN <<x>> \o OrderOf(S \ {x})
TraceNameOrder == OrderOf(Names)

\* The empty string is no name ("_" in the model).
NameOrNone(s) == IF s = "" THEN None ELSE s

-----------------------------------------------------------------------------
(* The state of the trace: the next event, and which backing object each   *)
(* model object is (the code names objects by inode number and birth time; *)
(* the model by identity). The map is a function from model objects to     *)
(* keys, not necessarily injective: two names hard-linked to one object    *)
(* (which the model does not have) are two model objects with one key. It *)
(* is forgotten at every restart: the code's view of identity across a     *)
(* crash is the backing filesystem's, which a test may have rearranged     *)
(* while dcfs was down (formal/README.md).                                 *)

VARIABLES l, okey

traceVars == <<l, okey>>

\* What an event observed about model objects: pairs <<object, key>>.
DbPairs(db) == {<<dbCur'.dent[x], ObsVal(db, x)>> :
                    x \in {y \in Names : IsKey(ObsVal(db, y))}}

\* Consistent with the map so far (`base`), and with itself; extends it.
Observe(base, pairs) ==
    /\ \A pr \in pairs : pr[1] \in DOMAIN base => base[pr[1]] = pr[2]
    /\ \A pr, pr2 \in pairs : pr[1] = pr2[1] => pr[2] = pr2[2]
    /\ okey' = [o \in DOMAIN base \cup {pr[1] : pr \in pairs} |->
                  IF o \in DOMAIN base THEN base[o]
                  ELSE (CHOOSE pr \in pairs : pr[1] = o)[2]]

\* The model's state after the step agrees with the cached state the event
\* recorded.
DbMatches(db) ==
    /\ dbCur'.complete = db.complete
    /\ dbCur'.epoch = db.epoch
    /\ dbCur'.attrValid = db.valid
    /\ dbCur'.dirty = db.dirty
    /\ dbCur'.clean = db.clean
    /\ durableD' = db.durable
    /\ inflight' = db.inflight
    /\ \A x \in Names :
         LET v == ObsVal(db, x) IN
         CASE v = "none"    -> dbCur'.dent[x] = NoRow
           [] v = "unknown" -> dbCur'.dent[x] = Unknown
           [] v = "absent"  -> dbCur'.dent[x] = Absent
           [] OTHER         -> dbCur'.dent[x] \notin {NoRow, Unknown, Absent}

\* A step for event e: the model's step (`step`, already conjoined by the
\* caller) and these: the cached state, and the objects observed.
Matches(e, extra) ==
    /\ DbMatches(e.db)
    /\ Observe(okey, DbPairs(e.db) \cup extra)
    /\ l' = l + 1

\* What a probe read, as a model object (the slot's rdObj after the step).
ProbePairs(e, o) == IF e.what = "absent" THEN {} ELSE {<<o, e.what>>}
ProbeOK(e, o) == (e.what = "absent") = (o = NoObj)

Stutter == UNCHANGED vars

-----------------------------------------------------------------------------
(* The initial state: the model's Init, except that the directory's cached *)
(* state is the one its trace begins with (a directory's trace begins when *)
(* the cache first has it; the model's Init is the empty cache), assumed  *)
(* correct and durable, and that names whose backing state the trace       *)
(* observes before anything changes them start in that state (fewer       *)
(* initial states to explore; it removes behaviors, so it cannot make an  *)
(* invalid trace valid).                                                   *)

HDB == Header.db

\* The first event that observes name x on the backing filesystem: a probe
\* of it, or a listing (which observes every name). 0 if none.
Observes(e, x) == \/ e.ev = "populate_read"
                  \/ e.ev = "probe" /\ e.n = x
FirstObs(x) == LET is == {i \in DOMAIN Events : Observes(Events[i], x)}
               IN IF is = {} THEN 0
                  ELSE CHOOSE i \in is : \A j \in is : i <= j
\* Whether a syscall that may have changed x comes before event i.
ChangedBefore(x, i) ==
    \E j \in 1..(i - 1) : /\ Events[j].ev = "syscall"
                          /\ x \in {Events[j].names[k] :
                                      k \in DOMAIN Events[j].names}
ObservedPresent(x, i) ==
    LET e == Events[i] IN
    IF e.ev = "probe" THEN e.what # "absent"
    ELSE \E k \in DOMAIN e.listing : e.listing[k][1] = x
Determined == {x \in Names : FirstObs(x) # 0 /\ ~ChangedBefore(x, FirstObs(x))}
DetPresent == {x \in Determined : ObservedPresent(x, FirstObs(x))}

\* How the directory's row came to be (the begin line's "origin"): created by
\* a mkdir (its backing directory is empty, its cached state has no
\* entries and its dirty row from the create's phase 3), first seen in its
\* parent's listing or by ParentOf (a new directories row: no entries, an
\* incomplete listing, epoch 0, not dirty), or already there when the
\* trace began ("existing": the assumption that it was correct).
Origin == IF Has(Header, "origin") THEN Header.origin ELSE "existing"
OriginOK ==
    CASE Origin = "mkdir" ->
           /\ HDB.dent = <<>> /\ HDB.dirty /\ HDB.epoch = 0
      [] Origin \in {"listing", "parent"} ->
           /\ HDB.dent = <<>> /\ ~HDB.complete /\ HDB.epoch = 0
           /\ ~HDB.dirty
      [] Origin = "existing" -> TRUE
      [] OTHER -> FALSE

TraceInit ==
    /\ l = 1
    /\ HDB.inflight = 0
    /\ OriginOK
    /\ \E free \in SUBSET (Names \ Determined) :
         LET present == DetPresent \cup free IN
         /\ Origin = "mkdir" => present = {}
         /\ \A x \in Names :
              LET v == ObsVal(HDB, x) IN
              /\ IsKey(v) => x \in present
              /\ v = "absent" => x \notin present
              /\ (v = "none" /\ HDB.complete) => x \notin present
         /\ bCur = [names |-> [x \in Names |-> IF x \in present
                                                THEN InitObj(x) ELSE NoObj],
                    ver |-> 0]
    /\ bOpts = {bCur}
    /\ dbCur = [dent |-> [x \in Names |->
                            LET v == ObsVal(HDB, x) IN
                            CASE v = "none" -> NoRow
                              [] v = "unknown" -> Unknown
                              [] v = "absent" -> Absent
                              [] OTHER -> InitObj(x)],
                complete |-> HDB.complete, epoch |-> HDB.epoch,
                attrValid |-> HDB.valid, attr |-> 0, dirty |-> HDB.dirty,
                clean |-> HDB.clean]
    /\ dbOpts = {dbCur}
    /\ okey = [o \in {InitObj(x) : x \in {y \in Names : IsKey(ObsVal(HDB, y))}}
                 |-> ObsVal(HDB, CHOOSE x \in Names : InitObj(x) = o)]
    /\ mode = "up"
    /\ seq = 0 /\ inflight = 0 /\ durableD = HDB.durable /\ running = None
    /\ ps = [p \in Procs |-> IdleProc]
    /\ servedWrong = FALSE
    /\ stamp = NumNames + 1 /\ muts = 0 /\ crashes = 0

-----------------------------------------------------------------------------
(* Steps that are more than one model action, or a model action restricted *)
(* to one of its cases. Each is a sequence of model steps with nothing in   *)
(* between, which the model allows (it allows every interleaving).        *)

\* A request of kind k (names n, m) arrives: the matching case of Arrive.
\* (Arrive itself is a disjunction over kinds and names; an event says
\* which.) The last conjunct checks the step is one of Arrive's.
ArriveAs(p, k, n, m) ==
    /\ ps[p].pc = "idle" /\ mode = "up" /\ running = None
    /\ k \in Requests
    /\ CASE k = "lookup" ->
              /\ LockFree(KernelDirLock)
              /\ LKFrom(p, NewReq("lookup", n, None, "LK", n, "Reply",
                                  KernelDirLock))
              /\ UNCHANGED muts
         [] k = "readdir" ->
              /\ LockFree(KernelDirLock)
              /\ RDFrom(p, NewReq("readdir", None, None, "RD", None, None,
                                  KernelDirLock))
              /\ UNCHANGED muts
         [] k = "readdirplus" ->
              /\ LockFree(KernelDirLock)
              /\ RDFrom(p, NewReq("readdirplus", None, None, "RD", None, None,
                                  KernelDirLock))
              /\ UNCHANGED muts
         [] k = "getattr" ->
              /\ GAFrom(p, NewReq("getattr", None, None, None, None, None,
                                  FALSE))
              /\ UNCHANGED muts
         [] k = "create" ->
              /\ LockFree(KernelDirLock)
              /\ muts < MaxMutations /\ muts' = muts + 1
              /\ C1From(p, NewReq("create", n, None, "C1", None, None,
                                  KernelDirLock))
         [] k = "unlink" ->
              /\ LockFree(KernelDirLock)
              /\ muts < MaxMutations /\ muts' = muts + 1
              /\ LKFrom(p, [NewReq("unlink", n, None, "LK", n, "U1",
                                   KernelDirLock) EXCEPT !.rsnap = seq])
         [] k = "rename" ->
              /\ LockFree(KernelDirLock)
              /\ muts < MaxMutations /\ muts' = muts + 1
              /\ n # m
              /\ LKFrom(p, [NewReq("rename", n, m, "LK", n, "R0",
                                   KernelDirLock) EXCEPT !.rsnap = seq])
         [] k = "sync" ->
              /\ S1From(p, NewReq("sync", None, None, "S1", None, None, FALSE))
              /\ UNCHANGED muts
    /\ UNCHANGED <<mode, crashes>>
    /\ Arrive(p)

\* A whole getattr of the directory, with nothing in between: Arrive
\* (GAFrom with the attributes unknown: the snapshot), GetattrStat and
\* GetattrFill. Its snapshot is taken in the same step as the fill, so
\* CanFill is "no mutation in flight". For the code's fills of a
\* directory's attributes that are not a getattr's own: a parent's listing
\* recording its child rows (PopulateDirectory, ResolveName), ParentOf,
\* and InitRoot at startup.
GetattrWhole ==
    /\ mode = "up" /\ running = None /\ "getattr" \in Requests
    /\ \E p \in Procs : ps[p].pc = "idle"
    /\ ~dbCur.attrValid
    /\ IF inflight = 0
       THEN Commit([dbCur EXCEPT !.attrValid = TRUE, !.attr = bCur.ver], FALSE)
       ELSE UnchangedDB
    /\ UNCHANGED <<bCur, bOpts, mode, seq, inflight, durableD, running, ps,
                   servedWrong, stamp, muts, crashes>>

-----------------------------------------------------------------------------
(* One action per kind of event (and model action it can be), named T_<the *)
(* model action>, so that TLC's coverage report (-coverage) says which      *)
(* model actions the trace took. e is the next event, p its slot.          *)

E == Events[l]
P == E.p
\* The next event is of kind `name` (and there is one).
Ev(name) == l <= NumEvents /\ E.ev = name
Req(e) == [k |-> e.req.k, n |-> NameOrNone(e.req.n), m |-> NameOrNone(e.req.m)]

\* LookupOrPopulate's cache read: what the event says it decided.
LookupOutcome(e, p) ==
    LET v == dbCur.dent[e.n] IN
    CASE e.out = "found"    -> v \notin {NoRow, Unknown, Absent}
      [] e.out = "neg"      -> v = Absent \/ (v = NoRow /\ dbCur.complete)
      [] e.out = "resolve"  -> ps'[p].pc = "RN_probe"
      [] e.out = "populate" -> ps'[p].pc = "PD_read"
LookupPairs(e) ==
    IF e.out = "found" THEN {<<dbCur.dent[e.n], e.key>>} ELSE {}

T_ArriveLookup ==
    /\ Ev("lookup") /\ ps[P].pc = "idle"
    /\ E.req.k \in {"lookup", "unlink", "rename"}
    /\ E.n = E.req.n
    /\ ArriveAs(P, Req(E).k, Req(E).n, Req(E).m)
    /\ LookupOutcome(E, P)
    /\ Matches(E, LookupPairs(E))

T_LookupStep ==
    /\ Ev("lookup") /\ ps[P].pc = "LK" /\ ps[P].lk = E.n
    /\ LookupStep(P)
    /\ LookupOutcome(E, P)
    /\ Matches(E, LookupPairs(E))

T_ResolveProbe ==
    /\ Ev("probe") /\ ps[P].pc = "RN_probe" /\ ps[P].lk = E.n
    /\ ResolveProbe(P)
    /\ ProbeOK(E, ps'[P].rdObj)
    /\ Matches(E, ProbePairs(E, ps'[P].rdObj))

T_ResolveCommit ==
    /\ Ev("resolve_commit")
    /\ E.recorded = CanFill(ps[P].snap)
    /\ ResolveCommit(P)
    /\ Matches(E, {})

T_PopulateRead ==
    /\ Ev("populate_read")
    /\ PopulateRead(P)
    /\ LET listed == {E.listing[k][1] : k \in DOMAIN E.listing}
           keyOf(x) == E.listing[CHOOSE k \in DOMAIN E.listing :
                                    E.listing[k][1] = x][2]
       IN /\ \A x \in Names : (x \in listed) = (ps'[P].rdList[x] # NoObj)
          /\ Matches(E, {<<ps'[P].rdList[x], keyOf(x)>> : x \in listed})

T_PopulateCommit ==
    /\ Ev("populate_commit")
    /\ E.recorded = (CanFill(ps[P].snap) /\ dbCur.epoch = ps[P].esnap)
    /\ PopulateCommit(P)
    /\ Matches(E, {})

T_ArriveReaddir ==
    /\ Ev("list_check") /\ ps[P].pc = "idle"
    /\ E.req.k \in {"readdir", "readdirplus"}
    /\ E.complete = DirListable(dbCur)
    /\ ArriveAs(P, E.req.k, None, None)
    /\ Matches(E, {})

T_ReaddirStep ==
    /\ Ev("list_check")
    /\ E.complete = DirListable(dbCur)
    /\ ReaddirStep(P)
    /\ Matches(E, {})

\* Readdirplus's check of "."'s attributes, part of its RDFrom step: it
\* found them valid (the request is done) or not (its statx is next).
T_ReaddirplusAttrCheck ==
    /\ Ev("rdp_attr_check")
    /\ IF E.valid THEN ps[P].pc = "idle" ELSE ps[P].pc = "RDP_stat"
    /\ Stutter
    /\ Matches(E, {})

T_ArriveGetattr ==
    /\ Ev("attr_check") /\ ps[P].pc = "idle"
    /\ E.valid = dbCur.attrValid
    /\ ArriveAs(P, "getattr", None, None)
    /\ Matches(E, {})

T_ReaddirplusStat == Ev("stat") /\ ReaddirplusStat(P) /\ Matches(E, {})
T_GetattrStat     == Ev("stat") /\ GetattrStat(P)     /\ Matches(E, {})
T_CreateStat      == Ev("stat") /\ CreateStat(P)      /\ Matches(E, {})
T_UnlinkStat      == Ev("stat") /\ UnlinkStat(P)      /\ Matches(E, {})
T_RenameStat      == Ev("stat") /\ RenameStat(P)      /\ Matches(E, {})

FillOK == Ev("fill") /\ E.recorded = CanFill(ps[P].snap)
T_ReaddirplusFill == FillOK /\ ReaddirplusFill(P) /\ Matches(E, {})
T_GetattrFill     == FillOK /\ GetattrFill(P)     /\ Matches(E, {})
T_CreateFill      == FillOK /\ CreateFill(P)      /\ Matches(E, {})
T_UnlinkFill      == FillOK /\ UnlinkFill(P)      /\ Matches(E, {})
T_RenameFill      == FillOK /\ RenameFill(P)      /\ Matches(E, {})

\* A fill of the directory's attributes outside its own requests (see
\* GetattrWhole); `filled` is the code's own decision. Valid attributes
\* stay valid (the model does not see the value: refreshing a correct value
\* changes nothing; the recorder makes a fill that records against the
\* guard's rule an "unexplained" line); a fill that did not record leaves
\* unknown ones unknown.
T_GetattrWhole ==
    /\ Ev("child_fill")
    /\ IF dbCur.attrValid \/ ~E.filled THEN Stutter ELSE GetattrWhole
    /\ Matches(E, {})

\* Phase 1. The code commits with a WAL fsync unless every inode it names
\* is durably dirty; the model unless the directory is (it leaves the other
\* inodes out), so where the model takes the fast path the code may still
\* fsync, which leaves fewer crash outcomes than the model allows.
SyncedOK == ~durableD => E.synced

T_ArriveCreate ==
    /\ Ev("phase1") /\ E.outcome = "begun" /\ ps[P].pc = "idle"
    /\ E.req.k = "create"
    /\ SyncedOK
    /\ ArriveAs(P, "create", Req(E).n, None)
    /\ Matches(E, {})

T_UnlinkPhase1 ==
    /\ Ev("phase1")
    /\ ps[P].pc = "U1" /\ ps[P].res.k # "neg"
    /\ (E.outcome = "begun") => SyncedOK
    /\ UnlinkPhase1(P)
    /\ (E.outcome = "begun") = (ps'[P].pc = "U_sys")
    /\ Matches(E, {})

T_RenamePhase1 ==
    /\ Ev("phase1")
    /\ ps[P].pc = "R1"
    /\ (E.outcome = "begun") => SyncedOK
    /\ RenamePhase1(P)
    /\ (E.outcome = "begun") = (ps'[P].pc = "R_sys")
    /\ Matches(E, {})

\* The unlink's resolve found nothing: its phase-1 step replies ENOENT.
T_UnlinkPhase1Absent ==
    /\ Ev("resolved") /\ ~E.found /\ ps[P].res.k = "neg"
    /\ UnlinkPhase1(P)
    /\ Matches(E, {})

\* ... or found the name: its phase-1 step (phase1 event) is next.
T_UnlinkResolved ==
    /\ Ev("resolved") /\ E.found
    /\ ps[P].pc = "U1" /\ ps[P].res.k = "found" /\ ps[P].n = E.n
    /\ Stutter
    /\ Matches(E, {})

T_RenameResolveDst ==
    /\ Ev("resolved") /\ ps[P].n = E.n
    /\ E.found = (ps[P].res.k # "neg")
    /\ RenameResolveDst(P)
    /\ Matches(E, {})

SyscallOK(okpc) == E.errno = 0 <=> ps'[P].pc = okpc
T_CreateSyscall ==
    /\ Ev("syscall") /\ CreateSyscall(P) /\ SyscallOK("C_probe")
    /\ Matches(E, {})
T_UnlinkSyscall ==
    /\ Ev("syscall") /\ UnlinkSyscall(P) /\ SyscallOK("U3")
    /\ Matches(E, {})
T_RenameSyscall ==
    /\ Ev("syscall") /\ RenameSyscall(P) /\ SyscallOK("R3")
    /\ Matches(E, {})

T_CreateProbe ==
    /\ Ev("probe") /\ ps[P].pc = "C_probe" /\ ps[P].n = E.n
    /\ CreateProbe(P)
    /\ ProbeOK(E, ps'[P].rdObj)
    /\ Matches(E, ProbePairs(E, ps'[P].rdObj))

\* The end of a mutation: its phase 3 (with what Mutation::Owns said) or
\* its failure.
OwnedOK == E.owned = Owns(ps[P])
T_CreatePhase3 ==
    /\ Ev("end") /\ (ps[P].rdObj # NoObj => OwnedOK)
    /\ CreatePhase3(P) /\ Matches(E, {})
T_UnlinkPhase3 ==
    /\ Ev("end") /\ OwnedOK /\ UnlinkPhase3(P) /\ Matches(E, {})
T_RenamePhase3 ==
    /\ Ev("end") /\ OwnedOK /\ RenamePhase3(P) /\ Matches(E, {})
T_CreateFailed == Ev("end") /\ CreateFailed(P) /\ Matches(E, {})
T_UnlinkFailed == Ev("end") /\ UnlinkFailed(P) /\ Matches(E, {})
T_RenameFailed == Ev("end") /\ RenameFailed(P) /\ Matches(E, {})

\* A failed mutation re-resolves its names: a rename's second name is
\* RenameFailed2; every other re-resolve is its LookupStep (next event).
T_RenameFailed2 ==
    /\ Ev("reresolve") /\ ps[P].pc = "R_fail2" /\ ps[P].m = E.n
    /\ RenameFailed2(P) /\ Matches(E, {})
T_Reresolve ==
    /\ Ev("reresolve") /\ ps[P].pc = "LK" /\ ps[P].lk = E.n
    /\ Stutter /\ Matches(E, {})

T_ArriveSync ==
    /\ Ev("sync_begin") /\ ps[P].pc = "idle"
    /\ ArriveAs(P, "sync", None, None)
    /\ Matches(E, {})
\* syncfs returned: no model step (the model's syncfs took effect at S1).
T_Syncfs ==
    /\ Ev("syncfs") /\ ps[P].pc = "S2" /\ Stutter /\ Matches(E, {})
T_SyncClearDirty ==
    /\ Ev("sync_clear") /\ SyncClearDirty(P) /\ Matches(E, {})

\* The request replied; the model's request had already (its last step).
T_Reply ==
    /\ Ev("reply") /\ ps[P].pc = "idle" /\ Stutter /\ Matches(E, {})

T_BeginShutdown == Ev("shutdown") /\ BeginShutdown /\ Matches(E, {})
T_StopSync == Ev("stop_sync") /\ StopSync /\ Matches(E, {})
T_StopClear == Ev("stop_clear") /\ StopClear /\ Matches(E, {})
T_StopCkpt == Ev("checkpoint") /\ StopCkpt /\ Matches(E, {})
T_StopFlag == Ev("clean") /\ StopFlag /\ Matches(E, {})

T_Crash == Ev("crash") /\ Crash /\ Matches(E, {})
\* A new process: the object map starts over (see okey).
T_Restart ==
    /\ Ev("restart") /\ Restart
    /\ DbMatches(E.db) /\ Observe(<<>>, DbPairs(E.db)) /\ l' = l + 1
\* RecoverDirty, as the model has it (a dirty directory forgets its
\* dentries and attributes), and one thing the model leaves out: the code
\* also makes unknown every dentry that points at a dirty inode, wherever
\* it is ("its name may have changed"), and the model has no child objects
\* in the dirty set. So a present name of this directory may become unknown
\* too; the event says which. (Unknown is always safe; formal/README.md,
\* "Findings of trace validation".)
T_Recover ==
    /\ Ev("recover") /\ mode = "recover"
    /\ LET rd == RecoverDirty(dbCur)
           new == [rd EXCEPT !.dent =
                     [x \in Names |->
                        IF /\ rd.dent[x] \notin {NoRow, Unknown, Absent}
                           /\ ObsVal(E.db, x) = "unknown"
                        THEN Unknown ELSE rd.dent[x]]]
       IN Commit(new, FALSE)
    /\ mode' = "start"
    /\ UNCHANGED <<bCur, bOpts, seq, inflight, durableD, running, ps,
                   servedWrong, stamp, muts, crashes>>
    /\ Matches(E, {})
T_StartRun == Ev("start_run") /\ StartRun /\ Matches(E, {})

\* One step per event (each action checks there is a next event, and moves
\* past it).
TraceNext ==
    \/ T_ArriveLookup \/ T_LookupStep \/ T_ResolveProbe \/ T_ResolveCommit
    \/ T_PopulateRead \/ T_PopulateCommit
    \/ T_ArriveReaddir \/ T_ReaddirStep \/ T_ReaddirplusAttrCheck
    \/ T_ArriveGetattr
    \/ T_ReaddirplusStat \/ T_GetattrStat \/ T_CreateStat \/ T_UnlinkStat
    \/ T_RenameStat
    \/ T_ReaddirplusFill \/ T_GetattrFill \/ T_CreateFill \/ T_UnlinkFill
    \/ T_RenameFill
    \/ T_GetattrWhole
    \/ T_ArriveCreate \/ T_UnlinkPhase1 \/ T_RenamePhase1
    \/ T_UnlinkPhase1Absent \/ T_UnlinkResolved \/ T_RenameResolveDst
    \/ T_CreateSyscall \/ T_UnlinkSyscall \/ T_RenameSyscall
    \/ T_CreateProbe
    \/ T_CreatePhase3 \/ T_UnlinkPhase3 \/ T_RenamePhase3
    \/ T_CreateFailed \/ T_UnlinkFailed \/ T_RenameFailed
    \/ T_RenameFailed2 \/ T_Reresolve
    \/ T_ArriveSync \/ T_Syncfs \/ T_SyncClearDirty
    \/ T_Reply
    \/ T_BeginShutdown \/ T_StopSync \/ T_StopClear \/ T_StopCkpt
    \/ T_StopFlag
    \/ T_Crash \/ T_Restart \/ T_Recover \/ T_StartRun

TraceSpec == TraceInit /\ [][TraceNext]_<<vars, traceVars>>
=============================================================================
