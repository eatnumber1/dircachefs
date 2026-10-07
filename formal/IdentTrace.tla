----------------------------- MODULE IdentTrace -----------------------------
(***************************************************************************)
(* Trace validation for the identity model (ident.tla): is a recorded      *)
(* run's history of one nodeid's identity a behavior of the model's? The   *)
(* method is Trace.tla's (formal/README.md, "Trace validation"): one step  *)
(* per event, constrained by what the event observed; the trace is valid   *)
(* iff some behavior matches every event, and then TLC's search depth is   *)
(* the number of events plus one.                                          *)
(*                                                                         *)
(* A trace is one nodeid's: the recorder (dcfs/testonly/trace_recorder.cc, *)
(* made with `identities`) begins it at the entry reply dcfs counts first, *)
(* and writes a line for each entry reply (the generation it carried),     *)
(* each FORGET (the lookups left), each reopen of its handle               *)
(* (IdentityResolved: the outcome, and how what it reached compares with   *)
(* the row), its row going, DESTROY and the starts. Each line carries      *)
(* whether the row exists.                                                 *)
(*                                                                         *)
(* What it checks, in ident.tla's terms: a nodeid's replies all carry one  *)
(* generation (NoBadInode, OneHandleOneObject for its own references); a  *)
(* nodeid whose row went is never handed out again (AUTOINCREMENT: today's *)
(* identity only; Phase 14's nodeid is the inode number, which comes back, *)
(* and T_IdReply must then be relaxed); a reopen is served exactly when    *)
(* ident.tla's identity check                                              *)
(* (SameObject, or anything with BugSkipVerify) says the object it reached *)
(* is the row's, and a stale or mismatched one makes the row go before     *)
(* anything else happens to the nodeid (ForgetStale). The comparison      *)
(* outcomes ("same", "other", "unknown") stand for abstract identities:   *)
(* the row's fields are 1 (known) or 0 (unknown), the reached object's 1   *)
(* (the same), 2 (another) or 0. Whether a stale handle's object was freed *)
(* or recycled is free. ident.tla's own variables stay at their initial    *)
(* values.                                                                 *)
(***************************************************************************)
EXTENDS ident, Json, IOUtils, Naturals, Sequences, TLC

Log == ndJsonDeserialize(IOEnv.DCFS_TRACE)
Events == SubSeq(Log, 2, Len(Log))
NumEvents == Len(Log) - 1

\* Empty values for ident.tla's own (unused) constants.
TraceNames == [n \in {} |-> None]
TraceInts == [o \in {} |-> 0]

VARIABLES
    row,    \* the traced nodeid has a row
    gone,   \* ... which went during the trace
    fgen,   \* the generation its replies carried ("" before the first)
    held,   \* the kernel holds an inode for it (dcfs counted lookups)
    stale,  \* a reopen answered ESTALE: its row must go next
    l       \* the next event

traceVars == <<row, gone, fgen, held, stale, l>>

E == Events[l]
Ev(name) == l <= NumEvents /\ E.ev = name
Has(e, f) == f \in DOMAIN e

\* What the code has equals the model's: the row, and dcfs's lookup count
\* where the line has it.
Step ==
    /\ row' = E.st.row
    /\ Has(E.st, "lk") => (held' = (E.st.lk > 0))
    /\ l' = l + 1
    /\ UNCHANGED vars

TraceInit ==
    /\ Init
    /\ row = Log[1].st.row
    /\ gone = FALSE /\ fgen = "" /\ held = FALSE /\ stale = FALSE
    /\ l = 1

\* An entry reply (LOOKUP, a READDIRPLUS entry, LOOKUP of "." or "..",
\* LINK, CREATE, TMPFILE): the row exists, was never deleted in this trace
\* (no nodeid is handed out twice), and the generation is the one every
\* earlier reply carried.
T_IdReply ==
    /\ Ev("reply") /\ ~stale /\ ~gone
    /\ E.st.row /\ E.fgen # ""
    /\ fgen = "" \/ fgen = E.fgen
    /\ fgen' = E.fgen /\ held' = TRUE
    /\ UNCHANGED <<gone, stale>>
    /\ Step

\* A FORGET (or a FORGET_MULTI entry): the lookups left.
T_IdForget ==
    /\ Ev("forget") /\ ~stale
    /\ UNCHANGED <<gone, fgen, stale>>
    /\ Step

\* The abstract identities a resolve line's comparisons stand for.
RowSide(c) == IF c = "unknown" THEN 0 ELSE 1
FoundSide(c) == CASE c = "same" -> 1 [] c = "other" -> 2 [] OTHER -> 0
\* ident.tla's decision for a handle that reached an object (Resolve).
Served(e) ==
    BugSkipVerify \/
    SameObject([ino |-> 1, gen |-> RowSide(e.gen), bt |-> RowSide(e.bt)],
               [ino |-> FoundSide(e.ino), gen |-> FoundSide(e.gen),
                bt |-> FoundSide(e.bt)])

\* A reopen of its handle (OpenNode): a stale handle (its object freed, or
\* recycled with another generation in the handle), or an object it
\* reached, served exactly when the identity check says it is the row's.
T_IdResolve ==
    /\ Ev("resolve") /\ ~stale /\ row
    /\ IF E.outcome = "stale_handle" THEN TRUE
       ELSE (E.outcome = "served") = Served(E)
    /\ stale' = (E.outcome # "served")
    /\ UNCHANGED <<gone, fgen, held>>
    /\ Step

\* Its row goes (InodeForgotten).
T_IdGone ==
    /\ Ev("gone") /\ row /\ ~E.st.row
    /\ gone' = TRUE /\ stale' = FALSE
    /\ UNCHANGED <<fgen, held>>
    /\ Step

\* DESTROY, a crash, a restart, a start: the kernel holds no inode any
\* more; the row stays (a daemon crash keeps the database).
T_IdRun ==
    /\ \/ Ev("destroy") \/ Ev("crash") \/ Ev("restart") \/ Ev("start")
    /\ ~stale
    /\ held' = FALSE
    /\ UNCHANGED <<gone, fgen, stale>>
    /\ Step

TraceNext == T_IdReply \/ T_IdForget \/ T_IdResolve \/ T_IdGone \/ T_IdRun
=============================================================================
