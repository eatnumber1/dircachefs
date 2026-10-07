------------------------------ MODULE lifetime ------------------------------
(***************************************************************************)
(* The inode lifetime model: which nodeids the kernel holds, what dcfs    *)
(* keeps for each (a database row, an in-memory removed record, the       *)
(* written-file entry and its held descriptor, open backing files), and   *)
(* when each goes. README.md ("The lifetime model") explains it; this     *)
(* comment says what is in it.                                             *)
(*                                                                         *)
(* The kernel counts the entry replies that hand out a nodeid (LOOKUP,    *)
(* CREATE, MKNOD/MKDIR/SYMLINK, LINK, TMPFILE, READDIRPLUS entries other  *)
(* than "." and "..") and gives them back with FORGET (or a FORGET_MULTI  *)
(* batch), the last one when it evicts the inode, which it does only with *)
(* no file of it open. It keeps an inode as long as it likes after the    *)
(* last close (a dentry, a working directory, an O_PATH descriptor, a     *)
(* passthrough mapping after close), and may send a FORGET of part of the *)
(* count at any time (a READDIRPLUS entry it could not link). dcfs counts *)
(* the same replies (DirCacheFS::lookups_). The rules (docs/design.md,    *)
(* "Row lifetime", "mmap after close"):                                    *)
(*  - A row is a cache record: a named object's row stays whatever the    *)
(*    kernel holds. Removing a name never deletes a row by itself: after  *)
(*    an unlink or a rename over the object (SettleUnlinkedFile), a row   *)
(*    whose object has no link left goes, unless dcfs has the object      *)
(*    open: then it stays until the last release.                         *)
(*  - If the kernel still holds the nodeid when its row goes, a removed   *)
(*    record (DirCacheFS::removed_) with a descriptor taken before the    *)
(*    backing syscall (HoldForRemoval) answers for the object until the   *)
(*    last FORGET, which closes it (RetireRemoved).                       *)
(*  - A file with a writable open in this run is in written_; its last    *)
(*    close takes an O_PATH descriptor (the held descriptor, up to a cap) *)
(*    that its last FORGET, DESTROY, or dcfs's own removal of its last    *)
(*    link drops (ReconcileWritten, RetireRemoved).                       *)
(*  - An O_TMPFILE file is a row without a name, with nlink 0 in its      *)
(*    attributes; at the start after an unclean shutdown, rows with nlink *)
(*    0 and no name are swept (cache::ForgetUnnamedRows, review L5).      *)
(*                                                                         *)
(* Invariants (the PROPERTIES section): a nodeid the kernel holds         *)
(* resolves to the object it was handed out for, or to ESTALE, never to   *)
(* another (NodeidStable), and for an object the kernel holds, never to   *)
(* ESTALE either (ReferencedServed); an object's row or removed record    *)
(* goes only once nothing references it (NotRetiredWhileReferenced); a    *)
(* held descriptor exists only for a written object whose last FORGET has *)
(* not come, and such an object keeps its written_ entry until then       *)
(* (HeldOnlyWhileWritten, WrittenUntilLastForget); no FORGET forgets more *)
(* than dcfs counted (ForgetKnown); dcfs's count is the kernel's and      *)
(* nothing it keeps outlives the kernel's references (LookupsExact,       *)
(* NothingLeaks); after a crash the kernel holds nothing and the start    *)
(* sweeps every unnamed row (KernelForgotAfterCrash, UnnamedRowsSwept).   *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - Each request is one step, except an unlink's or rename's removal,   *)
(*    which is two (the backing syscall, then phase 3's settling of the   *)
(*    removed object) so that a crash can fall between them: dcfs serves  *)
(*    one request at a time, and the interleavings of the write-through   *)
(*    protocol are dcfs.tla's.                                             *)
(*  - One directory, names Names; objects Objs, some named at the start,  *)
(*    the others not yet created. Directories are objects that are never  *)
(*    opened (Opendir keeps nothing): their removal is a file's with no   *)
(*    open, so they are not modelled apart.                                *)
(*  - Writing: the code puts a file in written_ at a writable open        *)
(*    (DirCacheFS::BeginWriting), not at a write, so the writable Open is *)
(*    the model's write. What the reconciliation records is dcfs.tla's    *)
(*    and reval.tla's business; here only the entry and its descriptor.   *)
(*  - A crash is a daemon crash: the database keeps what it committed     *)
(*    (a power loss's rollback is dcfs.tla's), the kernel's state goes    *)
(*    (a new mount holds no nodeids), and an object nothing names or      *)
(*    holds is freed.                                                      *)
(*  - The descriptor HoldForRemoval takes, and the reopen at the last     *)
(*    release, always succeed (a failure is logged; the kernel's later    *)
(*    requests get ESTALE); the held descriptor's cap and EMFILE are a    *)
(*    nondeterministic choice at the last release.                         *)
(*  - The kernel's mapping after close is not a variable: it only delays  *)
(*    the last FORGET, which the kernel may delay anyway.                 *)
(*  - Out-of-band changes: only a boundary stub going (OutOfBand), for    *)
(*    the stubs' nodeids.                                                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Objs,          \* backing objects
    Names,         \* the names of the directory
    InitBName,     \* the object at each name at the start (or None)
    MaxId,         \* rows' nodeids are 1..MaxId, handed out in order
    StubIds,       \* boundary stubs' nodeids (numbers above MaxId)
    Boundaries,    \* refused names (each shown as a stub)
    MaxLookups,    \* bound on a nodeid's lookup count
    MaxOpens,      \* bound on a nodeid's open files
    MaxCrashes,    \* bound on crashes
    OutOfBand,     \* a stub may go during a run (an out-of-band relisting)
    DestroyWithOpens,  \* DESTROY may come with files still open
    \* Bugs, each put back by one constant (known_bugs/lifetime_*):
    BugNonFinalForgetDropsHeld, \* any FORGET ends the written_ entry
    BugNonFinalForgetDropsRec,  \* any FORGET ends the removed record
    BugNoUnnamedSweep,          \* no sweep of unnamed rows (before 23.7)
    BugForgetMultiCountsOne,    \* a FORGET_MULTI entry takes off 1
    BugStubIdFromMax,           \* a stub's nodeid: MAX(live) + 1 (12.4b)
    BugNoRecoveredProbe         \* no probe of recovered rows (12.4b)

None == "none"

Ids == 1..MaxId
AllIds == Ids \cup StubIds

-----------------------------------------------------------------------------
(* One nodeid's state: the kernel's view, then dcfs's.                     *)
(*   k     the kernel's lookup count (entry replies minus FORGETs)         *)
(*   ko    the object (or boundary) the kernel's lookups of it were handed *)
(*         out for, since the count last rose from 0 (checking only)       *)
(*   op    open files (the kernel's, each one of dcfs's open_files_; the   *)
(*         shared backing fd exists while op > 0)                          *)
(*   wo    ... of which may write (BackingFile::writable_refs)             *)
(*   wrote a writable open of it since the count last rose from 0 in this  *)
(*         run (checking only)                                             *)
(*   lk    dcfs's count of the kernel's lookups (lookups_)                 *)
(*   row   its database row exists (inodes)                                *)
(*   nl0   ... and its nlink column is 0                                   *)
(*   rec   a removed record answers for it (removed_)                      *)
(*   wr    written_: "no" entry, "nofd" (an entry without a held fd),      *)
(*         "held" (with one)                                               *)

IdStates ==
    [k : 0..MaxLookups, ko : Objs \cup Boundaries \cup {None},
     op : 0..MaxOpens, wo : 0..MaxOpens, wrote : BOOLEAN,
     lk : 0..MaxLookups,
     row : BOOLEAN, nl0 : BOOLEAN, rec : BOOLEAN,
     wr : {"no", "nofd", "held"}]

Fresh == [k |-> 0, ko |-> None, op |-> 0, wo |-> 0, wrote |-> FALSE, lk |-> 0,
          row |-> FALSE, nl0 |-> FALSE, rec |-> FALSE, wr |-> "no"]

(* What each step does to one nodeid's state. The global actions below and *)
(* the trace spec (LifetimeTrace.tla) both use these.                     *)

\* An entry reply handing the nodeid out for x (DirCacheFS::ReplyEntry and
\* the other ++lookups_).
AfterLookup(s, x) ==
    [s EXCEPT !.k = @ + 1, !.lk = @ + 1,
              !.ko = IF s.k = 0 THEN x ELSE @]

\* The row a resolve or a creation records (UpsertInode): nl0 for an
\* unnamed O_TMPFILE file (RecordTmpfile).
WithRow(s, nl0) == IF s.row THEN s ELSE [s EXCEPT !.row = TRUE, !.nl0 = nl0]

\* An open (Open, or a Create's or Tmpfile's): a writable one puts it in
\* written_ (BeginWriting), unless a removed record answers for it.
AfterOpen(s, w) ==
    [s EXCEPT !.op = @ + 1, !.wo = IF w THEN @ + 1 ELSE @, !.wrote = @ \/ w,
              !.wr = IF w /\ ~s.rec /\ @ = "no" THEN "nofd" ELSE @]

\* RetireRemoved with a descriptor on the object (`held`): the row goes,
\* into a removed record if the kernel holds the nodeid; written_ lets go.
Retire(s, held) ==
    [s EXCEPT !.row = FALSE, !.nl0 = FALSE, !.wr = "no",
              !.rec = held /\ s.lk > 0]

\* A release (Release) of an open that may write iff w. The last one of a
\* row whose object has no link left retires it (delete_row), keeping the
\* shared fd for the removed record; that of a written file otherwise
\* takes the held descriptor, or none at the cap (heldOk FALSE).
MayRelease(s, w) == IF w THEN s.wo > 0 ELSE s.op > s.wo
AfterRelease(s, w, named, heldOk) ==
    LET r == [s EXCEPT !.wo = IF w THEN @ - 1 ELSE @] IN
    IF s.op > 1 THEN [r EXCEPT !.op = @ - 1]
    ELSE IF s.row /\ ~named THEN Retire([r EXCEPT !.op = 0], TRUE)
    ELSE [r EXCEPT !.op = 0,
                   !.wr = IF @ = "nofd" /\ heldOk THEN "held" ELSE @]

\* Phase 3 of a removal of one of the object's names (SettleUnlinkedFile,
\* or RetireRemoved for a directory): an open object keeps its row (its
\* attributes refreshed from the open fd: nlink 0 if no link is left);
\* otherwise the row goes if no link is left, into a removed record if
\* HoldForRemoval took a descriptor (`held`).
AfterSettle(s, named, held) ==
    IF s.op > 0 THEN [s EXCEPT !.nl0 = ~named]
    ELSE IF named THEN [s EXCEPT !.nl0 = FALSE]
    ELSE Retire(s, held)

\* A FORGET of n lookups, of which dcfs takes off cnt (DropLookups): if it
\* counted fewer it logs the error and treats it as the last. The last
\* one ends the removed record and written_ entry (ReconcileWritten).
Last(s, cnt) == s.lk <= cnt
AfterForget(s, n, cnt) ==
    [s EXCEPT !.k = @ - n,
              !.ko = IF s.k = n THEN None ELSE @,
              !.wrote = IF s.k = n THEN FALSE ELSE @,
              !.lk = IF s.lk < cnt THEN 0 ELSE @ - cnt,
              !.rec = IF Last(s, cnt) \/ BugNonFinalForgetDropsRec
                      THEN FALSE ELSE @,
              !.wr = IF Last(s, cnt) \/ BugNonFinalForgetDropsHeld
                     THEN "no" ELSE @]

\* A crash or DESTROY: the kernel holds nothing any more, and dcfs's
\* memory (lookups_, removed_, written_, open files) is gone or cleared.
AfterReset(s) ==
    [s EXCEPT !.k = 0, !.ko = None, !.op = 0, !.wo = 0, !.wrote = FALSE,
              !.lk = 0, !.rec = FALSE, !.wr = "no"]

\* The start's probe of a recovered row by handle (ProbeRecoveredRows): its
\* row goes if its object was freed (ESTALE) or has no link left.
AfterProbe(s, freed) ==
    IF s.row /\ freed THEN [s EXCEPT !.row = FALSE, !.nl0 = FALSE] ELSE s

\* The start's sweep (cache::ForgetUnnamedRows): a row with nlink 0 and no
\* name goes.
AfterSweep(s, named) ==
    IF s.row /\ s.nl0 /\ ~named THEN [s EXCEPT !.row = FALSE, !.nl0 = FALSE]
    ELSE s

-----------------------------------------------------------------------------

VARIABLES
    st,       \* st[i]: nodeid i's state (IdStates)
    obj,      \* obj[i]: the object row i was made for (fixed once made)
    nextId,   \* the next row id (AUTOINCREMENT: never handed out twice)
    stub,     \* stub[m]: boundary m's stub nodeid, or 0 (the stubs table)
    stubHigh, \* the highest stub nodeid handed out, or 0
              \* (cache_state.last_stub_id)
    bName,    \* the backing directory: the object at each name, or None
    bState,   \* each object: "unborn", "alive" or "dead" (freed)
    pend,     \* the removal between its syscall and its phase 3
    run,      \* "up", or "down" (after a crash or DESTROY)
    clean,    \* the last run shut down cleanly (the clean-shutdown flag)
    crashes,  \* crashes so far (for the bound)
    cut,      \* the removal a crash cut between its syscall and its phase 3
              \* (its row is in the dirty set at the next start), or 0
    forgetErr \* a FORGET forgot more than dcfs counted (history)

stubVars == <<stub, stubHigh>>
crashVars == <<crashes, cut>>
vars == <<st, obj, nextId, stubVars, bName, bState, pend, run, clean,
          crashVars, forgetErr>>

NoPend == [id |-> 0, held |-> FALSE]

TypeOK ==
    /\ st \in [AllIds -> IdStates]
    /\ obj \in [Ids -> Objs \cup {None}]
    /\ nextId \in 1..(MaxId + 1)
    /\ stub \in [Boundaries -> StubIds \cup {0}]
    /\ stubHigh \in StubIds \cup {0}
    /\ bName \in [Names -> Objs \cup {None}]
    /\ bState \in [Objs -> {"unborn", "alive", "dead"}]
    /\ pend \in [id : Ids \cup {0}, held : BOOLEAN]
    /\ run \in {"up", "down"}
    /\ clean \in BOOLEAN
    /\ crashes \in 0..MaxCrashes
    /\ cut \in Ids \cup {0}
    /\ forgetErr \in BOOLEAN

Named(o) == \E n \in Names : bName[n] = o
\* The row of o, if any (rows are unique per object: UpsertInode).
RowsOf(o) == {i \in Ids : st[i].row /\ obj[i] = o}

\* What dcfs answers a request for nodeid i with: the removed record's
\* object, the row's while open_by_handle_at reaches it (while it is not
\* freed), a stub's boundary, or ESTALE.
Resolve(i) ==
    IF i \in StubIds
    THEN IF \E m \in Boundaries : stub[m] = i
         THEN CHOOSE m \in Boundaries : stub[m] = i ELSE "estale"
    ELSE IF st[i].rec THEN obj[i]
    ELSE IF st[i].row /\ bState[obj[i]] = "alive" THEN obj[i]
    ELSE "estale"

\* After a step: an object that nothing names and no descriptor of dcfs's
\* holds (an open file, a held descriptor, a removed record's, or the one
\* HoldForRemoval took for the removal in flight) is freed. `bs` is the
\* step's own change of the objects' states.
PinnedNext(o) ==
    \E i \in Ids :
        /\ obj'[i] = o
        /\ \/ st'[i].op > 0 \/ st'[i].wr = "held" \/ st'[i].rec
           \/ (pend'.id = i /\ pend'.held)
Freed(bs) ==
    bState' = [o \in Objs |->
                 IF bs[o] = "alive" /\ ~(\E n \in Names : bName'[n] = o)
                    /\ ~PinnedNext(o)
                 THEN "dead" ELSE bs[o]]

Init ==
    /\ st = [i \in AllIds |-> Fresh]
    /\ obj = [i \in Ids |-> None]
    /\ nextId = 1
    /\ stub = [m \in Boundaries |-> 0]
    /\ stubHigh = 0
    /\ bName = InitBName
    /\ bState = [o \in Objs |->
                   IF \E n \in Names : InitBName[n] = o
                   THEN "alive" ELSE "unborn"]
    /\ pend = NoPend
    /\ run = "up"
    /\ clean = TRUE
    /\ crashes = 0
    /\ cut = 0
    /\ forgetErr = FALSE

\* A request may start: the daemon is up and no removal is half done.
Up == run = "up" /\ pend = NoPend

\* Nodeid i is o's row, found or made by a resolve (LookupOrPopulate,
\* ResolveName: a new row takes the next id).
RowFor(o, i) ==
    \/ /\ st[i].row /\ obj[i] = o
       /\ UNCHANGED <<obj, nextId>>
    \/ /\ RowsOf(o) = {} /\ i = nextId
       /\ obj' = [obj EXCEPT ![i] = o]
       /\ nextId' = nextId + 1

-----------------------------------------------------------------------------
(* Requests.                                                               *)

\* LOOKUP of n (or a READDIRPLUS entry): its row, found or made, handed out.
Lookup(n) ==
    /\ Up /\ bName[n] # None
    /\ \E i \in Ids :
         /\ RowFor(bName[n], i)
         /\ st[i].k < MaxLookups
         /\ st' = [st EXCEPT ![i] = AfterLookup(WithRow(@, FALSE), bName[n])]
    /\ UNCHANGED <<stubVars, bName, bState, pend, run, clean, crashVars,
                   forgetErr>>

\* CREATE of n (open, writable or not): a new object, its new row.
Create(n, w) ==
    /\ Up /\ bName[n] = None /\ nextId \in Ids
    /\ \E o \in Objs : bState[o] = "unborn"
    /\ LET o == CHOOSE o \in Objs : bState[o] = "unborn"
           i == nextId
       IN /\ obj' = [obj EXCEPT ![i] = o]
          /\ nextId' = nextId + 1
          /\ bName' = [bName EXCEPT ![n] = o]
          /\ bState' = [bState EXCEPT ![o] = "alive"]
          /\ st' = [st EXCEPT ![i] = AfterOpen(AfterLookup(WithRow(@, FALSE),
                                                           o), w)]
    /\ UNCHANGED <<stubVars, pend, run, clean, crashVars, forgetErr>>

\* TMPFILE (DirCacheFS::Tmpfile): a new unnamed object, open for writing,
\* recorded as a row without a dentry, nlink 0.
Tmpfile ==
    /\ Up /\ nextId \in Ids
    /\ \E o \in Objs : bState[o] = "unborn"
    /\ LET o == CHOOSE o \in Objs : bState[o] = "unborn"
           i == nextId
       IN /\ obj' = [obj EXCEPT ![i] = o]
          /\ nextId' = nextId + 1
          /\ bState' = [bState EXCEPT ![o] = "alive"]
          /\ st' = [st EXCEPT ![i] = AfterOpen(AfterLookup(WithRow(@, TRUE),
                                                           o), TRUE)]
    /\ UNCHANGED <<stubVars, bName, pend, run, clean, crashVars, forgetErr>>

\* LINK of nodeid i to n: a second name, or the first of an O_TMPFILE file
\* (its attributes refreshed: nlink 1). A removed object (no row) gets
\* ESTALE (not a step).
Link(i, n) ==
    /\ Up /\ st[i].row /\ st[i].k > 0 /\ st[i].k < MaxLookups
    /\ bName[n] = None /\ bState[obj[i]] = "alive"
    /\ bName' = [bName EXCEPT ![n] = obj[i]]
    /\ st' = [st EXCEPT ![i] = AfterLookup([@ EXCEPT !.nl0 = FALSE], obj[i])]
    /\ UNCHANGED <<obj, nextId, stubVars, bState, pend, run, clean, crashVars,
                   forgetErr>>

\* OPEN of a nodeid the kernel holds, writable or not: of a row, or of a
\* removed object through its record (an open of /proc/<pid>/fd/<n>).
Open(i, w) ==
    /\ Up /\ st[i].k > 0 /\ st[i].op < MaxOpens /\ Resolve(i) # "estale"
    /\ st' = [st EXCEPT ![i] = AfterOpen(@, w)]
    /\ UNCHANGED <<obj, nextId, stubVars, bName, bState, pend, run, clean,
                   crashVars, forgetErr>>

\* RELEASE of an open of nodeid i that may write iff w.
Release(i, w) ==
    /\ Up /\ st[i].op > 0 /\ MayRelease(st[i], w)
    /\ \E heldOk \in BOOLEAN :
         st' = [st EXCEPT ![i] = AfterRelease(@, w, Named(obj[i]), heldOk)]
    /\ UNCHANGED <<obj, nextId, stubVars, bName, pend, run, clean, crashVars,
                   forgetErr>>
    /\ Freed(bState)

\* An UNLINK of n (src = None), or a RENAME of src over n: the resolve
\* (which may record a row), HoldForRemoval if the kernel holds the
\* nodeid, and the backing syscall. Phase 3 is Settle.
Remove(n, src) ==
    /\ Up /\ bName[n] # None
    /\ IF src = None THEN TRUE
       ELSE src # n /\ bName[src] \notin {None, bName[n]}
    /\ \E i \in Ids :
         /\ RowFor(bName[n], i)
         /\ st' = [st EXCEPT ![i] = WithRow(@, FALSE)]
         /\ pend' = [id |-> i, held |-> st[i].lk > 0]
    /\ bName' = IF src = None THEN [bName EXCEPT ![n] = None]
                ELSE [bName EXCEPT ![n] = bName[src], ![src] = None]
    /\ UNCHANGED <<stubVars, run, clean, crashVars, forgetErr>>
    /\ Freed(bState)

\* Phase 3 of the removal: SettleUnlinkedFile (RefreshAfterRename for a
\* rename's replaced object).
Settle ==
    /\ run = "up" /\ pend # NoPend
    /\ st' = [st EXCEPT ![pend.id] = AfterSettle(@, Named(obj[pend.id]),
                                                 pend.held)]
    /\ pend' = NoPend
    /\ UNCHANGED <<obj, nextId, stubVars, bName, run, clean, crashVars,
                   forgetErr>>
    /\ Freed(bState)

\* The kernel may FORGET n of a nodeid's lookups (its state s), all of them
\* only once no file of it is open (it evicts the inode).
MayForgetS(s, n) ==
    /\ 1 <= n /\ n <= s.k
    /\ n = s.k => s.op = 0
MayForget(i, n) == MayForgetS(st[i], n)

\* FORGET (DirCacheFS::Forget).
Forget(i, n) ==
    /\ Up /\ MayForget(i, n)
    /\ st' = [st EXCEPT ![i] = AfterForget(@, n, n)]
    /\ forgetErr' = (forgetErr \/ st[i].lk < n)
    /\ UNCHANGED <<obj, nextId, stubVars, bName, pend, run, clean, crashVars>>
    /\ Freed(bState)

\* FORGET_MULTI (DirCacheFS::ForgetMulti): f[i] lookups of each nodeid i in
\* its domain (the kernel names each nodeid once), in one step.
Counted(n) == IF BugForgetMultiCountsOne THEN 1 ELSE n
ForgetMulti(f) ==
    /\ Up /\ \A i \in DOMAIN f : MayForget(i, f[i])
    /\ st' = [i \in AllIds |->
                IF i \in DOMAIN f THEN AfterForget(st[i], f[i], Counted(f[i]))
                ELSE st[i]]
    /\ forgetErr' = (forgetErr \/ \E i \in DOMAIN f : st[i].lk < Counted(f[i]))
    /\ UNCHANGED <<obj, nextId, stubVars, bName, pend, run, clean, crashVars>>
    /\ Freed(bState)

\* DESTROY (DirCacheFS::Destroy), then FinishRun: the kernel let go of
\* everything (it sends no FORGETs at unmount). With DestroyWithOpens,
\* files may still be open (SIGTERM, or a lazy unmount: libfuse aborts the
\* connection); dcfs exits and closes them. FinishRun records a clean
\* shutdown only with nothing left dirty (ctx.dirty.any): a writable open
\* keeps its row durably dirty through the sync point (BeginWriting; the
\* last writable release's EndWriting lets it go), so a row with a
\* writable open left makes the next start unclean.
Destroy ==
    /\ Up /\ (DestroyWithOpens \/ \A i \in AllIds : st[i].op = 0)
    /\ st' = [i \in AllIds |-> AfterReset(st[i])]
    /\ run' = "down"
    /\ clean' = ~(\E i \in Ids : st[i].wo > 0 /\ st[i].row)
    /\ UNCHANGED <<obj, nextId, stubVars, bName, pend, crashVars, forgetErr>>
    /\ Freed(bState)

\* The daemon crashes, possibly between a removal's syscall and its phase
\* 3. The database keeps its rows.
Crash ==
    /\ run = "up" /\ crashes < MaxCrashes
    /\ st' = [i \in AllIds |-> AfterReset(st[i])]
    /\ pend' = NoPend
    /\ run' = "down" /\ clean' = FALSE /\ crashes' = crashes + 1
    /\ cut' = pend.id
    /\ UNCHANGED <<obj, nextId, stubVars, bName, forgetErr>>
    /\ Freed(bState)

\* The next start (StartRun): after an unclean shutdown, the probe of the
\* rows recovery found dirty (ProbeRecoveredRows: the row of a removal the
\* crash cut goes once its object is freed; the code probes every dirty
\* row, which changes nothing for the others) and the sweep of unnamed rows
\* (cache::ForgetUnnamedRows; also when recovery found dirty rows, which a
\* clean shutdown never leaves).
Restart ==
    /\ run = "down"
    /\ run' = "up"
    /\ st' = [i \in AllIds |->
                IF i \notin Ids \/ clean THEN st[i]
                ELSE AfterProbe(IF BugNoUnnamedSweep THEN st[i]
                                ELSE AfterSweep(st[i], Named(obj[i])),
                                i = cut /\ ~BugNoRecoveredProbe
                                  /\ bState[obj[i]] = "dead")]
    /\ cut' = 0
    /\ UNCHANGED <<obj, nextId, stubVars, bName, bState, pend, clean, crashes,
                   forgetErr>>

-----------------------------------------------------------------------------
(* Boundary stubs (cache::SetRefused): the next nodeid up from the highest *)
(* ever handed out (cache_state.last_stub_id), or the first. A refusal     *)
(* forgotten (ForgetNegativeDentries, a mutation's phase 1) keeps its stub *)
(* row, so it is no step here: the same name refused again keeps its      *)
(* nodeid. BugStubIdFromMax puts back the next up from the highest live    *)
(* stub's (before step 12.4b).                                              *)

Max(S) == CHOOSE x \in S : \A y \in S : y <= x
Min(S) == CHOOSE x \in S : \A y \in S : x <= y
LiveStubs == {stub[m] : m \in Boundaries} \ {0}
NextStub ==
    IF BugStubIdFromMax
    THEN IF LiveStubs = {} THEN Min(StubIds) ELSE Max(LiveStubs) + 1
    ELSE IF stubHigh = 0 THEN Min(StubIds) ELSE stubHigh + 1

\* A probe or listing finds m refused: its stub row.
Refuse(m) ==
    /\ Up /\ stub[m] = 0 /\ NextStub \in StubIds
    /\ stub' = [stub EXCEPT ![m] = NextStub]
    /\ stubHigh' = NextStub
    /\ UNCHANGED <<st, obj, nextId, bName, bState, pend, run, clean, crashVars,
                   forgetErr>>

\* LOOKUP of m: its stub's nodeid handed out.
LookupStub(m) ==
    /\ Up /\ stub[m] # 0 /\ st[stub[m]].k < MaxLookups
    /\ st' = [st EXCEPT ![stub[m]] = AfterLookup(@, m)]
    /\ UNCHANGED <<obj, nextId, stubVars, bName, bState, pend, run, clean,
                   crashVars, forgetErr>>

\* With OutOfBand: m stops being refused (a relisting after an out-of-band
\* change finds it a plain directory, or gone): the trigger deletes its
\* stub, and its nodeid is stale (ESTALE).
StubGone(m) ==
    /\ Up /\ OutOfBand /\ stub[m] # 0
    /\ stub' = [stub EXCEPT ![m] = 0]
    /\ UNCHANGED <<stubHigh, st, obj, nextId, bName, bState, pend, run, clean,
                   crashVars, forgetErr>>

-----------------------------------------------------------------------------

Next ==
    \/ \E n \in Names : Lookup(n)
    \/ \E n \in Names, w \in BOOLEAN : Create(n, w)
    \/ Tmpfile
    \/ \E i \in Ids, n \in Names : Link(i, n)
    \/ \E i \in Ids, w \in BOOLEAN : Open(i, w)
    \/ \E i \in Ids, w \in BOOLEAN : Release(i, w)
    \/ \E n \in Names, src \in Names \cup {None} : Remove(n, src)
    \/ Settle
    \/ \E i \in AllIds, n \in 1..MaxLookups : Forget(i, n)
    \/ \E ids \in (SUBSET {i \in AllIds : st[i].k > 0}) \ {{}} :
         \E f \in [ids -> 1..MaxLookups] : ForgetMulti(f)
    \/ Destroy
    \/ Crash
    \/ Restart
    \/ \E m \in Boundaries : Refuse(m) \/ LookupStub(m) \/ StubGone(m)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties.                                                             *)

\* (1) A nodeid the kernel holds resolves to the object it was handed out
\* for, or to ESTALE: never to another (no live nodeid is handed out
\* again, stubs' included).
NodeidStable ==
    \A i \in AllIds : st[i].k > 0 => Resolve(i) \in {st[i].ko, "estale"}

\* ... and never to ESTALE either: the kernel's reference keeps a removed
\* object reachable, as on a local filesystem (step 23.2). Not for a stub
\* that went after an out-of-band change (its name is no boundary any
\* more): ESTALE is its answer.
ReferencedServed ==
    \A i \in AllIds :
        st[i].k > 0 /\ (i \in Ids \/ ~OutOfBand) => Resolve(i) = st[i].ko

\* (2) An object's row or removed record goes only when the kernel holds
\* no lookup of it, no file of it is open and no held descriptor is
\* left; a removed record stands in for a row, never beside one.
NotRetiredWhileReferenced ==
    \A i \in Ids :
        /\ (st[i].k > 0 \/ st[i].op > 0 \/ st[i].wr = "held")
             => (st[i].row \/ st[i].rec)
        /\ st[i].rec => ~st[i].row

\* (3) A written_ entry (and so a held descriptor) exists only for a file
\* with a writable open since the kernel's count last rose from 0, whose
\* last FORGET has not come, and which still has its row (dcfs's own
\* removal of its last link drops it) ...
HeldOnlyWhileWritten ==
    \A i \in Ids :
        st[i].wr # "no" => st[i].wrote /\ st[i].k > 0 /\ st[i].row

\* ... and such a file keeps it until its last FORGET, which is the only
\* later event at which a store through a mapping after close can be seen.
WrittenUntilLastForget ==
    \A i \in Ids :
        st[i].wrote /\ st[i].row => st[i].wr # "no"

\* (4) No FORGET forgets a lookup dcfs did not count (DropLookups's error:
\* a FORGET of a nodeid dcfs does not know).
ForgetKnown ==
    /\ ~forgetErr
    /\ \A i \in AllIds : st[i].k <= st[i].lk

\* (5) dcfs's count is the kernel's, and what it keeps for a nodeid
\* (removed record, written_ entry, held descriptor) is bounded by what
\* the kernel holds: nothing outlives the last FORGET.
LookupsExact == \A i \in AllIds : st[i].lk = st[i].k
NothingLeaks ==
    \A i \in AllIds : (st[i].rec \/ st[i].wr # "no") => st[i].k > 0

\* (6) After a crash the kernel holds nothing and dcfs's memory is empty;
\* once started, no row with nlink 0 and no name is left that nothing has
\* open (the rows whose last release never came).
KernelForgotAfterCrash ==
    run = "down" =>
        \A i \in AllIds : st[i].k = 0 /\ st[i].op = 0 /\ st[i].lk = 0
                          /\ ~st[i].rec /\ st[i].wr = "no"
UnnamedRowsSwept ==
    Up => \A i \in Ids :
            st[i].row /\ st[i].nl0 /\ ~Named(obj[i]) => st[i].op > 0

\* Stronger than UnnamedRowsSwept (formal/findings/): no row is left of an
\* object that has been freed.
RowsNameLiveObjects ==
    Up => \A i \in Ids : st[i].row => bState[obj[i]] = "alive"

\* Every invariant the real configurations check.
LifetimeSafe ==
    /\ NodeidStable /\ ReferencedServed /\ NotRetiredWhileReferenced
    /\ HeldOnlyWhileWritten /\ WrittenUntilLastForget /\ ForgetKnown
    /\ LookupsExact /\ NothingLeaks /\ KernelForgotAfterCrash
    /\ UnnamedRowsSwept
=============================================================================
