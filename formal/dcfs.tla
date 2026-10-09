-------------------------------- MODULE dcfs --------------------------------
(***************************************************************************)
(* A model of dcfs's write-through protocol (docs/design.md, "The          *)
(* write-through protocol", "Crashes, power loss and recovery",            *)
(* "Concurrency, today and with coroutines").                              *)
(*                                                                         *)
(* What is modelled: one cached directory D (the root) whose entries are   *)
(* the names in `Names`; the backing filesystem's view of D (name ->       *)
(* object, and D's own attributes, abstracted to a version stamp); the     *)
(* cache database's view of D (one dentry row per name, the completeness   *)
(* flag and its epoch, D's cached attributes, D's row in the dirty set,    *)
(* the clean-shutdown flag); for both disks, which of their states a crash *)
(* may leave; and the daemon's in-memory state (fill guards, the durable   *)
(* part of the dirty set, requests in flight).                             *)
(*                                                                         *)
(* Requests run concurrently, interleaving at every backing syscall: the   *)
(* coroutine architecture the design prepares for. A request's code        *)
(* between two syscalls runs without interruption by other requests (as    *)
(* it does on one thread with synchronous SQLite), but a crash can happen  *)
(* between any two of its transactions.                                    *)
(*                                                                         *)
(* formal/README.md explains every variable and action, names the dcfs     *)
(* function each one stands for, and lists the abstractions.               *)
(*                                                                         *)
(* The Bug* constants re-introduce one historical bug each (see            *)
(* docs/plan/audits/ and formal/README.md); every real configuration sets  *)
(* them all FALSE. They exist so that formal/known_bugs/ can show the      *)
(* model is fine-grained enough to find those bugs.                        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
    Names,          \* the names in D, e.g. {"a", "b"}
    Procs,          \* request slots: at most this many requests in flight
    Requests,       \* the request kinds the model issues (subset of AllKinds)
    MaxMutations,   \* bound: mutations started over a whole behavior
    MaxCrashes,     \* bound: crashes over a whole behavior
    KernelDirLock,  \* the kernel serializes D's namespace ops, lookups, readdirs
    Interrupts,     \* FUSE_INTERRUPT is modelled (Phase 22: Interrupt)
    BugPhase1NotDurable,        \* crash F1
    BugCreateKeepsParentAttrs,  \* crash F3
    BugUnguardedFills,          \* tri-state F1
    BugRestoreComplete,         \* tri-state F4
    BugSyncIgnoresMutations,    \* finding sync_during_mutation (R4)
    BugReaddirplusUnlocked,     \* finding readdirplus_unlocked (R4)
    BugRenameStaleSource,       \* finding rename_stale_source (R4)
    \* Interrupts (Phase 22; none historical, known_bugs/interrupt_*):
    BugInterruptAfterSyscall,   \* interruptible after the backing syscall
    BugInterruptUndo,           \* an interrupt puts the resolved name back
    BugInterruptLeaksGuard      \* an interrupted mutation never Ends

AllKinds == {"lookup", "readdir", "readdirplus", "getattr",
             "create", "linkcreate", "unlink", "rename", "attrchange", "sync"}
\* The requests of a regular file F (step 23.8; "Access times of a file"
\* below). Not in AllKinds: the configurations that leave them out keep
\* F's state constant, and their state counts.
FileKinds == {"fopen", "frelease", "fgetattr", "fset"}

ASSUME /\ Names # {} /\ IsFiniteSet(Names)
       /\ Procs # {} /\ IsFiniteSet(Procs)
       /\ Requests \subseteq AllKinds \cup FileKinds
       /\ MaxMutations \in Nat /\ MaxCrashes \in Nat
       /\ \A b \in {KernelDirLock, Interrupts, BugPhase1NotDurable,
                    BugCreateKeepsParentAttrs, BugUnguardedFills,
                    BugRestoreComplete, BugSyncIgnoresMutations,
                    BugReaddirplusUnlocked, BugRenameStaleSource,
                    BugInterruptAfterSyscall, BugInterruptUndo,
                    BugInterruptLeaksGuard} :
              b \in BOOLEAN

-----------------------------------------------------------------------------
(* Values *)

NoObj   == "-"        \* backing: no object under this name
NoRow   == "none"     \* cache: no dentry row (read through `complete`)
Unknown == "unknown"  \* cache: dentry row in state 'unknown'
Absent  == "absent"   \* cache: dentry row in state 'absent' (negative entry)
None    == "_"        \* an unused name or label

NumNames == Cardinality(Names)
NameOrder == CHOOSE f \in [1..NumNames -> Names] :
                 \A i, j \in 1..NumNames : i # j => f[i] # f[j]
\* Backing objects are identities (inode number + generation): never reused.
\* Objects o1..oN can exist initially (one per name); every create makes the
\* next fresh one, o(N+1), o(N+2), ...
Obj(i) == "o" \o ToString(i)
InitObj(n) == Obj(CHOOSE i \in 1..NumNames : NameOrder[i] = n)
MaxStamp == NumNames + MaxMutations
Objs == {Obj(i) : i \in 1..MaxStamp}
\* A dentry row: present (the object), absent, unknown, or no row at all.
DentVals == Objs \cup {NoRow, Unknown, Absent}

\* A state of the backing filesystem: D's entries, and D's attributes
\* (mtime/ctime/nlink), abstracted to the stamp of the last change to D;
\* and F's attributes (its access time above all), the stamp of the last
\* change to F (a read through passthrough, or a setattr).
BStates == [names : [Names -> Objs \cup {NoObj}], ver : 0..MaxStamp,
            f : 0..MaxStamp]
\* F's row in the `dirty` table: none, atime_only = 1, or a mutation's.
FDirtyVals == {"no", "atime", "mut"}
\* A state of the cache database, as far as D (and F) is concerned.
DBStates == [dent : [Names -> DentVals],  \* dentries rows (parent = D)
             complete : BOOLEAN,          \* directories.children_complete
             epoch : Nat,                 \* directories.epoch
             attrValid : BOOLEAN,         \* inodes.attrs_valid of D
             attr : 0..MaxStamp,          \* D's cached attributes (0 while
                                          \* unknown: the stale hint the code
                                          \* keeps is never served)
             dirty : BOOLEAN,             \* D is in the `dirty` table
             clean : BOOLEAN,             \* cache_state.clean_shutdown
             fValid : BOOLEAN,            \* F's inodes.attrs_valid
             fAttr : 0..MaxStamp,         \* F's cached attributes (0 while
                                          \* unknown)
             fDirty : FDirtyVals]         \* F's `dirty` row: none, or its
                                          \* atime_only reason

\* What a request answered: an entry (found/neg), a listing, attributes.
NoRes == [k |-> "none", name |-> None, o |-> NoObj, s |-> {}, v |-> 0]

(* What a request replies (the reply ghost, step 12.7b): an errno class    *)
(* and the answers the reply carries, a set of answers as above (a         *)
(* lookup's entry or negative entry; a listing; D's attributes; a          *)
(* readdirplus carries a listing and D's attributes; a mutation, a sync    *)
(* or an error carries none), and the kind of the request that replied     *)
(* (`k`, set by Reply). The classes are the errnos the model's             *)
(* requests reply: "ok" (a negative entry is a successful reply),          *)
(* "ENOENT", "EEXIST", "EAGAIN" and "EINTR". Every other errno             *)
(* (ENOTEMPTY, EACCES, EIO, ...) is one the model never replies; a trace   *)
(* with one is cut there (README, "Trace validation").                     *)
Errnos == {"ok", "ENOENT", "EEXIST", "EAGAIN", "EINTR"}
Rep(e, a) == [e |-> e, a |-> a, k |-> None]
NoRep == Rep(None, {})

Modes == {"up", "down", "recover", "start", "probe",
          "stop_sync", "stop_clear", "stop_ckpt", "stop_flag"}

-----------------------------------------------------------------------------
VARIABLES
    bCur,        \* the backing filesystem as the kernel sees it now
    bOpts,       \* backing states a crash may leave: the last synced one and
                 \* every later one (some prefix of the unsynced writes)
    dbCur,       \* the cache database as the daemon sees it now
    dbOpts,      \* database states a crash may leave: the last fsynced
                 \* commit and every later commit (a prefix of the WAL)
    mode,        \* the daemon: "up" serving, "down", or a start/stop step
    seq,         \* FillGuards::seq (D is the only inode tracked: README)
    inflight,    \* FillGuards::inflight[D]
    durableD,    \* D \in Context::dirty.durable
    running,     \* the request running code between two syscalls, or None
    ps,          \* per request slot: what it is doing (see IdleProc)
    servedWrong, \* history: some answer served from the cache was wrong
    fm,          \* F in the daemon's memory and the kernel (see NoF)
    stamp,       \* next fresh object / attribute stamp (history)
    muts,        \* mutations started so far (for the MaxMutations bound)
    crashes      \* crashes so far (for the MaxCrashes bound)

vars == <<bCur, bOpts, dbCur, dbOpts, mode, seq, inflight, durableD,
          running, ps, servedWrong, fm, stamp, muts, crashes>>

\* F (step 23.8): `opens`, the kernel's open files of F (only through one
\* can it read F); `held`, dcfs's opens of F (BackingFile::refs: dcfs holds
\* F's backing descriptor while it is above 0; the kernel's RELEASE comes
\* once its file is gone, so `opens` drops first); `seq`/`inflight`,
\* FillGuards::touched[F] and inflight[F]; `durable`, F in
\* Context::dirty.durable; `lost`, history: a power loss left F's cached
\* attributes behind the backing filesystem's (README: "Access times of a
\* file"), until the next record of them.
NoF == [opens |-> 0, held |-> 0, seq |-> 0, inflight |-> 0,
         durable |-> FALSE, lost |-> FALSE]

\* A request slot's local state. `pc` is where its code is; `locked` whether
\* it holds the kernel's lock on D; `lk`/`cont` are the name and the
\* continuation of a LookupOrPopulate in progress, `res` its answer; `snap`/
\* `esnap` a fill's FillSnapshot and D's epoch at that time (`snap`: also a
\* sync point's BeginSync snapshot); `rd*` what a
\* syscall read; `mseq` the seq of the mutation's phase 1 (Mutation::Owns);
\* `src` a rename's resolved source, `rsnap` the FillSnapshot a rename or
\* unlink took before it resolved its names; `attempts` how many times a
\* readdir populated or a rename's or unlink's phase 1 found its resolve
\* stale; `was` the pre-phase-1
\* completeness (BugRestoreComplete only). Fields are reset once used, so
\* that slots doing the same thing are the same state.
\*
\* For ReplyObservable (step 12.7b; ghosts, which no step of the code
\* reads): `eff` what the backing filesystem answered at the request's
\* effect point, its mutation's syscall ("ok", "EEXIST", "ENOENT"; None
\* before it); `win` the answers the backing gave to the request's queries
\* (BAns) at earlier instants since its call, that it no longer gives
\* (Remember); and, on an idle slot, `rep` the reply of the last request
\* that held it, and `rb`, flipped at every reply. A readdirplus also keeps
\* the listing it replies in `res` across its statx.
IdleProc == [pc |-> "idle", kind |-> None, n |-> None, m |-> None,
             locked |-> FALSE, lk |-> None, cont |-> None, res |-> NoRes,
             snap |-> 0, esnap |-> 0, rdObj |-> NoObj,
             rdList |-> [x \in Names |-> NoObj], rdVer |-> 0,
             mseq |-> 0, src |-> NoObj, rsnap |-> 0, attempts |-> 0,
             was |-> FALSE, eff |-> None, win |-> {}, rep |-> NoRep,
             rb |-> FALSE, fsnap |-> 0, fheld |-> FALSE, frel |-> 0,
             fdirty |-> FALSE]

TypeOK ==
    /\ bCur \in BStates /\ bOpts \subseteq BStates /\ bCur \in bOpts
    /\ dbCur \in DBStates /\ dbOpts \subseteq DBStates /\ dbCur \in dbOpts
    /\ mode \in Modes
    /\ seq \in Nat /\ inflight \in Nat /\ durableD \in BOOLEAN
    /\ running \in Procs \cup {None}
    /\ \A p \in Procs : ps[p].locked \in BOOLEAN
    /\ Cardinality({p \in Procs : ps[p].locked}) <= 1
    /\ \A p \in Procs : /\ ps[p].eff \in Errnos \cup {None}
                       /\ ps[p].rep.e \in Errnos \cup {None}
    /\ servedWrong \in BOOLEAN
    /\ fm \in [opens : Nat, held : Nat, seq : Nat, inflight : Nat,
              durable : BOOLEAN, lost : BOOLEAN]
    /\ fm.opens <= fm.held
    /\ stamp \in 1..(MaxStamp + 1) /\ muts \in 0..MaxMutations
    /\ crashes \in 0..MaxCrashes

-----------------------------------------------------------------------------
(* Reading the cache: cache::Lookup's three-way answer. *)

\* A name with no row is absent if the listing is complete, else unknown.
ReadState(d, n) ==
    IF d.dent[n] = NoRow THEN (IF d.complete THEN Absent ELSE Unknown)
    ELSE d.dent[n]

\* cache::IsDirComplete: complete, and no row is unknown.
DirListable(d) == d.complete /\ \A x \in Names : d.dent[x] # Unknown

\* cache::ListDir: the present rows.
Listing(d) == {x \in Names : d.dent[x] \in Objs}

FoundOrNeg(n, o) ==
    IF o = NoObj THEN [NoRes EXCEPT !.k = "neg", !.name = n]
    ELSE [NoRes EXCEPT !.k = "found", !.name = n, !.o = o]
ListRes(s) == [NoRes EXCEPT !.k = "list", !.s = s]
AttrRes(v) == [NoRes EXCEPT !.k = "attr", !.v = v]

\* Whether answer r agrees with backing state b.
AnswerOK(r, b) ==
    CASE r.k = "found" -> b.names[r.name] = r.o
      [] r.k = "neg"   -> b.names[r.name] = NoObj
      [] r.k = "list"  -> r.s = {x \in Names : b.names[x] # NoObj}
      [] r.k = "attr"  -> r.v = b.ver
      [] OTHER         -> TRUE

\* The names backing state b has.
BListing(b) == {x \in Names : b.names[x] # NoObj}

\* The answers backing state b gives to the queries a request (slot state
\* r) can still reply an answer to: a lookup's name; an unlink's or a
\* rename's (source) name until its phase 1 or syscall (its reply is then
\* the syscall's, or EAGAIN/EINTR); D's attributes for a getattr; the
\* listing for a readdir; both for a readdirplus. None for a create or a
\* sync, which reply no answer.
BAns(r, b) ==
    CASE r.kind = "lookup" -> {FoundOrNeg(r.n, b.names[r.n])}
      [] r.kind \in {"unlink", "rename"} /\ r.mseq = 0 /\ r.eff = None ->
           {FoundOrNeg(r.n, b.names[r.n])}
      [] r.kind = "getattr" -> {AttrRes(b.ver)}
      [] r.kind = "readdir" -> {ListRes(BListing(b))}
      [] r.kind = "readdirplus" -> {ListRes(BListing(b)), AttrRes(b.ver)}
      [] OTHER -> {}

(* Writing: a commit appends to the WAL. A kSync commit (phase 1, startup,  *)
(* shutdown) fsyncs it, so every earlier commit is durable too; a normal    *)
(* commit may or may not survive a power loss.                              *)
Commit(new, sync) ==
    /\ dbCur' = new
    /\ dbOpts' = IF sync THEN {new} ELSE dbOpts \cup {new}

\* A backing syscall that changes D: durable only after a later syncfs.
BWrite(new) == bCur' = new /\ bOpts' = bOpts \cup {new}

\* The backing filesystem changes from bCur to b2: request r remembers the
\* answers its queries had until now and no longer have (ReplyObservable:
\* the answers its reply may carry are those, and the ones they have now).
Remember(r, b2) == [r EXCEPT !.win = r.win \cup (BAns(r, bCur) \ BAns(r, b2))]

(* The fill guard (cache::CanFill) and mutation ownership                    *)
(* (cache::Mutation::Owns), for D. Every phase 1 and every end of a         *)
(* mutation touches D here (every modelled mutation names D), so            *)
(* FillGuards::touched[D] is always `seq` and "touched[D] <= snapshot" is   *)
(* "seq <= snapshot".                                                       *)
CanFill(s) == BugUnguardedFills \/ (inflight = 0 /\ seq <= s)
Owns(r) == inflight = 1 /\ seq = r.mseq

-----------------------------------------------------------------------------
(* Request steps. A step runs only while no other request is between two  *)
(* syscalls (`running`): a step that ends just before a syscall releases   *)
(* it, a step followed directly by more code keeps it. Syscall steps touch *)
(* only the backing filesystem and the slot's own state, and commute with  *)
(* every other request's code, so letting them wait for `running` too     *)
(* loses no behavior. (Two of them, ResolveProbe and PopulateRead, also    *)
(* take the fill snapshot the code takes just before the syscall, at the   *)
(* start of a stretch of code; between other requests' stretches is where  *)
(* that happens.)                                                          *)
(*                                                                         *)
(* Each step is written as XFrom(p, r): what slot p does with local state *)
(* r. X(p) applies it to the slot's current state; Arrive applies the     *)
(* first step of a request to a fresh one. The frame conditions for       *)
(* `mode`, `muts` and `crashes` are left to those callers.                *)

Free(p) == running \in {None, p}
At(p, label) == ps[p].pc = label /\ Free(p) /\ mode = "up"

\* The idle slot after request r (in slot state old) replied v: it keeps
\* the reply with r's kind (`rep`) and flips `rb`, so that a reply is
\* always a step that changes the state (ReplyObservable checks it).
\* (MC_liveness.cfg, which checks no reply property and has no VIEW to
\* merge idle slots, turns the reply ghost off with ForgetReply.)
RecordReply(old, r, v) ==
    [IdleProc EXCEPT !.rep = [v EXCEPT !.k = r.kind], !.rb = ~old.rb]
ForgetReply(old, r, v) == IdleProc
\* Request r (slot p's state, or the request arriving in it) replies v and
\* is done (releasing the kernel's lock if held).
Reply(p, r, v) ==
    /\ ps' = [ps EXCEPT ![p] = RecordReply(ps[p], r, v)]
    /\ running' = None

\* What a failed mutation replies, after re-resolving its names: the error
\* its syscall returned. (known_bugs/reply_after_failed_syscall overrides
\* it with FailedReplyOK.)
FailedReply(r) == Rep(r.eff, {})
\* Not the code: a failed mutation that replies success.
FailedReplyOK(r) == Rep("ok", {})

\* The reply at the end of a LookupOrPopulate whose continuation is the
\* reply: a lookup's answer, or a failed mutation's error.
ReplyAt(r) == IF r.kind = "lookup" THEN Rep("ok", {r.res}) ELSE FailedReply(r)

\* More code follows directly (or the reply, if r.pc = "Reply").
Then(p, r) ==
    IF r.pc = "Reply" THEN Reply(p, r, ReplyAt(r))
    ELSE ps' = [ps EXCEPT ![p] = r] /\ running' = p
\* A syscall follows.
Syscall(p, r) == ps' = [ps EXCEPT ![p] = r] /\ running' = None
\* This step was a syscall.
AfterSyscall(p, r) == ps' = [ps EXCEPT ![p] = r] /\ UNCHANGED running
\* This step was a mutation's syscall, which changed D to b2; every other
\* request in flight remembers what its queries answered before.
MutSyscall(p, r, b2) ==
    /\ BWrite(b2)
    /\ ps' = [q \in Procs |-> IF q = p THEN r ELSE Remember(ps[q], b2)]
    /\ UNCHANGED running

\* Serving answer r from the cache: record whether it was wrong.
Serve(r) == servedWrong' = (servedWrong \/ ~AnswerOK(r, bCur))
NoServe == UNCHANGED servedWrong

UnchangedBacking == UNCHANGED <<bCur, bOpts>>
UnchangedDB == UNCHANGED <<dbCur, dbOpts>>
UnchangedGuards == UNCHANGED <<seq, inflight, durableD, fm>>

(***************************************************************************)
(* backing::LookupOrPopulate(D, r.lk), then continue at r.cont with the   *)
(* answer in `res`.                                                       *)
(***************************************************************************)

\* The cache lookup. A known answer is served from the cache; an unknown
\* name is resolved alone (backing::ResolveName) if the listing is
\* complete, else the directory is listed (backing::PopulateDirectory).
\* Both begin with OpenNode(D), a syscall.
LKFrom(p, r) ==
    /\ LET n == r.lk
           v == dbCur.dent[n]
           ans == FoundOrNeg(n, IF v \in Objs THEN v ELSE NoObj)
       IN IF v \in Objs \/ v = Absent \/ (v = NoRow /\ dbCur.complete)
          THEN /\ Serve(ans)
               /\ Then(p, [r EXCEPT !.pc = r.cont, !.res = ans,
                                    !.lk = None, !.cont = None])
          ELSE /\ NoServe
               /\ Syscall(p, [r EXCEPT !.pc = IF dbCur.complete
                                               THEN "RN_probe"
                                               ELSE "PD_read"])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED stamp

\* ResolveName: the fill snapshot (cache::BeginFill, after OpenNode), and
\* the probe of the name (openat(D, name, O_PATH), statx, ...).
RNProbe(p) ==
    /\ At(p, "RN_probe")
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "RN_commit", !.snap = seq,
                                     !.rdObj = bCur.names[ps[p].lk]])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* ResolveName: record the name (present or absent) if CanFill(D), and
\* answer from what the probe read either way.
RNCommit(p) ==
    /\ At(p, "RN_commit")
    /\ LET r == ps[p]
           n == r.lk
           o == r.rdObj
       IN /\ IF CanFill(r.snap)
             THEN Commit([dbCur EXCEPT !.dent[n] = IF o = NoObj THEN Absent
                                                   ELSE o], FALSE)
             ELSE UnchangedDB
          /\ Then(p, [r EXCEPT !.pc = r.cont, !.res = FoundOrNeg(n, o),
                               !.lk = None, !.cont = None, !.snap = 0,
                               !.rdObj = NoObj])
    /\ UnchangedBacking /\ UnchangedGuards /\ UNCHANGED <<servedWrong, stamp>>

\* PopulateDirectory: the fill snapshot and D's epoch (after OpenNode),
\* and phase A: getdents64 and a probe of every child.
PDRead(p) ==
    /\ At(p, "PD_read")
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "PD_commit", !.snap = seq,
                                     !.esnap = dbCur.epoch,
                                     !.rdList = bCur.names])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* What LookupOrPopulate answers for name n when its population could not
\* be recorded: what the listing l read. (known_bugs/
\* reply_unknown_as_negative overrides it with UnknownAsNegative.)
UnrecordedAnswer(n, l) == FoundOrNeg(n, l[n])
\* Not the code: the cache's answer instead, an unknown name taken for a
\* negative one.
UnknownAsNegative(n, l) ==
    FoundOrNeg(n, IF dbCur.dent[n] \in Objs THEN dbCur.dent[n] ELSE NoObj)

\* PopulateDirectory phase B: if CanFill(D) and the epoch did not move,
\* link every listed child, prune every other row (PruneDentriesNotIn) and
\* mark D complete, in one transaction. Then LookupOrPopulate answers from
\* the cache if the listing was recorded, else from the listing itself;
\* readdir (ListCached) checks again or retries.
PDCommit(p) ==
    /\ At(p, "PD_commit")
    /\ LET r == ps[p]
           l == r.rdList
           ok == CanFill(r.snap) /\ dbCur.epoch = r.esnap
           new == [dbCur EXCEPT !.dent = [x \in Names |->
                                            IF l[x] # NoObj THEN l[x]
                                            ELSE NoRow],
                                !.complete = TRUE]
           n == r.lk
           cached == FoundOrNeg(n, IF new.dent[n] \in Objs THEN new.dent[n]
                                   ELSE NoObj)
           r2 == [r EXCEPT !.snap = 0, !.esnap = 0,
                           !.rdList = [x \in Names |-> NoObj]]
       IN /\ IF ok THEN Commit(new, FALSE) ELSE UnchangedDB
          /\ IF r.kind \in {"readdir", "readdirplus"}
             THEN NoServe /\ Then(p, [r2 EXCEPT !.pc = "RD"])
             ELSE IF ok
             THEN /\ Serve(cached)
                  /\ Then(p, [r2 EXCEPT !.pc = r.cont, !.res = cached,
                                        !.lk = None, !.cont = None])
             ELSE /\ NoServe
                  /\ Then(p, [r2 EXCEPT !.pc = r.cont,
                                        !.res = UnrecordedAnswer(n, l),
                                        !.lk = None, !.cont = None])
    /\ UnchangedBacking /\ UnchangedGuards /\ UNCHANGED stamp

LK(p) == At(p, "LK") /\ LKFrom(p, ps[p])

(***************************************************************************)
(* Readdir and Readdirplus (DirCacheFS::ListCached, then "." and "..").   *)
(***************************************************************************)

\* ListCached: if the listing can be served (IsDirComplete), it is taken
\* from the cache (ListDir: the present rows) at once, before any syscall,
\* and that is the answer. Readdir then needs no syscall (ParentOf of the
\* root needs none); Readdirplus replies "." with EntryFor(D), which
\* refreshes D's attributes (BeginFill, then syscalls) if they are unknown.
\* Otherwise populate and check again, up to kAttempts = 3 times (then
\* EAGAIN). BugReaddirplusUnlocked: Readdirplus lists only after "."'s
\* refresh, without checking again (finding readdirplus_unlocked).
RDFromCode(p, r) ==
    /\ IF DirListable(dbCur)
       THEN LET list == ListRes(Listing(dbCur)) IN
            IF r.kind = "readdir" \/ dbCur.attrValid
            THEN /\ Serve(list)
                 /\ Reply(p, r, Rep("ok", IF r.kind = "readdir" THEN {list}
                                       ELSE {list, AttrRes(dbCur.attr)}))
            ELSE /\ IF BugReaddirplusUnlocked THEN NoServe ELSE Serve(list)
                 /\ Syscall(p, [r EXCEPT !.pc = "RDP_stat", !.snap = seq,
                                         !.res = IF BugReaddirplusUnlocked
                                                 THEN NoRes ELSE list])
       ELSE /\ NoServe
            /\ IF r.attempts < 3
               THEN Syscall(p, [r EXCEPT !.pc = "PD_read",
                                         !.attempts = r.attempts + 1])
               ELSE Reply(p, r, Rep("EAGAIN", {}))
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED stamp

\* (Split so that limitations/atime_limits.tla can put a stamp of D's access
\* time around it: RDFrom <- RDFromStampingAtime.)
RDFrom(p, r) == RDFromCode(p, r)

RD(p) == At(p, "RD") /\ RDFrom(p, ps[p])

\* Readdirplus: FillAttrs(D), and the reply (the listing taken before the
\* refresh). BugReaddirplusUnlocked: ListDir now, without checking
\* IsDirComplete again.
RDPFill(p) ==
    /\ At(p, "RDP_fill")
    /\ IF CanFill(ps[p].snap)
       THEN Commit([dbCur EXCEPT !.attrValid = TRUE, !.attr = ps[p].rdVer],
                   FALSE)
       ELSE UnchangedDB
    /\ IF BugReaddirplusUnlocked THEN Serve(ListRes(Listing(dbCur)))
       ELSE NoServe
    /\ Reply(p, ps[p], Rep("ok", {IF BugReaddirplusUnlocked
                           THEN ListRes(Listing(dbCur)) ELSE ps[p].res,
                           AttrRes(ps[p].rdVer)}))
    /\ UnchangedBacking /\ UnchangedGuards /\ UNCHANGED stamp

(***************************************************************************)
(* Getattr of D (DirCacheFS::Getattr -> EntryFor -> FreshAttr), and the   *)
(* refreshes of D's attributes that end every mutation.                    *)
(***************************************************************************)

\* Valid attributes are served from the cache; otherwise RefreshAttrs:
\* BeginFill, then OpenNode and statx.
GAFrom(p, r) ==
    /\ IF dbCur.attrValid
       THEN Serve(AttrRes(dbCur.attr))
            /\ Reply(p, r, Rep("ok", {AttrRes(dbCur.attr)}))
       ELSE NoServe /\ Syscall(p, [r EXCEPT !.pc = "GA_stat", !.snap = seq])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED stamp

\* A statx of D (the label says for which request step).
Stat(p, label, next) ==
    /\ At(p, label)
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = next, !.rdVer = bCur.ver])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* backing::FillAttrs(D) with the slot's snapshot, then the reply (for a
\* getattr: what the statx read, not served from the cache; for a
\* mutation: success).
FillAttrsAndReply(p, label) ==
    /\ At(p, label)
    /\ IF CanFill(ps[p].snap)
       THEN Commit([dbCur EXCEPT !.attrValid = TRUE, !.attr = ps[p].rdVer],
                   FALSE)
       ELSE UnchangedDB
    /\ Reply(p, ps[p], IF ps[p].kind = "getattr"
                THEN Rep("ok", {AttrRes(ps[p].rdVer)}) ELSE Rep("ok", {}))
    /\ UnchangedBacking /\ UnchangedGuards /\ UNCHANGED <<servedWrong, stamp>>

(***************************************************************************)
(* Mutations: phase 1 (cache::BeginMutation and the Begin* functions).    *)
(***************************************************************************)

\* The "mark unknown" writes: each named row unknown, and (if `attrs`) D's
\* attributes unknown; D into the dirty set. BugRestoreComplete restores the
\* pre-per-name-state encoding: the row deleted and the listing marked
\* incomplete (MarkIncomplete bumps the epoch).
MarkUnknown(d, names, attrs) ==
    LET d1 == [d EXCEPT !.dent = [x \in Names |->
                                    IF x \in names
                                    THEN IF BugRestoreComplete THEN NoRow
                                         ELSE Unknown
                                    ELSE d.dent[x]],
                        !.attrValid = IF attrs THEN FALSE ELSE d.attrValid,
                        !.attr = IF attrs THEN 0 ELSE d.attr,
                        !.dirty = TRUE]
    IN IF BugRestoreComplete
       THEN [d1 EXCEPT !.complete = FALSE, !.epoch = d.epoch + 1]
       ELSE d1

\* Not the code (known_bugs/effect_before_syscall): a phase 1 that records
\* a removal's outcome (the names absent) before its syscall, instead of
\* marking them unknown. A configuration puts it in with MarkUnknown <-
\* MarkAbsentEarly.
MarkAbsentEarly(d, names, attrs) ==
    [d EXCEPT !.dent = [x \in Names |-> IF x \in names THEN Absent
                                       ELSE d.dent[x]],
              !.attrValid = IF attrs THEN FALSE ELSE d.attrValid,
              !.attr = IF attrs THEN 0 ELSE d.attr,
              !.dirty = TRUE]

\* One transaction, committed durably (kSync: a WAL fsync) unless D is
\* already durably dirty since the last sync point (the fast path, which
\* commits at normal durability). Then the mutation is in flight on D
\* (RegisterMutation). The caller records mseq = seq + 1.
BeginMutation(names, attrs) ==
    LET sync == ~durableD /\ ~BugPhase1NotDurable
    IN /\ Commit(MarkUnknown(dbCur, names, attrs), sync)
       /\ durableD' = (durableD \/ sync)
       /\ seq' = seq + 1
       /\ inflight' = inflight + 1
       /\ UNCHANGED fm

\* Mutation::End: no longer in flight.
EndMutation == seq' = seq + 1 /\ inflight' = inflight - 1
               /\ UNCHANGED <<durableD, fm>>

\* BugRestoreComplete: the old phase 3's "MarkDirComplete(parent, true) if it
\* was complete before phase 1", applied to `d`.
Restore(r, d) ==
    IF BugRestoreComplete /\ r.was THEN [d EXCEPT !.complete = TRUE] ELSE d

\* The slot after phase 1.
InFlight(r, next) ==
    [r EXCEPT !.pc = next, !.mseq = seq + 1, !.res = NoRes,
              !.was = BugRestoreComplete /\ dbCur.complete]

(* Create (DirCacheFS::CreateChild; Mknod, Mkdir, Symlink, Create), and   *)
(* "linkcreate": the link of an unnamed O_TMPFILE file into D (step 23.4; *)
(* DirCacheFS::Link of a file Tmpfile made). To D the second is a create  *)
(* too: a new object appears under a name that did not exist; the only    *)
(* difference is that the code knows which object, so its phase 3 needs  *)
(* no probe (backing::RecordNewLink links the name to the file's row).    *)

\* Phase 1 (cache::BeginCreate): the name unknown, D's attributes unknown
\* (not with BugCreateKeepsParentAttrs), D dirty. Then OpenNode(D) and the
\* create syscall.
C1From(p, r) ==
    /\ BeginMutation({r.n}, ~BugCreateKeepsParentAttrs)
    /\ Syscall(p, InFlight(r, "C_sys"))
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

\* Phase 2: mkdirat/openat(O_CREAT)/... (or a linkcreate's linkat): EEXIST
\* if the name exists. A linkcreate's object is the unnamed file, new to D
\* (a fresh identity), and its phase 3 is next, knowing it.
CSys(p) ==
    /\ At(p, "C_sys")
    /\ LET n == ps[p].n IN
       IF bCur.names[n] = NoObj
       THEN /\ stamp' = stamp + 1
            /\ MutSyscall(p,
                   IF ps[p].kind = "linkcreate"
                   THEN [ps[p] EXCEPT !.pc = "C_rec", !.rdObj = Obj(stamp),
                                      !.eff = "ok"]
                   ELSE [ps[p] EXCEPT !.pc = "C_probe", !.eff = "ok"],
                   [bCur EXCEPT !.names = [@ EXCEPT ![n] = Obj(stamp)],
                               !.ver = stamp])
       ELSE /\ UNCHANGED <<bCur, bOpts, stamp>>
            /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "C_fail",
                                             !.eff = "EEXIST"])
    /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED servedWrong

\* Phase 3, backing::RecordNewChild: probe the new name.
CProbe(p) ==
    /\ At(p, "C_probe")
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "C_rec",
                                     !.rdObj = bCur.names[ps[p].n]])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* Phase 3: record the dentry if the mutation still Owns(D), in one
\* transaction; then Mutation::End; then RefreshAttrsFromFd(D) as a fill
\* (BeginFill, statx). A probe that found nothing (the name vanished)
\* fails RecordNewChild: End, and the error is replied.
CRec(p) ==
    /\ At(p, "C_rec")
    /\ LET r == ps[p]
           n == r.n
           o == r.rdObj
       IN IF o = NoObj
          THEN UnchangedDB /\ EndMutation /\ Reply(p, r, Rep("ENOENT", {}))
          ELSE /\ Commit(Restore(r, IF Owns(r)
                                    THEN [dbCur EXCEPT !.dent[n] = o]
                                    ELSE dbCur), FALSE)
               /\ EndMutation
               /\ Syscall(p, [r EXCEPT !.pc = "C_stat", !.snap = seq + 1,
                                       !.mseq = 0, !.rdObj = NoObj,
                                       !.was = FALSE])
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

\* A failed phase 2: Mutation::End, then ReresolveAfterFailure (a
\* LookupOrPopulate of the name); the syscall's error is replied.
Fail(p, label, name, cont) ==
    /\ At(p, label)
    /\ EndMutation
    /\ Then(p, [ps[p] EXCEPT !.pc = "LK", !.lk = name, !.cont = cont,
                             !.mseq = 0, !.was = FALSE])
    /\ UnchangedBacking /\ UnchangedDB /\ UNCHANGED <<servedWrong, stamp>>

(* A change of D's own attributes (step 12.11): DirCacheFS::Setattr,      *)
(* Setxattr and Removexattr (an xattr change changes D's ctime), and the   *)
(* flag-setting Ioctl (FS_IOC_SETFLAGS, FS_IOC_FSSETXATTR) of D. Phase 1   *)
(* marks D's attributes unknown and D dirty; the syscall changes them;     *)
(* Mutation::End, with no record of its own ("phase 3 is refreshes        *)
(* only"); then a refresh, as a fill. The model's attribute change never  *)
(* fails (a trace with a failed one is cut there).                         *)

\* Phase 1 (cache::BeginAttrChange, BeginXattrChange): D's attributes
\* unknown, D dirty. Then the checkpoint and the syscall.
A1From(p, r) ==
    /\ BeginMutation({}, TRUE)
    /\ Syscall(p, InFlight(r, "A_sys"))
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

\* Phase 2: chmod/utimensat/setxattr/ioctl...: D's attributes change.
ASys(p) ==
    /\ At(p, "A_sys")
    /\ stamp' = stamp + 1
    /\ MutSyscall(p, [ps[p] EXCEPT !.pc = "A3", !.eff = "ok"],
                   [bCur EXCEPT !.ver = stamp])
    /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED servedWrong

\* The end of an attribute change: Mutation::End (known_bugs/
\* attr_change_end_skipped overrides it with AttrChangeEndSkipped).
AttrChangeEnd == EndMutation
\* Not the code: an attribute change that never Ends (8.2's mutation
\* survivors: a deleted Mutation::End after copy_file_range, fallocate,
\* setattr), its guard left raised.
AttrChangeEndSkipped == UNCHANGED <<seq, inflight, durableD, fm>>

\* Mutation::End, then the refresh (RefreshAttrs, RefreshAttrsFromFd): its
\* fill snapshot, taken right after the End, then OpenNode and statx.
A3(p) ==
    /\ At(p, "A3")
    /\ AttrChangeEnd
    /\ Syscall(p, [ps[p] EXCEPT !.pc = "A_stat", !.snap = seq', !.mseq = 0])
    /\ UnchangedBacking /\ UnchangedDB /\ UNCHANGED <<servedWrong, stamp>>

(* Unlink and Rmdir (DirCacheFS::RemoveChild). *)

\* After resolving the name: ENOENT, or phase 1 (cache::BeginRemove: the
\* name and D's attributes unknown, D dirty), then the unlinkat syscall.
\* As in R1, phase 1's transaction first verifies that no mutation of D
\* began or ended since the snapshot taken before the resolve (`rsnap`),
\* and none is in flight; if one did, nothing is written and the name is
\* resolved again from a new snapshot, at most 3 times in all (then
\* EAGAIN). In the code the check protects the resolved child's records
\* (phase 1 marks them unknown, the unlinkat removes whatever the name
\* holds); child records are not modelled, so here it only keeps the
\* model's behaviors (retries, EAGAIN) those of the code.
U1(p) ==
    /\ At(p, "U1")
    /\ LET r == ps[p] IN
       IF r.res.k = "neg"
       THEN /\ Reply(p, r, Rep("ENOENT", {r.res}))
            /\ UnchangedDB /\ UnchangedGuards
       ELSE IF inflight = 0 /\ seq <= r.rsnap
       THEN /\ BeginMutation({r.n}, TRUE)
            /\ Syscall(p, [InFlight(r, "U_sys") EXCEPT !.rsnap = 0,
                                                       !.attempts = 0])
       ELSE /\ UnchangedDB /\ UnchangedGuards
            /\ IF r.attempts < 2
               THEN Then(p, [r EXCEPT !.pc = "LK", !.lk = r.n, !.cont = "U1",
                                      !.rsnap = seq, !.res = NoRes,
                                      !.attempts = r.attempts + 1])
               ELSE Reply(p, r, Rep("EAGAIN", {}))
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

\* Phase 2: unlinkat(D, name): ENOENT if the name is gone.
USys(p) ==
    /\ At(p, "U_sys")
    /\ LET n == ps[p].n IN
       IF bCur.names[n] # NoObj
       THEN /\ stamp' = stamp + 1
            /\ MutSyscall(p, [ps[p] EXCEPT !.pc = "U3", !.eff = "ok"],
                           [bCur EXCEPT !.names = [@ EXCEPT ![n] = NoObj],
                                       !.ver = stamp])
       ELSE /\ UNCHANGED <<bCur, bOpts, stamp>>
            /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "U_fail",
                                             !.eff = "ENOENT"])
    /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED servedWrong

\* Phase 3: the name absent (SetNegative) if Owns(D); End; RefreshAttrs(D)
\* as a fill (BeginFill, then OpenNode and statx).
U3(p) ==
    /\ At(p, "U3")
    /\ LET r == ps[p] IN
       /\ Commit(Restore(r, IF Owns(r)
                            THEN [dbCur EXCEPT !.dent[r.n] = Absent]
                            ELSE dbCur), FALSE)
       /\ EndMutation
       /\ Syscall(p, [r EXCEPT !.pc = "U_stat", !.snap = seq + 1, !.mseq = 0,
                               !.was = FALSE])
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

(* Rename within D (DirCacheFS::Rename, flags 0). *)

\* After resolving the source: ENOENT, or resolve the destination.
R0(p) ==
    /\ At(p, "R0")
    /\ IF ps[p].res.k = "neg"
       THEN Reply(p, ps[p], Rep("ENOENT", {ps[p].res}))
       ELSE Then(p, [ps[p] EXCEPT !.pc = "LK", !.src = ps[p].res.o,
                                  !.res = NoRes, !.lk = ps[p].m,
                                  !.cont = "R1"])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* Phase 1 (cache::BeginRename): first the verification, in its
\* transaction: no mutation of D began or ended since the snapshot taken
\* before the source was resolved (`rsnap`), and none is in flight
\* (CanFill's test). If it holds, both names and D's attributes unknown, D
\* dirty, then renameat2. If not, nothing is written and the rename
\* resolves both names again from a new snapshot, at most 3 times in all
\* (then EAGAIN). BugRenameStaleSource: no verification (finding
\* rename_stale_source).
R1(p) ==
    /\ At(p, "R1")
    /\ LET r == ps[p] IN
       IF BugRenameStaleSource \/ (inflight = 0 /\ seq <= r.rsnap)
       THEN /\ BeginMutation({r.n, r.m}, TRUE)
            /\ Syscall(p, [InFlight(r, "R_sys") EXCEPT !.rsnap = 0,
                                                       !.attempts = 0])
       ELSE /\ UnchangedDB /\ UnchangedGuards
            /\ IF r.attempts < 2
               THEN Then(p, [r EXCEPT !.pc = "LK", !.lk = r.n, !.cont = "R0",
                                      !.rsnap = seq, !.src = NoObj,
                                      !.res = NoRes,
                                      !.attempts = r.attempts + 1])
               ELSE Reply(p, r, Rep("EAGAIN", {}))
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

\* Phase 2: renameat2(D, n, D, m): moves whatever n names now over m;
\* ENOENT if n is gone.
RSys(p) ==
    /\ At(p, "R_sys")
    /\ LET n == ps[p].n
           m == ps[p].m
       IN IF bCur.names[n] # NoObj
          THEN /\ stamp' = stamp + 1
               /\ MutSyscall(p, [ps[p] EXCEPT !.pc = "R3", !.eff = "ok"],
                     [bCur EXCEPT !.names = [@ EXCEPT ![m] = bCur.names[n],
                                                     ![n] = NoObj],
                                 !.ver = stamp])
          ELSE /\ UNCHANGED <<bCur, bOpts, stamp>>
               /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "R_fail",
                                                !.eff = "ENOENT"])
    /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED servedWrong

\* Phase 3, one transaction if Owns(D): m -> the source (resolved before
\* phase 1, and verified by it), n absent. End; RefreshAfterRename's
\* RefreshAttrs(D) as a fill.
R3(p) ==
    /\ At(p, "R3")
    /\ LET r == ps[p] IN
       /\ Commit(Restore(r, IF Owns(r)
                            THEN [dbCur EXCEPT !.dent[r.m] = r.src,
                                               !.dent[r.n] = Absent]
                            ELSE dbCur), FALSE)
       /\ EndMutation
       /\ Syscall(p, [r EXCEPT !.pc = "R_stat", !.snap = seq + 1, !.mseq = 0,
                               !.src = NoObj, !.was = FALSE])
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp>>

\* After a failed rename re-resolved n: re-resolve m too.
RFail2(p) ==
    /\ At(p, "R_fail2")
    /\ Then(p, [ps[p] EXCEPT !.pc = "LK", !.lk = ps[p].m, !.cont = "Reply",
                             !.res = NoRes, !.src = NoObj])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

(***************************************************************************)
(* A sync point (backing::SyncBacking): from the periodic                 *)
(* DirCacheFS::MaybeSyncBacking, or after FSYNC/FSYNCDIR. Other requests  *)
(* run while it waits on syncfs, as they will under coroutines.           *)
(***************************************************************************)

\* cache::BeginSync (the guards' clock, in `snap`; D's row in the snapshot
\* of the dirty set is implied: D only becomes dirty through a phase 1,
\* which moves the clock), then syncfs(2): every backing write so far is
\* durable.
S1From(p, r) ==
    /\ bOpts' = {bCur}
    /\ Syscall(p, [r EXCEPT !.pc = "S2", !.snap = seq, !.fsnap = fm.seq,
                             !.fheld = fm.held > 0,
                             !.fdirty = dbCur.fDirty # "no"])
    /\ UNCHANGED bCur /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* cache::ClearDirty, at normal durability: D's row goes only if no mutation
\* of D began or ended since BeginSync and none is in flight (CanFill's
\* test, with the snapshot); otherwise its syscall may have come after the
\* syncfs started. Context::dirty.durable is cleared either way.
\* BugSyncIgnoresMutations: the old ClearDirty, which kept only writable
\* opens (finding sync_during_mutation).
\* F's row (step 23.8) goes on the same terms (it was in BeginSync's
\* snapshot of the dirty set: `fdirty`; a cold open's mark moves no clock),
\* and only if F was not open
\* (dcfs held no descriptor for it) at BeginSync nor now
\* (cache::SyncSnapshot::open_files, Context::open_files): the kernel may
\* have read it after the syncfs began. SyncKeepsHeld(r) is that test;
\* known_bugs/atime_sync_clears_held overrides it.
SyncKeepsHeld(r) == r.fheld \/ fm.held > 0
S2(p) ==
    /\ At(p, "S2")
    /\ LET r == ps[p]
           clear == BugSyncIgnoresMutations \/ (inflight = 0 /\ seq <= r.snap)
           clearF == /\ r.fdirty
                     /\ fm.inflight = 0 /\ fm.seq <= r.fsnap
                     /\ ~SyncKeepsHeld(r)
       IN Commit([dbCur EXCEPT !.dirty = IF clear THEN FALSE ELSE dbCur.dirty,
                               !.fDirty = IF clearF THEN "no" ELSE dbCur.fDirty],
                 FALSE)
    /\ durableD' = FALSE
    /\ fm' = [fm EXCEPT !.durable = FALSE]
    /\ Reply(p, ps[p], Rep("ok", {}))
    /\ UnchangedBacking /\ UNCHANGED <<seq, inflight, servedWrong, stamp>>

(***************************************************************************)
(* FUSE_INTERRUPT (Phase 22; docs/design.md, "Cancellation"). dcfs serves  *)
(* one request at a time and cannot stop a backing syscall once issued;   *)
(* it looks for an interrupt of the request it serves at checkpoints      *)
(* (Checkpoint in dcfs/interrupts.h), each just before a backing syscall: *)
(* before LookupOrPopulate's probe or population (RN_probe, PD_read),     *)
(* between the population's probe batches (PD_commit: its reads are      *)
(* abandoned, nothing is committed), and before a mutation's phase-2      *)
(* syscall (C_sys, U_sys, R_sys). There an interrupted request replies    *)
(* EINTR; a mutation past phase 1 ends (Mutation::End) without a phase 3, *)
(* its names and D's attributes left unknown and D dirty, as phase 1 left *)
(* them: the backing filesystem is unchanged, so unknown is sound. From    *)
(* the syscall through phase 3 there is no checkpoint: the change exists  *)
(* and is recorded, and the request replies success.                      *)
(*                                                                         *)
(* BugInterruptAfterSyscall: also interruptible after the syscall, before *)
(* phase 3, cancelling the mutation by putting its resolved name back.    *)
(* BugInterruptUndo: an interrupt before the syscall puts the resolved    *)
(* name back instead of leaving it unknown. BugInterruptLeaksGuard: the   *)
(* interrupted mutation never Ends (its guard stays raised).              *)
(***************************************************************************)

InterruptPcs == {"RN_probe", "PD_read", "PD_commit", "C_sys", "U_sys", "R_sys",
                 "A_sys"}
AfterSyscallPcs == {"C_probe", "C_rec", "U3", "R3", "A3"}

\* Putting a rename's source name back (the only resolved name the model
\* keeps past phase 1): what cancelling the mutation would do.
UndoRename(r, d) ==
    IF r.kind = "rename" THEN [d EXCEPT !.dent[r.n] = r.src] ELSE d

Interrupt(p) ==
    /\ Interrupts /\ mode = "up" /\ Free(p)
    /\ \/ ps[p].pc \in InterruptPcs
       \/ BugInterruptAfterSyscall /\ ps[p].pc \in AfterSyscallPcs
    /\ LET r == ps[p]
           mutating == r.mseq # 0
           undo == mutating /\ (BugInterruptUndo \/ r.pc \in AfterSyscallPcs)
       IN /\ IF mutating /\ ~BugInterruptLeaksGuard THEN EndMutation
             ELSE UnchangedGuards
          /\ IF undo THEN Commit(UndoRename(r, dbCur), FALSE) ELSE UnchangedDB
    /\ Reply(p, ps[p], Rep("EINTR", {}))
    /\ UnchangedBacking /\ UNCHANGED <<servedWrong, stamp, mode, muts, crashes>>

(***************************************************************************)
(* Access times of a file (step 23.8; docs/design.md, "Access times").     *)
(* F is a regular file: its attributes are a stamp that changes with each *)
(* read through passthrough (its access time; FRead, the kernel's: dcfs   *)
(* never sees it, and the backing filesystem writes it back lazily, so it  *)
(* is durable only after a later syncfs) and each setattr through dcfs    *)
(* (fset). While dcfs holds F's backing descriptor (fm.held > 0) its       *)
(* attributes come from a statx of it, a "held fill", which is always     *)
(* current: at the release (FR_ steps) and for a getattr (FG_). Any      *)
(* F's attributes while it is held marks its row dirty, atime only, and    *)
(* advances its guard (cache::MarkAtimeDirty), so that a sync point that   *)
(* began before keeps the row; so does a cold open, before the kernel can *)
(* read; a sync point keeps the row while F is held. An atime-only row     *)
(* does not drive a sync point (SyncDriven). The F requests interleave     *)
(* with D's and with each other at their syscalls; F has no names, and   *)
(* its guard is its own (FillGuards::touched[F], inflight[F]).            *)
(***************************************************************************)

\* A sync point runs only for a row that is not atime-only (Context::
\* dirty.any): one of D's, or F's mutation row. (With no row at all the
\* model lets one run, as it always did: it changes nothing then.)
SyncDriven == dbCur.dirty \/ dbCur.fDirty # "atime"

\* cache::CanFill for F.
CanFillF(s) == fm.inflight = 0 /\ fm.seq <= s

\* F's row marked dirty, atime only (a mutation's row stays one).
AtimeDirty(d) == IF d.fDirty = "no" THEN "atime" ELSE d.fDirty

\* Serving F's attributes from the cache: wrong if ahead of the backing
\* filesystem, or behind it with no power loss to explain it.
ServeF(v) ==
    servedWrong' = (servedWrong \/ v > bCur.f \/ (v # bCur.f /\ ~fm.lost))

UnchangedD == UNCHANGED <<seq, inflight, durableD>>

\* An F request (slot state r) replies. Its answer (F's attributes) is not
\* one of the reply ghost's (ReplyObservable checks D's); ServeF and
\* FileExact check what F's attributes are served as.
FReply(p, r) == Reply(p, r, Rep("ok", {}))

\* DirCacheFS::Open of F. A cold one (dcfs holds nothing) marks F's row
\* dirty, atime only, at normal durability, before the reply: from then on
\* the kernel may read. It records nothing, so it does not touch F's guard
\* (GuardTouch::kNone: a sync point keeps an open file's row anyway). The
\* reopen by handle is no step of its own here (nothing reads its result).
\* OpenMark is that mark; known_bugs/atime_open_not_dirty overrides it.
OpenMark ==
    /\ Commit([dbCur EXCEPT !.fDirty = AtimeDirty(dbCur)], FALSE)
    /\ fm' = [fm EXCEPT !.opens = @ + 1, !.held = @ + 1]
FOpen(p, r) ==
    /\ IF fm.held = 0
       THEN OpenMark
       ELSE UnchangedDB /\ fm' = [fm EXCEPT !.opens = @ + 1, !.held = @ + 1]
    /\ FReply(p, r)
    /\ UnchangedBacking /\ UnchangedD /\ UNCHANGED <<servedWrong, stamp>>

\* A read through one of the kernel's open files of F (passthrough): the
\* backing filesystem stamps F's access time, in memory.
FRead ==
    /\ mode = "up" /\ running = None /\ fm.opens > 0 /\ muts < MaxMutations
    /\ BWrite([bCur EXCEPT !.f = stamp])
    /\ stamp' = stamp + 1 /\ muts' = muts + 1
    /\ UNCHANGED <<dbCur, dbOpts, mode, seq, inflight, durableD, fm, running, ps,
                   servedWrong, fm, crashes>>

\* backing::FillHeldAttrs, given what its statx read (r.rdVer, its snapshot
\* r.fsnap): nothing if the row has exactly that; else, in one transaction,
\* the attributes if CanFill(F) (else unknown), and the row dirty, atime
\* only; F's guard advances (also with a mutation in flight: its phase 3
\* must not record what it read before the reads seen here). `held` is
\* what the step does to fm.held (a release ends a hold). HeldFillMark is
\* the record; known_bugs/atime_held_fill_not_dirty overrides it.
HeldFillMark(r, ok) ==
    /\ Commit([dbCur EXCEPT !.fValid = ok,
                            !.fAttr = IF ok THEN r.rdVer ELSE 0,
                            !.fDirty = AtimeDirty(dbCur)], FALSE)
    /\ fm' = [fm EXCEPT !.held = @ - r.frel, !.seq = @ + 1, !.lost = FALSE]
HeldFill(r) ==
    IF dbCur.fValid /\ dbCur.fAttr = r.rdVer
    THEN UnchangedDB /\ fm' = [fm EXCEPT !.held = @ - r.frel]
    ELSE HeldFillMark(r, CanFillF(r.fsnap))

\* backing::FillAttrs of F (a refresh by handle): if CanFill(F), the
\* attributes; if F is held then (Context::open_files), also the row dirty,
\* atime only, and F's guard advanced (cache::UpdateAttr's MarkIfOpen).
FillF(r) ==
    IF CanFillF(r.fsnap)
    THEN /\ Commit([dbCur EXCEPT !.fValid = TRUE, !.fAttr = r.rdVer,
                                 !.fDirty = IF fm.held > 0
                                            THEN AtimeDirty(dbCur)
                                            ELSE dbCur.fDirty], FALSE)
         /\ fm' = [fm EXCEPT !.seq = IF fm.held > 0 THEN @ + 1 ELSE @,
                             !.lost = FALSE]
    ELSE UnchangedDB /\ UNCHANGED fm

\* A statx of F (the label says for which step): the stamp, and F's
\* snapshot (taken in the same step: see the abstractions).
FStat(p, label, next) ==
    /\ At(p, label)
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = next, !.rdVer = bCur.f,
                                     !.fsnap = fm.seq])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards
    /\ UNCHANGED <<servedWrong, stamp>>

\* DirCacheFS::Release of F (one open): the kernel's file is gone; the held
\* fill, through the descriptor dcfs still holds; then the hold ends.
FReleaseFrom(p, r) ==
    /\ fm' = [fm EXCEPT !.opens = @ - 1]
    /\ Syscall(p, [r EXCEPT !.pc = "FR_stat", !.frel = 1])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedD
    /\ UNCHANGED <<servedWrong, stamp>>
FRFill(p) ==
    /\ At(p, "FR_fill")
    /\ HeldFill(ps[p])
    /\ FReply(p, ps[p])
    /\ UnchangedBacking /\ UnchangedD /\ UNCHANGED <<servedWrong, stamp>>

\* DirCacheFS::Getattr of F: while held, a held fill, answered from its
\* statx; else from the cache if valid; else a refresh by handle (a fill).
FGetattrFrom(p, r) ==
    /\ IF fm.held > 0
       THEN NoServe /\ Syscall(p, [r EXCEPT !.pc = "FG_stat"])
       ELSE IF dbCur.fValid
       THEN ServeF(dbCur.fAttr) /\ FReply(p, r)
       ELSE NoServe /\ Syscall(p, [r EXCEPT !.pc = "FG_rstat"])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED stamp
FGFill(p) ==
    /\ At(p, "FG_fill")
    /\ HeldFill(ps[p])
    /\ FReply(p, ps[p])
    /\ UnchangedBacking /\ UnchangedD /\ UNCHANGED <<servedWrong, stamp>>
FGRFill(p) ==
    /\ At(p, "FG_rfill")
    /\ FillF(ps[p])
    /\ FReply(p, ps[p])
    /\ UnchangedBacking /\ UnchangedD /\ UNCHANGED <<servedWrong, stamp>>

\* DirCacheFS::Setattr of F (utimensat through dcfs, say): phase 1 (F's
\* attributes unknown, its row a mutation's, committed kSync unless F is
\* durably dirty), the syscall, Mutation::End, then the refresh by handle
\* as an ordinary fill.
FSetFrom(p, r) ==
    /\ LET sync == ~fm.durable
       IN /\ Commit([dbCur EXCEPT !.fValid = FALSE, !.fAttr = 0,
                                  !.fDirty = "mut"], sync)
          /\ fm' = [fm EXCEPT !.durable = @ \/ sync, !.seq = @ + 1,
                              !.inflight = @ + 1, !.lost = FALSE]
    /\ Syscall(p, [r EXCEPT !.pc = "FS_sys"])
    /\ UnchangedBacking /\ UnchangedD /\ UNCHANGED <<servedWrong, stamp>>
FSSys(p) ==
    /\ At(p, "FS_sys")
    /\ BWrite([bCur EXCEPT !.f = stamp])
    /\ stamp' = stamp + 1
    /\ AfterSyscall(p, [ps[p] EXCEPT !.pc = "FS_end"])
    /\ UnchangedDB /\ UnchangedGuards /\ UNCHANGED servedWrong
FSEnd(p) ==
    /\ At(p, "FS_end")
    /\ fm' = [fm EXCEPT !.seq = @ + 1, !.inflight = @ - 1]
    /\ Syscall(p, [ps[p] EXCEPT !.pc = "FS_stat"])
    /\ UnchangedBacking /\ UnchangedDB /\ UnchangedD
    /\ UNCHANGED <<servedWrong, stamp>>
FSFill(p) ==
    /\ At(p, "FS_fill")
    /\ FillF(ps[p])
    /\ FReply(p, ps[p])
    /\ UnchangedBacking /\ UnchangedD /\ UNCHANGED <<servedWrong, stamp>>

(***************************************************************************)
(* A request arrives (fuse_ops.cc dispatching to DirCacheFS) and runs its *)
(* first step. With KernelDirLock, the kernel holds D's lock across       *)
(* lookups, readdirs and namespace mutations of D (i_rwsem, and FUSE's    *)
(* per-directory lock without FUSE_CAP_PARALLEL_DIROPS); not across a     *)
(* getattr or an fsync.                                                   *)
(***************************************************************************)

NewReq(kind, n, m, pc, lk, cont, locked) ==
    [IdleProc EXCEPT !.kind = kind, !.n = n, !.m = m, !.pc = pc, !.lk = lk,
                     !.cont = cont, !.locked = locked]

\* The kernel's lock on D is free (if the request needs it).
LockFree(locked) == locked => \A q \in Procs : ~ps[q].locked

Arrive(p) ==
    /\ ps[p].pc = "idle" /\ mode = "up" /\ running = None
    /\ \/ /\ "lookup" \in Requests /\ LockFree(KernelDirLock)
          /\ \E n \in Names :
               LKFrom(p, NewReq("lookup", n, None, "LK", n, "Reply",
                                KernelDirLock))
          /\ UNCHANGED muts
       \/ /\ "readdir" \in Requests /\ LockFree(KernelDirLock)
          /\ RDFrom(p, NewReq("readdir", None, None, "RD", None, None,
                              KernelDirLock))
          /\ UNCHANGED muts
       \/ /\ "readdirplus" \in Requests /\ LockFree(KernelDirLock)
          /\ RDFrom(p, NewReq("readdirplus", None, None, "RD", None, None,
                              KernelDirLock))
          /\ UNCHANGED muts
       \/ /\ "getattr" \in Requests
          /\ GAFrom(p, NewReq("getattr", None, None, None, None, None, FALSE))
          /\ UNCHANGED muts
       \/ /\ "create" \in Requests /\ LockFree(KernelDirLock)
          /\ muts < MaxMutations /\ muts' = muts + 1
          /\ \E n \in Names :
               C1From(p, NewReq("create", n, None, "C1", None, None,
                                KernelDirLock))
       \/ /\ "linkcreate" \in Requests /\ LockFree(KernelDirLock)
          /\ muts < MaxMutations /\ muts' = muts + 1
          /\ \E n \in Names :
               C1From(p, NewReq("linkcreate", n, None, "C1", None, None,
                                KernelDirLock))
       \/ /\ "unlink" \in Requests /\ LockFree(KernelDirLock)
          /\ muts < MaxMutations /\ muts' = muts + 1
          /\ \E n \in Names :
               LKFrom(p, [NewReq("unlink", n, None, "LK", n, "U1",
                                 KernelDirLock) EXCEPT !.rsnap = seq])
       \/ /\ "rename" \in Requests /\ LockFree(KernelDirLock)
          /\ muts < MaxMutations /\ muts' = muts + 1
          /\ \E n, m \in Names :
               /\ n # m
               /\ LKFrom(p, [NewReq("rename", n, m, "LK", n, "R0",
                                    KernelDirLock) EXCEPT !.rsnap = seq])
       \/ /\ "attrchange" \in Requests /\ LockFree(KernelDirLock)
          /\ muts < MaxMutations /\ muts' = muts + 1
          /\ A1From(p, NewReq("attrchange", None, None, "A1", None, None,
                              KernelDirLock))
       \/ /\ "sync" \in Requests /\ SyncDriven
          /\ S1From(p, NewReq("sync", None, None, "S1", None, None, FALSE))
          /\ UNCHANGED muts
       \/ /\ "fopen" \in Requests /\ fm.opens < Cardinality(Procs)
          /\ FOpen(p, NewReq("fopen", None, None, None, None, None, FALSE))
          /\ UNCHANGED muts
       \/ /\ "frelease" \in Requests /\ fm.opens > 0
          /\ FReleaseFrom(p, NewReq("frelease", None, None, None, None, None,
                                    FALSE))
          /\ UNCHANGED muts
       \/ /\ "fgetattr" \in Requests
          /\ FGetattrFrom(p, NewReq("fgetattr", None, None, None, None, None,
                                    FALSE))
          /\ UNCHANGED muts
       \/ /\ "fset" \in Requests
          /\ muts < MaxMutations /\ muts' = muts + 1
          /\ FSetFrom(p, NewReq("fset", None, None, None, None, None, FALSE))
    /\ UNCHANGED <<mode, crashes>>

\* The request steps by name, each with the frame for the variables only
\* the daemon's lifecycle changes. (Listed one by one in Next so that a
\* counterexample names the step.)
F == UNCHANGED <<mode, muts, crashes>>
LookupStep(p)      == LK(p) /\ F
ResolveProbe(p)    == RNProbe(p) /\ F
ResolveCommit(p)   == RNCommit(p) /\ F
PopulateRead(p)    == PDRead(p) /\ F
PopulateCommit(p)  == PDCommit(p) /\ F
ReaddirStep(p)     == RD(p) /\ F
ReaddirplusStat(p) == Stat(p, "RDP_stat", "RDP_fill") /\ F
ReaddirplusFill(p) == RDPFill(p) /\ F
GetattrStat(p)     == Stat(p, "GA_stat", "GA_fill") /\ F
GetattrFill(p)     == FillAttrsAndReply(p, "GA_fill") /\ F
CreateSyscall(p)   == CSys(p) /\ F
CreateProbe(p)     == CProbe(p) /\ F
CreatePhase3(p)    == CRec(p) /\ F
CreateStat(p)      == Stat(p, "C_stat", "C_fill") /\ F
CreateFill(p)      == FillAttrsAndReply(p, "C_fill") /\ F
CreateFailed(p)    == Fail(p, "C_fail", ps[p].n, "Reply") /\ F
UnlinkPhase1(p)    == U1(p) /\ F
UnlinkSyscall(p)   == USys(p) /\ F
UnlinkPhase3(p)    == U3(p) /\ F
UnlinkStat(p)      == Stat(p, "U_stat", "U_fill") /\ F
UnlinkFill(p)      == FillAttrsAndReply(p, "U_fill") /\ F
UnlinkFailed(p)    == Fail(p, "U_fail", ps[p].n, "Reply") /\ F
RenameResolveDst(p) == R0(p) /\ F
RenamePhase1(p)    == R1(p) /\ F
RenameSyscall(p)   == RSys(p) /\ F
RenamePhase3(p)    == R3(p) /\ F
RenameStat(p)      == Stat(p, "R_stat", "R_fill") /\ F
RenameFill(p)      == FillAttrsAndReply(p, "R_fill") /\ F
RenameFailed(p)    == Fail(p, "R_fail", ps[p].n, "R_fail2") /\ F
RenameFailed2(p)   == RFail2(p) /\ F
AttrChangeSyscall(p) == ASys(p) /\ F
AttrChangePhase3(p) == A3(p) /\ F
AttrChangeStat(p)  == Stat(p, "A_stat", "A_fill") /\ F
AttrChangeFill(p)  == FillAttrsAndReply(p, "A_fill") /\ F
SyncClearDirty(p)  == S2(p) /\ F
FileReleaseStat(p) == FStat(p, "FR_stat", "FR_fill") /\ F
FileReleaseFill(p) == FRFill(p) /\ F
FileGetattrStat(p) == FStat(p, "FG_stat", "FG_fill") /\ F
FileGetattrFill(p) == FGFill(p) /\ F
FileRefreshStat(p) == FStat(p, "FG_rstat", "FG_rfill") /\ F
FileRefreshFill(p) == FGRFill(p) /\ F
FileSetSyscall(p)  == FSSys(p) /\ F
FileSetEnd(p)      == FSEnd(p) /\ F
FileSetStat(p)     == FStat(p, "FS_stat", "FS_fill") /\ F
FileSetFill(p)     == FSFill(p) /\ F

-----------------------------------------------------------------------------
(* Crashes, startup recovery (backing::StartRun) and clean shutdown        *)
(* (backing::FinishRun).                                                   *)

\* The daemon's memory is gone: no request, no guard, no durable-set entry.
ResetMemory ==
    /\ seq' = 0 /\ inflight' = 0 /\ durableD' = FALSE /\ running' = None
    /\ ps' = [p \in Procs |-> IdleProc]

\* A crash at any moment. A kernel crash or a power loss: each disk
\* independently keeps some prefix of its unsynced writes, and that is now
\* all there is. A daemon crash: the disks keep everything (both are in the
\* kernel's page cache), but nothing more is durable than before (step
\* 12.6b): a later power loss may still lose what was not synced, so the
\* crashed run's dirty rows must stay until a sync point.
\* F in memory is gone too (the kernel's opens with the mount). `lost`
\* records whether the crash left F's cached attributes, as recovery will
\* make them, behind the backing filesystem's: only a power loss may
\* (the cold open's dirty row was not durable yet, and the backing
\* filesystem kept the reads' access time), never a daemon crash.
\* (RecoverDirty forgets F's attributes if its row is dirty.)
FBehind(d, b) == d.fDirty = "no" /\ d.fValid /\ d.fAttr # b.f
PowerLoss ==
    /\ \E s \in dbOpts, t \in bOpts :
         /\ dbCur' = s /\ dbOpts' = {s}
         /\ bCur' = t /\ bOpts' = {t}
         /\ fm' = [NoF EXCEPT !.lost = fm.lost \/ FBehind(s, t)]
DaemonCrash ==
    /\ UNCHANGED <<bCur, bOpts, dbCur, dbOpts>>
    /\ fm' = [NoF EXCEPT !.lost = fm.lost]
Crash ==
    /\ mode # "down" /\ crashes < MaxCrashes
    /\ PowerLoss \/ DaemonCrash
    /\ mode' = "down" /\ crashes' = crashes + 1
    /\ ResetMemory
    /\ UNCHANGED <<servedWrong, stamp, muts>>

\* main.cc starts the daemon again.
Restart ==
    /\ mode = "down" /\ mode' = "recover"
    /\ UNCHANGED <<bCur, bOpts, dbCur, dbOpts, seq, inflight, durableD, fm,
                   running, ps, servedWrong, stamp, muts, crashes>>

\* cache::RecoverDirty, one transaction at normal durability: for a dirty
\* D, forget every dentry of it and mark its listing incomplete (epoch
\* bump), mark its attributes unknown. As an image of what a recovery
\* makes of a crash state it is also not dirty (the code keeps the dirty
\* set until a sync point: Recover). (Recover
\* below adds what it does to dentries pointing at dirty children.) (StartRun
\* calls it whatever clean_shutdown says; with an empty dirty set it
\* changes nothing.)
\* F's row likewise, whatever its reason: its attributes unknown.
RecoverD(d) ==
    IF d.dirty
    THEN [d EXCEPT !.dent = [x \in Names |-> NoRow], !.complete = FALSE,
                   !.epoch = d.epoch + 1, !.attrValid = FALSE, !.attr = 0,
                   !.dirty = FALSE]
    ELSE d
RecoverF(d) ==
    IF d.fDirty # "no"
    THEN [d EXCEPT !.fValid = FALSE, !.fAttr = 0, !.fDirty = "no"]
    ELSE d
RecoverDirty(d) == RecoverF(RecoverD(d))

\* cache::RecoverDirty also makes unknown every dentry that points at a
\* dirty inode, wherever it is (the inode may have been renamed or
\* unlinked), even in a clean directory. Child objects are not in the
\* model's dirty set (README: abstractions), so recovery may make unknown
\* any of D's present dentries besides: those whose objects were dirty.
\* (Unknown is always safe; CrashSafe checks the least recovery forgets.)
\* RecoverForgetting(d, {}) = RecoverDirty(d): Recover's behaviours are a
\* superset of the old ones (more states reachable, none lost).
PresentNames(d) == {x \in Names : d.dent[x] \in Objs}
RecoverForgetting(d, forget) ==
    [RecoverDirty(d) EXCEPT
        !.dent = [x \in Names |-> IF x \in forget THEN Unknown
                                 ELSE RecoverDirty(d).dent[x]]]

\* The dirty set itself stays until a sync point (step 12.6b): the crashed
\* run's backing changes may not be durable yet, and the start still has to
\* probe its rows.
Recover ==
    /\ mode = "recover"
    /\ \E forget \in SUBSET PresentNames(RecoverDirty(dbCur)) :
         Commit([RecoverForgetting(dbCur, forget)
                    EXCEPT !.dirty = dbCur.dirty, !.fDirty = dbCur.fDirty],
                FALSE)
    /\ mode' = "start"
    /\ UNCHANGED <<bCur, bOpts, seq, inflight, durableD, fm, running, ps,
                   servedWrong, stamp, muts, crashes>>

\* StartRun's last transaction, kSync: clean_shutdown = 0 (and the boot id).
\* Then backing::Startup's probe (ProbesDone).
StartRun ==
    /\ mode = "start"
    /\ Commit([dbCur EXCEPT !.clean = FALSE], TRUE)
    /\ mode' = "probe"
    /\ UNCHANGED <<bCur, bOpts, seq, inflight, durableD, fm, running, ps,
                   servedWrong, stamp, muts, crashes>>

\* Not the code (known_bugs/recover_clears_dirty_first, and trace
\* validation's variant of it): RecoverDirty split into two transactions,
\* the first emptying the dirty set, the second forgetting what it was for.
\* A configuration puts it in with Recover <- RecoverClearsDirtyFirst and
\* Modes <- BugModes (the step between is mode "recover2").
BugModes == {"up", "down", "recover", "recover2", "start", "probe",
             "stop_sync", "stop_clear", "stop_ckpt", "stop_flag"}
RecoverClearsDirtyFirst ==
    \/ /\ mode = "recover" /\ dbCur.dirty
       /\ Commit([dbCur EXCEPT !.dirty = FALSE], FALSE)
       /\ mode' = "recover2"
       /\ UNCHANGED <<bCur, bOpts, seq, inflight, durableD, fm, running, ps,
                      servedWrong, stamp, muts, crashes>>
    \/ /\ mode = "recover" /\ ~dbCur.dirty
       /\ mode' = "start"
       /\ UNCHANGED <<bCur, bOpts, dbCur, dbOpts, seq, inflight, durableD,
                      fm, running, ps, servedWrong, stamp, muts, crashes>>
    \/ /\ mode = "recover2"
       /\ LET d == [dbCur EXCEPT !.dirty = TRUE] IN
            \E forget \in SUBSET PresentNames(RecoverDirty(d)) :
              Commit(RecoverForgetting(d, forget), FALSE)
       /\ mode' = "start"
       /\ UNCHANGED <<bCur, bOpts, seq, inflight, durableD, fm, running, ps,
                      servedWrong, stamp, muts, crashes>>

\* backing::Startup, after InitRoot: the probe of the recovered rows
\* (ProbeRecoveredRows, not modelled for D) ends; the rows stay dirty until
\* a sync point. Then the daemon serves.
ProbesDone ==
    /\ mode = "probe" /\ mode' = "up"
    /\ UNCHANGED <<bCur, bOpts, dbCur, dbOpts, seq, inflight, durableD, fm,
                   running, ps, servedWrong, stamp, muts, crashes>>

\* Not the code (known_bugs/recovery_clears_dirty): the start takes the
\* recovered rows out of the dirty set when its probe ends, with no syncfs
\* since the crashed run's backing changes (step 12.6b's first version). A
\* configuration puts it in with ProbesDone <- ProbesDoneClearing.
ProbesDoneClearing ==
    /\ mode = "probe"
    /\ Commit([dbCur EXCEPT !.dirty = FALSE], FALSE)
    /\ mode' = "up"
    /\ UNCHANGED <<bCur, bOpts, seq, inflight, durableD, fm, running, ps,
                   servedWrong, stamp, muts, crashes>>

\* Unmount: the session loop has stopped, no request is in flight.
\* (With F open dcfs's last sync point keeps F's row, so the shutdown is not
\* clean: as a daemon crash, not modelled apart.)
BeginShutdown ==
    /\ mode = "up" /\ \A p \in Procs : ps[p].pc = "idle"
    /\ fm.held = 0
    /\ mode' = "stop_sync"
    /\ UNCHANGED <<bCur, bOpts, dbCur, dbOpts, seq, inflight, durableD, fm,
                   running, ps, servedWrong, stamp, muts, crashes>>

\* FinishRun: a sync point (syncfs, then ClearDirty) ...
StopSync ==
    /\ mode = "stop_sync" /\ bOpts' = {bCur} /\ mode' = "stop_clear"
    /\ UNCHANGED <<bCur, dbCur, dbOpts, seq, inflight, durableD, fm, running, ps,
                   servedWrong, stamp, muts, crashes>>

StopClear ==
    /\ mode = "stop_clear"
    /\ Commit([dbCur EXCEPT !.dirty = FALSE, !.fDirty = "no"], FALSE)
    /\ durableD' = FALSE /\ mode' = "stop_ckpt"
    /\ fm' = [fm EXCEPT !.durable = FALSE]
    /\ UNCHANGED <<bCur, bOpts, seq, inflight, running, ps, servedWrong,
                   stamp, muts, crashes>>

\* ... a TRUNCATE checkpoint (everything committed is now durable) ...
StopCkpt ==
    /\ mode = "stop_ckpt" /\ dbOpts' = {dbCur} /\ mode' = "stop_flag"
    /\ UNCHANGED <<bCur, bOpts, dbCur, seq, inflight, durableD, fm, running, ps,
                   servedWrong, stamp, muts, crashes>>

\* ... and clean_shutdown = 1, kSync (the dirty set is empty: no writable
\* opens are modelled). The process exits.
StopFlag ==
    /\ mode = "stop_flag"
    /\ Commit([dbCur EXCEPT !.clean = TRUE], TRUE)
    /\ mode' = "down"
    /\ ResetMemory
    /\ fm' = [NoF EXCEPT !.lost = fm.lost]
    /\ UNCHANGED <<bCur, bOpts, servedWrong, stamp, muts, crashes>>

-----------------------------------------------------------------------------
(* The specification. *)

Init ==
    /\ \E present \in SUBSET Names :
         bCur = [names |-> [n \in Names |-> IF n \in present THEN InitObj(n)
                                            ELSE NoObj],
                 ver |-> 0, f |-> 0]
    /\ bOpts = {bCur}
    \* A new database: nothing cached.
    /\ dbCur = [dent |-> [n \in Names |-> NoRow], complete |-> FALSE,
                epoch |-> 0, attrValid |-> FALSE, attr |-> 0, dirty |-> FALSE,
                clean |-> FALSE, fValid |-> FALSE, fAttr |-> 0, fDirty |-> "no"]
    /\ dbOpts = {dbCur}
    /\ mode = "up"
    /\ seq = 0 /\ inflight = 0 /\ durableD = FALSE /\ running = None
    /\ ps = [p \in Procs |-> IdleProc]
    /\ servedWrong = FALSE
    /\ fm = NoF
    /\ stamp = NumNames + 1 /\ muts = 0 /\ crashes = 0

\* A crash while serving, during startup recovery (Restart's steps up to
\* StartRun), or during a clean shutdown: Crash, split only so that TLC's
\* coverage shows each fires (step 12.6: a crash during recovery must be
\* reachable for RecoveryIdempotent to mean anything).
StopModes == {"stop_sync", "stop_clear", "stop_ckpt", "stop_flag"}
CrashServing == mode = "up" /\ Crash
CrashRecovering == mode \notin {"up", "down"} \cup StopModes /\ Crash
CrashStopping == mode \in StopModes /\ Crash

Next ==
    \/ \E p \in Procs :
         \/ Arrive(p)
         \/ LookupStep(p) \/ ResolveProbe(p) \/ ResolveCommit(p)
         \/ PopulateRead(p) \/ PopulateCommit(p)
         \/ ReaddirStep(p) \/ ReaddirplusStat(p) \/ ReaddirplusFill(p)
         \/ GetattrStat(p) \/ GetattrFill(p)
         \/ CreateSyscall(p) \/ CreateProbe(p) \/ CreatePhase3(p)
         \/ CreateStat(p) \/ CreateFill(p) \/ CreateFailed(p)
         \/ UnlinkPhase1(p) \/ UnlinkSyscall(p) \/ UnlinkPhase3(p)
         \/ UnlinkStat(p) \/ UnlinkFill(p) \/ UnlinkFailed(p)
         \/ RenameResolveDst(p) \/ RenamePhase1(p) \/ RenameSyscall(p)
         \/ RenamePhase3(p) \/ RenameStat(p) \/ RenameFill(p)
         \/ RenameFailed(p) \/ RenameFailed2(p)
         \/ AttrChangeSyscall(p) \/ AttrChangePhase3(p)
         \/ AttrChangeStat(p) \/ AttrChangeFill(p)
         \/ SyncClearDirty(p)
         \/ Interrupt(p)
         \/ FileReleaseStat(p) \/ FileReleaseFill(p)
         \/ FileGetattrStat(p) \/ FileGetattrFill(p)
         \/ FileRefreshStat(p) \/ FileRefreshFill(p)
         \/ FileSetSyscall(p) \/ FileSetEnd(p) \/ FileSetStat(p)
         \/ FileSetFill(p)
    \/ FRead
    \/ CrashServing \/ CrashRecovering \/ CrashStopping
    \/ Restart \/ Recover \/ StartRun \/ ProbesDone
    \/ BeginShutdown \/ StopSync \/ StopClear \/ StopCkpt \/ StopFlag

\* Startup and shutdown steps are never postponed forever.
Fairness ==
    /\ WF_vars(Restart) /\ WF_vars(Recover) /\ WF_vars(StartRun)
    /\ WF_vars(ProbesDone)
    /\ WF_vars(StopSync) /\ WF_vars(StopClear) /\ WF_vars(StopCkpt)
    /\ WF_vars(StopFlag)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
(* Properties. *)

\* What the cache says about name n (or D's attributes) agrees with the
\* backing filesystem: unknown, or the truth.
NameOK(d, b, n) ==
    LET r == ReadState(d, n) IN
    \/ r = Unknown
    \/ r = Absent /\ b.names[n] = NoObj
    \/ r \in Objs /\ b.names[n] = r
AttrOK(d, b) == d.attrValid => d.attr = b.ver
\* F's cached attributes are never ahead of the backing filesystem's: a
\* value it has had (stamps only grow, and a power loss takes the backing
\* filesystem back to an earlier one). Exactness is FileExact's.
FileOK(d, b) == d.fValid => d.fAttr <= b.f
Correct(d, b) ==
    (\A n \in Names : NameOK(d, b, n)) /\ AttrOK(d, b) /\ FileOK(d, b)

\* "No cache ahead (or behind)": while the daemon serves, everything the
\* cache could serve agrees with the backing filesystem right now --
\* including after any crash and recovery.
CacheNeverWrong == mode = "up" => Correct(dbCur, bCur)

\* Completeness never claims a name is absent that the backing filesystem
\* has. (Part of CacheNeverWrong; stated on its own as in the plan.)
CompleteNeverHides ==
    mode = "up" =>
        \A n \in Names : (dbCur.complete /\ dbCur.dent[n] = NoRow)
                            => bCur.names[n] = NoObj

\* Every answer served from the cache matched the backing filesystem at the
\* moment it was served (see Serve).
ServedFromCacheIsCurrent == ~servedWrong

\* The tri-state rule: from a mutation's phase 1 until its phase 3 records
\* the outcome (or it fails), the records it changes read unknown.
MutatedNames(p) ==
    CASE ps[p].pc \in {"C_sys", "C_probe", "C_rec", "C_fail"} -> {ps[p].n}
      [] ps[p].pc \in {"U_sys", "U3", "U_fail"} -> {ps[p].n}
      [] ps[p].pc \in {"R_sys", "R3", "R_fail"} -> {ps[p].n, ps[p].m}
      [] OTHER -> {}
\* ... and an attribute change's: D's attributes, until its End.
AttrsMutated(p) == MutatedNames(p) # {} \/ ps[p].pc \in {"A_sys", "A3"}
TriState ==
    mode = "up" =>
        \A p \in Procs : AttrsMutated(p) =>
            /\ \A n \in MutatedNames(p) : ReadState(dbCur, n) = Unknown
            /\ ~dbCur.attrValid

\* Whatever a crash leaves of the two disks right now, startup recovery
\* turns it into a correct cache. (Stronger than CacheNeverWrong after an
\* actual crash: it is checked in every state, for every possible crash.)
CrashSafe == \A s \in dbOpts, t \in bOpts : Correct(RecoverDirty(s), t)

\* Recovery may crash and start again (FSCQ's crash condition for
\* recovery, step 12.6): while it runs (from a crash to ProbesDone),
\* every database state a crash may leave recovers to a correct cache,
\* whatever a crash left of the backing filesystem. It is CrashSafe in the
\* recovery modes, named for what a crash during recovery relies on: that
\* recovery's own commits never leave a state it cannot start from again
\* (known_bugs/recover_clears_dirty_first: one that empties the dirty set
\* first does).
RecoveryModes == Modes \ ({"up"} \cup StopModes)
RecoveryIdempotent ==
    mode \in RecoveryModes =>
        \A s \in dbOpts, t \in bOpts : Correct(RecoverDirty(s), t)

\* The fast path's premise: if Context::dirty.durable has D, every database
\* state a crash may leave has D dirty.
DurableSetSound ==
    /\ durableD => \A s \in dbOpts : s.dirty
    /\ fm.durable => \A s \in dbOpts : s.fDirty = "mut"

\* clean_shutdown = 1 is only ever durable with an empty dirty set.
CleanMeansNoDirty ==
    \A s \in dbOpts : s.clean => ~s.dirty /\ s.fDirty = "no"

\* Step 23.8: while dcfs holds no descriptor for F, its cached attributes are
\* the backing filesystem's exactly (what a stat is served from), unless a
\* power loss left them behind (fm.lost: the cold open's dirty row had not
\* reached the disk, and the backing filesystem kept a read's access time),
\* until they are next recorded. A daemon crash never does.
FileExact ==
    (mode = "up" /\ fm.held = 0 /\ ~fm.lost) =>
        (dbCur.fValid => dbCur.fAttr = bCur.f)
\* The same without the power-loss exception: limitations/
\* atime_power_loss_while_open expects its counterexample.
FileExactStrict ==
    (mode = "up" /\ fm.held = 0) => (dbCur.fValid => dbCur.fAttr = bCur.f)

\* The fill guards are balanced: a mutation is in flight on D (inflight)
\* exactly while some request is between its phase 1 and its End
\* (Mutation::End releases it on every path, an interrupt's included).
GuardsBalanced ==
    inflight = Cardinality({p \in Procs : ps[p].mseq # 0})

-----------------------------------------------------------------------------
(* Effect points (step 12.7, SibylFS's call, effect, return): each request *)
(* has at most one step at which what other requests see of D changes,    *)
(* and it is the backing syscall of a mutation; a fill's effect is on the  *)
(* cache only, at its commit, and changes nothing anyone sees. They are   *)
(* properties of steps ([][A]_vars), so TLC checks them on every           *)
(* transition: the observer reads D between any two steps.                *)

\* What another request would see of D now: each name's object (or NoObj)
\* and D's attributes, from the cache where it knows them, else from the
\* backing filesystem (the fill such a request would make).
Observed(d, b) ==
    [names |-> [n \in Names |->
                  LET r == ReadState(d, n) IN
                  IF r = Unknown THEN b.names[n]
                  ELSE IF r = Absent THEN NoObj ELSE r],
     ver |-> IF d.attrValid THEN d.attr ELSE b.ver]

\* What the cache knows: the names it answers without the backing
\* filesystem, and whether D's attributes are valid.
Known(d) == [names |-> {n \in Names : ReadState(d, n) # Unknown},
             attrs |-> d.attrValid]

\* The step is a mutation's backing syscall: the actions CSys, USys, RSys
\* themselves, not merely a step from their pc (another step from there,
\* such as an interrupt, is no effect point).
SyscallStep == \E p \in Procs :
                 \/ CreateSyscall(p) \/ UnlinkSyscall(p) \/ RenameSyscall(p)
                 \/ AttrChangeSyscall(p)

\* The fills the trace validation adds outside any request slot (a child's
\* or parent's row, the root's attributes at InitRoot: Trace.tla's
\* GetattrWhole); none in the model itself. Trace.cfg overrides it.
OutOfSlotFill == FALSE

\* The step records what was read: a fill's commit (a resolve, a
\* population, an attribute fill) or a mutation's phase 3.
CommitStep ==
    \/ \E p \in Procs :
         \/ ResolveCommit(p) \/ PopulateCommit(p) \/ ReaddirplusFill(p)
         \/ GetattrFill(p) \/ CreatePhase3(p) \/ CreateFill(p)
         \/ UnlinkPhase3(p) \/ UnlinkFill(p) \/ RenamePhase3(p)
         \/ RenameFill(p) \/ AttrChangeFill(p)
    \/ OutOfSlotFill

Serving == mode = "up" /\ mode' = "up"

\* What other requests see of D changes only at a mutation's backing
\* syscall: never at its phase 1 or phase 3, never at a fill, never at a
\* step of a request that has no syscall (nor at an interrupt). Each
\* request reaches its syscall at most once, so it has at most one effect
\* point.
EffectAtSyscall ==
    [][(Serving /\ Observed(dbCur', bCur') # Observed(dbCur, bCur))
         => SyscallStep]_vars

\* ... and the backing filesystem changes only there (while serving; a
\* crash may undo unsynced changes, which is no request's effect).
\* (F's backing attributes change at its setattr's syscall and at the
\* kernel's reads, FRead: the effect points of those.)
BackingAtSyscall ==
    [][(Serving /\ bCur' # bCur)
         => SyscallStep \/ FRead \/ \E p \in Procs : FileSetSyscall(p)]_vars

\* The cache learns (a name it did not know, or D's attributes) only at a
\* commit of what a request read; that changes nothing anyone sees
\* (EffectAtSyscall).
CacheLearnsAtCommit ==
    [][(Serving /\ (Known(dbCur').names \ Known(dbCur).names # {}
                    \/ (Known(dbCur').attrs /\ ~Known(dbCur).attrs)))
         => CommitStep]_vars

-----------------------------------------------------------------------------
(* Replies (step 12.7b, SibylFS's observation: call, effect, return).      *)
(* Every reply is one the backing filesystem gave: for a mutation that     *)
(* reached its syscall, the result of that syscall, its effect point       *)
(* (EffectAtSyscall); for an answer (a lookup's entry, a listing, D's      *)
(* attributes, an unlink's or rename's ENOENT from its resolve), what the  *)
(* backing filesystem answered at some instant between the request's call  *)
(* and its reply; EAGAIN and EINTR, which report that nothing happened,    *)
(* only from a request that had no effect (no syscall, or a failed one: a  *)
(* failed mutation whose re-resolve is interrupted replies EINTR, not its  *)
(* syscall's error, ReresolveAfterFailure). The instants are the backing   *)
(* states while the request was in flight: the one at its call, and each   *)
(* one a syscall made since (the backing changes at nothing else:          *)
(* BackingAtSyscall). An answer served from the cache is checked against   *)
(* the backing filesystem too, not against what the cache says: where the  *)
(* protocol lets the cache know an answer (CacheNeverWrong), that is the   *)
(* backing's answer at the instant it is served.                           *)

\* The answers a request (slot state r) has had from the backing filesystem
\* since its call: those it remembered (`win`) and those it has now.
Window(r) == r.win \cup BAns(r, bCur)

\* The kinds of answer a request of kind k replies (found and negative
\* entries are one kind): what its reply must carry, all of it.
QK(x) == IF x.k \in {"found", "neg"} THEN "entry" ELSE x.k
QueryKinds(k) ==
    CASE k \in {"lookup", "unlink", "rename"} -> {"entry"}
      [] k = "getattr" -> {"attr"}
      [] k = "readdir" -> {"list"}
      [] k = "readdirplus" -> {"list", "attr"}
      [] OTHER -> {}

\* The reply v of request r (its slot state at the step that replied) is
\* one the backing filesystem gave.
ReplyWitnessed(r, v) ==
    CASE r.kind \in FileKinds -> v.e = "ok" /\ v.a = {}
      [] v.e \in {"EAGAIN", "EINTR"} -> r.eff # "ok" /\ v.a = {}
      [] r.eff # None -> v.e = r.eff /\ v.a = {}
      [] r.kind = "sync" -> v.e = "ok" /\ v.a = {}
      [] OTHER ->
           /\ v.a # {} /\ v.a \subseteq Window(r)
           /\ {QK(x) : x \in v.a} = QueryKinds(r.kind)
           /\ IF r.kind \in {"unlink", "rename"}
              THEN v.e = "ENOENT" /\ \A x \in v.a : x.k = "neg"
              ELSE v.e = "ok"

\* A request that arrived and replied in one step (an answer served from
\* the cache at once): its only instant is the current one.
ImmediateWitnessed(v) ==
    IF v.k \in FileKinds THEN v.e = "ok" /\ v.a = {}
    ELSE /\ v.e = "ok" /\ v.a # {} /\ \A x \in v.a : AnswerOK(x, bCur)
         /\ {QK(x) : x \in v.a} = QueryKinds(v.k)

\* Every reply is one the backing filesystem gave (above). A reply is the
\* step at which a slot becomes idle, or an idle slot flips `rb` (a request
\* that arrived and replied at once).
ReplyObservable ==
    [][\A p \in Procs :
         (Serving /\ ps'[p].pc = "idle"
                  /\ (ps[p].pc # "idle" \/ ps'[p].rb # ps[p].rb))
           => IF ps[p].pc = "idle" THEN ImmediateWitnessed(ps'[p].rep)
              ELSE ReplyWitnessed(ps[p], ps'[p].rep)]_vars

\* Recovery always terminates: the daemon always gets back to serving.
RecoveryTerminates == (mode # "up") ~> (mode = "up")
=============================================================================
