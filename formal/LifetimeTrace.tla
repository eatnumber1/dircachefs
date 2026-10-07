---------------------------- MODULE LifetimeTrace ----------------------------
(***************************************************************************)
(* Trace validation for the lifetime model (lifetime.tla): is a recorded  *)
(* run's history of one nodeid a behavior of the model's nodeids? The     *)
(* method is Trace.tla's (formal/README.md, "Trace validation"): one step *)
(* per event, the model step the event stands for, constrained by what    *)
(* the event observed; the trace is valid iff some behavior matches every *)
(* event, and then TLC's search depth is the number of events plus one.   *)
(*                                                                         *)
(* A trace is one nodeid's: the recorder (dcfs/testonly/trace_recorder.cc, *)
(* made with `lifetimes`) begins it at the entry reply dcfs counts first,  *)
(* or at the CREATE or TMPFILE that made it, and writes a line for each    *)
(* step of LifetimeChanged and for DESTROY and the starts. Each line       *)
(* carries what dcfs keeps for the nodeid after the step ("st"), which the *)
(* model's state must equal: its lookup count, removed record, written_    *)
(* entry and open files, and its row and the row's nlink (the run's lines: *)
(* only the row).                                                          *)
(*                                                                         *)
(* The steps are lifetime.tla's per-nodeid operators (AfterLookup, ...),   *)
(* the same ones its actions apply, so a Bug* constant changes both. What  *)
(* a trace leaves free: whether the object still has a name on the backing *)
(* filesystem (`named`: set by a create, a link or a tmpfile, chosen at a  *)
(* removal, and otherwise unchanged, under exclusive access), the held     *)
(* descriptor's cap (a release's heldOk), and the other nodeids and the    *)
(* directory (no event of theirs is mapped here). lifetime.tla's own       *)
(* variables stay at their initial values.                                 *)
(***************************************************************************)
EXTENDS lifetime, Json, IOUtils, Naturals, Sequences, TLC

Log == ndJsonDeserialize(IOEnv.DCFS_TRACE)
Events == SubSeq(Log, 2, Len(Log))
NumEvents == Len(Log) - 1

\* Bounds the trace cannot exceed.
TraceMax == Len(Log) + 1
\* The traced nodeid's object, and an empty directory for lifetime.tla's
\* own (unused) variables.
TraceObjs == {"o"}
TraceBName == [n \in {} |-> None]

Has(e, f) == f \in DOMAIN e

VARIABLES
    nd,     \* the traced nodeid's state (lifetime.tla's IdStates)
    named,  \* its object has a name on the backing filesystem
    cln,    \* the last shutdown was clean (for the start's sweep)
    l       \* the next event

E == Events[l]
Ev(name) == l <= NumEvents /\ E.ev = name

\* What the code kept equals the model's, field by field (the run's lines
\* carry only the row).
Matches(k) ==
    /\ Has(k, "lk") => nd'.lk = k.lk
    /\ Has(k, "rec") => nd'.rec = k.rec
    /\ Has(k, "wr") => nd'.wr = k.wr
    /\ Has(k, "refs") => nd'.op = k.refs
    /\ nd'.row = k.row
    /\ nd'.nl0 = k.nl0

Step == /\ Matches(E.st)
        /\ l' = l + 1
        /\ UNCHANGED vars

\* The begin line: no lookup counted, nothing kept, the row as it was.
TraceInit ==
    /\ Init
    /\ LET b == Log[1].st IN
         /\ b.lk = 0 /\ ~b.rec /\ b.wr = "no" /\ b.refs = 0
         /\ nd = [Fresh EXCEPT !.row = b.row, !.nl0 = b.nl0]
    /\ named \in BOOLEAN
    /\ cln \in BOOLEAN
    /\ l = 1

\* An entry reply: by a name of the object (Lookup: its row found or
\* made), of "." or ".." (no name: the row must exist), or a LINK's (Link:
\* a new name, nlink no longer 0).
T_LifeLookup ==
    /\ Ev("lookup") /\ E.via = "lookup" /\ named
    /\ nd' = AfterLookup(WithRow(nd, FALSE), "o")
    /\ UNCHANGED <<named, cln>>
    /\ Step
T_LifeLookupDot ==
    /\ Ev("lookup") /\ E.via = "dot" /\ nd.row
    /\ nd' = AfterLookup(nd, "o")
    /\ UNCHANGED <<named, cln>>
    /\ Step
T_LifeLink ==
    /\ Ev("lookup") /\ E.via = "link" /\ nd.row
    /\ nd' = AfterLookup([nd EXCEPT !.nl0 = FALSE], "o")
    /\ named' = TRUE
    /\ UNCHANGED cln
    /\ Step

\* CREATE and TMPFILE: a new nodeid's first step.
T_LifeCreate ==
    /\ Ev("create") /\ nd = Fresh
    /\ nd' = AfterOpen(AfterLookup(WithRow(nd, FALSE), "o"), E.w)
    /\ named' = TRUE
    /\ UNCHANGED cln
    /\ Step
T_LifeTmpfile ==
    /\ Ev("tmpfile") /\ nd = Fresh
    /\ nd' = AfterOpen(AfterLookup(WithRow(nd, TRUE), "o"), TRUE)
    /\ named' = FALSE
    /\ UNCHANGED cln
    /\ Step

\* OPEN of a nodeid the kernel holds, of its row or its removed record.
T_LifeOpen ==
    /\ Ev("open") /\ nd.k > 0 /\ (nd.row \/ nd.rec)
    /\ nd' = AfterOpen(nd, E.w)
    /\ UNCHANGED <<named, cln>>
    /\ Step

\* RELEASE (the held descriptor's cap left free).
T_LifeRelease ==
    /\ Ev("release") /\ nd.op > 0 /\ MayRelease(nd, E.w)
    /\ \E heldOk \in BOOLEAN : nd' = AfterRelease(nd, E.w, named, heldOk)
    /\ UNCHANGED <<named, cln>>
    /\ Step

\* An unlink's, rmdir's or rename's removal of one of the object's names,
\* both steps (Remove, then Settle): HoldForRemoval held it iff dcfs
\* counted a lookup; whether a name is left is free.
T_LifeRemoved ==
    /\ Ev("removed")
    /\ E.held = (nd.lk > 0)
    /\ \E nm \in BOOLEAN :
         /\ named' = nm
         /\ nd' = AfterSettle(WithRow(nd, FALSE), nm, E.held)
    /\ UNCHANGED cln
    /\ Step

\* FORGET, or an entry of a FORGET_MULTI: the kernel must hold the lookups
\* it forgets (a FORGET of more than dcfs counted is no behavior).
T_LifeForget ==
    /\ Ev("forget")
    /\ MayForgetS(nd, E.n)
    /\ nd' = AfterForget(nd, E.n, IF E.batch THEN Counted(E.n) ELSE E.n)
    /\ UNCHANGED <<named, cln>>
    /\ Step

\* DESTROY, a crash, a start after a clean shutdown: the kernel holds
\* nothing, dcfs's memory is gone; the row stays. Whether the shutdown
\* after a DESTROY is clean depends on every nodeid's writable opens, not
\* only this one's: the next run's crash or restart line says.
T_LifeDestroy ==
    /\ Ev("destroy") /\ nd' = AfterReset(nd) /\ cln' \in BOOLEAN
    /\ UNCHANGED named
    /\ Step
T_LifeCrash ==
    /\ Ev("crash") /\ nd' = AfterReset(nd) /\ cln' = FALSE
    /\ UNCHANGED named
    /\ Step
T_LifeRestart ==
    /\ Ev("restart") /\ nd' = AfterReset(nd) /\ cln' = TRUE
    /\ UNCHANGED named
    /\ Step

\* The start ran: after an unclean shutdown, its probe of the rows that
\* were dirty (which the trace does not see: a row of an object with no
\* name, freed by the crash, may go); at every start, its sweep of unnamed
\* rows.
T_LifeStart ==
    /\ Ev("start")
    /\ LET swept == IF BugNoUnnamedSweep \/ (cln /\ BugSweepOnlyUnclean)
                    THEN nd ELSE AfterSweep(nd, named)
       IN nd' \in IF cln THEN {swept}
                  ELSE {swept} \cup
                       (IF BugNoRecoveredProbe THEN {}
                        ELSE {AfterProbe(swept, ~named)})
    /\ UNCHANGED <<named, cln>>
    /\ Step

TraceNext ==
    \/ T_LifeLookup \/ T_LifeLookupDot \/ T_LifeLink \/ T_LifeCreate
    \/ T_LifeTmpfile \/ T_LifeOpen \/ T_LifeRelease \/ T_LifeRemoved
    \/ T_LifeForget \/ T_LifeDestroy \/ T_LifeCrash \/ T_LifeRestart
    \/ T_LifeStart
=============================================================================
