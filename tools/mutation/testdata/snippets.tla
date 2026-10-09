---------------------------- MODULE snippets ----------------------------
(* Fixture of tools/mutation/tla_operators_test.py: constructs copied from  *)
(* formal/dcfs.tla (bulleted lists, nested indentation, LET, \A and \E,     *)
(* EXCEPT, primes, UNCHANGED, tuples, Commit calls) with their comments.    *)
(* A comment with operators in it: x < y /\ z = 1 \/ Commit(a, TRUE).       *)
EXTENDS Naturals

Commit(new, sync) ==
    /\ dbCur' = new
    /\ dbOpts' = IF sync THEN {new} ELSE dbOpts \cup {new}

\* A comment line: seq <= 3 /\ inflight = 1.
CanFill(s) == BugUnguardedFills \/ (inflight = 0 /\ seq <= s)
Owns(r) == inflight = 1 /\ seq = r.mseq

Free(p) == running \in {None, p}
At(p, label) == ps[p].pc = label /\ Free(p) /\ mode = "up"

UnchangedBacking == UNCHANGED <<bCur, bSeq>>
UnchangedDB == UNCHANGED <<dbCur, dbOpts>>
UnchangedGuards == UNCHANGED <<seq, inflight, durableD, fm>>
AfterSyscall(p, r) == ps' = [ps EXCEPT ![p] = r] /\ UNCHANGED running
Then(p, r) ==
    IF r.pc = "Reply" THEN Reply(p, r, ReplyAt(r))
    ELSE ps' = [ps EXCEPT ![p] = r] /\ running' = p

RNProbe(p) ==
    /\ At(p, "RN_probe")
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "RN_commit", !.snap = seq])
    /\ UnchangedBacking /\ UnchangedGuards

RNCommit(p) ==
    /\ At(p, "RN_commit")
    /\ LET r == ps[p]
           n == r.lk
           o == r.rdObj
       IN /\ IF CanFill(r.snap)
             THEN Commit([dbCur EXCEPT !.dent[n] = IF o = NoObj THEN Absent
                                                   ELSE o], FALSE)
             ELSE UnchangedDB
          /\ Then(p, [r EXCEPT !.pc = r.cont, !.lk = None])
    /\ UnchangedBacking /\ UnchangedGuards

BeginMutation(names, attrs) ==
    LET sync == ~durableD /\ ~BugPhase1NotDurable
    IN /\ Commit(MarkUnknown(dbCur, names, attrs), sync)
       /\ durableD' = (durableD \/ sync)
       /\ seq' = seq + 1
       /\ UNCHANGED fm

Next ==
    \/ \E p \in Procs :
         \/ Arrive(p)
         \/ LookupStep(p) \/ ResolveProbe(p) \/ ResolveCommit(p)
         \/ SyncClearDirty(p)
    \/ FRead \/ AtimeExpiry
    \/ Restart \/ Recover

Safe ==
    /\ \A x \in Names : dbCur.dent[x] \in Objs \cup {Unknown, Absent, NoRow}
    /\ \E k \in 1..Len(bSeq) : bSeq[k] = bCur
    /\ Reorder \in {"seq", "metaprefix", "ext4"}
    /\ servedWrong = FALSE

Pair == <<bCur, bSeq>>
Frame == UNCHANGED <<seq, inflight, servedWrong>>
Count(i) == i + 1 + 2
Ranges == 1..Len(bSeq)
Bump == seq' = seq + 1 /\ inflight' = inflight - 1
=============================================================================
