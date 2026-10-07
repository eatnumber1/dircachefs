----------------------------- MODULE RevalTrace -----------------------------
(***************************************************************************)
(* Trace validation for the revalidation model (reval.tla): is a recorded *)
(* run's history of one file a behavior of the model's file F? The method *)
(* is Trace.tla's (formal/README.md, "Trace validation"): one step per    *)
(* event, the model action the event stands for, constrained by what the  *)
(* event observed; the trace is valid iff some behavior matches every     *)
(* event, and then TLC's search depth is the number of events plus one.   *)
(*                                                                         *)
(* A trace is one file's: the recorder (dcfs/testonly/trace_recorder.cc,  *)
(* made with `files`) begins it at an open that finds no shared backing   *)
(* fd (or a create), and writes a line for each open, release, flag       *)
(* change or read (FS_IOC_SETFLAGS, FS_IOC_FSSETXATTR, FS_IOC_GETFLAGS),  *)
(* SETATTR of the mode or owner, and write dcfs makes itself, and an     *)
(* "oob" line where the test changed the backing file behind dcfs's back. *)
(* Each open and release line carries the shared backing fd the code left *)
(* (whether there is one and whether it is O_RDWR, the write fd beside a  *)
(* read-only one, and the outstanding opens and writable opens), which the *)
(* model's must equal.                                                     *)
(*                                                                         *)
(* What a trace leaves free: the backing file's mode and flags (except    *)
(* that a successful SETFLAGS sets the flags it names, and nothing changes *)
(* them between events but an "oob" line), dcfs's cached attributes, the  *)
(* kernel's cached attributes and every permission decision taken from    *)
(* them (no event observes them: the model's mode part is model-only),    *)
(* and the directory D (no event of D is mapped here).                    *)
(***************************************************************************)
EXTENDS reval, Json, IOUtils, Naturals, Sequences, FiniteSets, TLC

Log == ndJsonDeserialize(IOEnv.DCFS_TRACE)
Events == SubSeq(Log, 2, Len(Log))
NumEvents == Len(Log) - 1

Has(e, f) == f \in DOMAIN e

\* One model handle per open in the trace (at least one).
NumOpens == Cardinality({i \in DOMAIN Events : Events[i].ev = "open"})
TraceHandles == 1..(IF NumOpens = 0 THEN 1 ELSE NumOpens)
\* A bound the trace cannot exceed (no step of a trace counts changes).
TraceMaxChanges == Len(Log)

VARIABLE l

E == Events[l]
Ev(name) == l <= NumEvents /\ E.ev = name

\* The shared backing fd the code left equals the model's.
FdMatches(fd) ==
    /\ sfd'.st = fd.sfd
    /\ wfd' = fd.wfd
    /\ Cardinality({h \in Handles : hs'[h].st = "open"}) = fd.refs
    /\ Cardinality({h \in Handles : hs'[h].st = "open" /\ Writes(hs'[h].m)})
         = fd.wrefs

\* The next event; nothing in a trace observes D or counts changes.
Step == l' = l + 1 /\ UNCHANGED <<dirVars, changes>>

\* The initial state: the model's, with D left out (nothing observes it).
TraceInit ==
    /\ Init
    /\ bD = [cf |-> FALSE, ents |-> {}]
    /\ cD = [x \in Names |-> "unknown"]
    /\ l = 1

\* An open, granted (errno 0) or refused with EPERM by the GETFLAGS
\* re-check or the backing filesystem's reopen. The kernel's own refusal
\* (default_permissions, EACCES) never reaches dcfs, so no event is one.
T_Open ==
    /\ Ev("open")
    /\ Held(sfd) = E.shared
    /\ \E h \in Handles : OpenF(h, E.mode)
    /\ lastOpen'.by # "kernel"
    /\ (E.errno = 0) = (lastOpen'.by = "granted")
    /\ FdMatches(E.fd)
    /\ Step

\* A release of an open that may write iff E.writable (dcfs's own record
\* of the open; the RELEASE's flags are not the open's).
T_Release ==
    /\ Ev("release")
    /\ \E h \in Handles : /\ hs[h].st = "open"
                          /\ Writes(hs[h].m) = E.writable
                          /\ ReleaseF(h)
    /\ FdMatches(E.fd)
    /\ Step

\* FS_IOC_SETFLAGS (the flags it set: E.imm, E.app) or FS_IOC_FSSETXATTR
\* (left free) through dcfs.
T_SetFlags ==
    /\ Ev("setflags")
    /\ IF E.errno = 0
       THEN \E imm, app \in BOOLEAN :
              /\ Has(E, "imm") => (imm = E.imm /\ app = E.app)
              /\ SetFlagsF(imm, app)
       ELSE FailedAttrChangeF
    /\ Step

T_GetFlags == Ev("getflags") /\ GetFlagsF /\ Step

\* A SETATTR of the mode or owner through dcfs: the model's chmod, its
\* outcome for the caller left free.
T_Chmod ==
    /\ Ev("chmod")
    /\ IF E.errno = 0 THEN \E v \in BOOLEAN : ChmodF(v) ELSE FailedAttrChangeF
    /\ Step

\* dcfs wrote through the file's write fd: it could (errno 0), or it had
\* none that can write (EBADF).
T_Write ==
    /\ Ev("write")
    /\ \E h \in Handles : WriteF(h)
    /\ IF E.errno = 0 THEN writeErr' = "none" ELSE writeErr' # "none"
    /\ Step

\* The test changed the backing file behind dcfs's back.
T_OutOfBand ==
    /\ Ev("oob") /\ OutOfBand
    /\ \E b \in BStates : bF' = b
    /\ UNCHANGED <<cF, kAttr, sfd, wfd, hs, lastOpen, writeErr>>
    /\ Step

TraceNext ==
    \/ T_Open \/ T_Release \/ T_SetFlags \/ T_GetFlags \/ T_Chmod \/ T_Write
    \/ T_OutOfBand
=============================================================================
