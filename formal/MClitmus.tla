------------------------------ MODULE MClitmus ------------------------------
(***************************************************************************)
(* Litmus tests of the backing filesystem's crash consistency through dcfs *)
(* (step 12.8), after Ferrite's (Bornholt et al., ASPLOS 2016, sections    *)
(* 3.4 and 3.5) and the directory fsync that step 11.2's ACE-style tests   *)
(* ran into. Each is a fixed program (Script) run through dcfs from a      *)
(* fixed directory (InitPresent), with a surprising outcome (LitmusBad, a  *)
(* predicate on what is visible after a power loss: D's entries and the    *)
(* files' contents). Two invariants ask whether a power loss now could     *)
(* leave that outcome:                                                     *)
(*                                                                         *)
(*   LitmusOK         served through dcfs: what dcfs serves once recovery  *)
(*                    has run (the recovered cache where it knows a name,  *)
(*                    else the backing filesystem; contents always from    *)
(*                    the backing file);                                   *)
(*   LitmusBackingOK  the backing filesystem itself.                       *)
(*                                                                         *)
(* By CrashSafe (dcfs.tla: once recovery has run, what dcfs serves of D is *)
(* the backing's own state) the first can fail only where the second does. *)
(* (CrashRefines alone would not give that under "seq", where names and    *)
(* data come from one state.) The configurations (MC_litmus_*.cfg,         *)
(* limitations/litmus_*.cfg) say, for each regime, which outcome is        *)
(* expected; README.md, "The backing filesystem's crash consistency", has  *)
(* the table.                                                              *)
(*                                                                         *)
(* The predicates are about the run in which the program ran: they hold    *)
(* vacuously once a crash has happened (MaxCrashes = 1 lets the model's    *)
(* recovery run after the program too, under every invariant of dcfs).     *)
(***************************************************************************)
EXTENDS MC

\* A program step: a request kind and its names.
Step(k, n, m) == [k |-> k, n |-> n, m |-> m]

\* Atomic replace via rename (ARVR) and atomic create via rename (ACVR):
\* create t, write t's contents, rename t over f (f holds old contents in
\* ARVR, does not exist in ACVR).
ViaRename == <<Step("create", "t", None), Step("write", "t", None),
               Step("rename", "t", "f")>>
\* Implied directory fsync: create f, write it, fsync it (FSYNC).
FsyncFile == <<Step("create", "f", None), Step("write", "f", None),
               Step("fsync", "f", None)>>
\* Directory fsync (step 11.2's ACE "direct" kind as first written): create
\* f, write it, fsync D (FSYNCDIR).
FsyncDir == <<Step("create", "f", None), Step("write", "f", None),
              Step("sync", None, None)>>

\* The program a configuration runs (Script <- ...).
Script == ViaRename

\* The script's steps run in order, one at a time (one request slot). The
\* mutations (create, write, rename) are counted by `muts`, so the next one
\* is Script[muts + 1]; a sync point (sync, fsync) comes only last, once
\* every mutation has run (and may come again: it changes nothing more).
ScriptAllows(k, n, m) ==
    IF k \in SyncKinds
    THEN /\ muts = Len(Script) - 1
         /\ Script[Len(Script)] = Step(k, n, m)
    ELSE /\ muts < Len(Script)
         /\ Script[muts + 1] = Step(k, n, m)

\* Where the programs start: f with its old contents (ARVR), or nothing.
WithF == {{"f"}}
Empty == {{}}

\* The program's sync point has replied (Ferrite's mark: the caller saw the
\* fsync return).
Marked == \E p \in Procs : ps[p].rep.k \in SyncKinds

\* What is visible: D's entries and the files' contents.
NoFile(v, n) == v.names[n] = NoObj
NoData(v, n) == ~NoFile(v, n) /\ v.data[v.names[n]] = 0

\* The surprising outcomes (Ferrite's "exists?" predicates; contents are
\* written whole, so "neither the old contents nor the new" is "absent or
\* empty", and "not the data" is "empty").
\* ARVR: f holds neither its old contents nor the new ones.
BadReplace(v) == NoFile(v, "f") \/ NoData(v, "f")
\* ACVR: f exists but without the data.
BadCreate(v) == NoData(v, "f")
\* Implied directory fsync, directory fsync: the fsync returned, and f is
\* absent or without the data it was written.
BadAfterFsync(v) == Marked /\ (NoFile(v, "f") \/ NoData(v, "f"))

LitmusBad(v) == BadReplace(v)

\* What a power loss now may leave visible: through dcfs (after recovery)
\* and on the backing filesystem.
ServedAfterPowerLoss ==
    {[names |-> Observed(RecoverDirty(s), t).names, data |-> t.data] :
       s \in dbOpts, t \in BCrash}
BackingAfterPowerLoss == {[names |-> t.names, data |-> t.data] : t \in BCrash}

LitmusOK ==
    (mode = "up" /\ crashes = 0) => \A v \in ServedAfterPowerLoss : ~LitmusBad(v)
LitmusBackingOK ==
    (mode = "up" /\ crashes = 0) => \A v \in BackingAfterPowerLoss : ~LitmusBad(v)
=============================================================================
