------------------------------- MODULE ident -------------------------------
(***************************************************************************)
(* The identity model: what a nodeid and its FUSE generation stand for,   *)
(* how dcfs mints and resolves them, and what an NFS client's handle      *)
(* reaches after the backing filesystem recycles an inode number, after a *)
(* crash, a power loss or a cache wipe, or after a change behind dcfs's   *)
(* back. README.md ("The identity model") explains it; this comment says   *)
(* what is in it.                                                          *)
(*                                                                         *)
(* The kernel puts (nodeid, generation) in an NFS file handle             *)
(* (fuse_encode_fh). Decoding one (fuse_get_dentry) uses the kernel's own  *)
(* inode for the nodeid if its generation matches, and otherwise sends    *)
(* LOOKUP(nodeid, ".") and returns ESTALE unless the reply's generation   *)
(* is the handle's (an ENOENT reply also becomes ESTALE). An entry reply  *)
(* whose generation differs from the one of the inode the kernel holds    *)
(* for that nodeid makes the kernel mark that inode bad (fuse_iget): its   *)
(* users get EIO, not ESTALE. Requests on an inode carry only the nodeid. *)
(*                                                                         *)
(* dcfs today (Target = FALSE; docs/design.md "Identity model"): a        *)
(* nodeid is a row id (AUTOINCREMENT), its generation a random number     *)
(* drawn when the row is made (cache::UpsertInode); a row records the     *)
(* backing object's inode number, generation (FS_IOC_GETVERSION, 0 if it  *)
(* cannot be read), file handle and birth time, read by one probe through  *)
(* one O_PATH descriptor (ProbeObject). A request reaching the backing    *)
(* filesystem reopens the row's handle (open_by_handle_at) and compares   *)
(* the result with the row (VerifyBackingIdentity): ESTALE, and the row   *)
(* goes, if the handle is stale or reaches another object. A probe of a   *)
(* name invalidates the rows its object's inode number had for another    *)
(* object (UpsertInode). LOOKUP(nodeid, ".") is answered from the row.     *)
(* After an unclean shutdown, the start probes the rows of the dirty set  *)
(* by handle (ProbeRecoveredRows).                                         *)
(*                                                                         *)
(* Phase 14's target (Target = TRUE; the plan's phase 14): the nodeid     *)
(* is the backing inode number, the generation the backing generation     *)
(* from the handle; rows are keyed by inode number; LOOKUP(nodeid, ".")   *)
(* without a row opens the inode by number (any generation) and replies   *)
(* with the generation it finds, which the kernel compares.               *)
(*                                                                         *)
(* Invariants (the PROPERTIES section): no two references the kernel or   *)
(* an NFS client holds with one (nodeid, generation) stand for different  *)
(* objects (OneHandleOneObject); a nodeid the kernel holds, and a handle  *)
(* the kernel accepts, resolve to their object or to ESTALE               *)
(* (HeldResolvesToItsObject, HandlesResolveToTheirObject); a handle of an *)
(* object that still exists is served (ServedWhileLive), one of an object *)
(* that is gone is not (GoneIsStale); a row whose object is gone resolves *)
(* to ESTALE at every entry point, and no probe of another object takes   *)
(* it over (ReuseDetected); dcfs never makes the kernel mark an inode bad *)
(* (NoBadInode).                                                           *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - One directory, names Names; objects Objs, each with a fixed inode   *)
(*    number (InoOf: two objects with one number are a recycling) and a   *)
(*    generation and birth time unique to it (GenOf). What dcfs can see   *)
(*    of them is Evidence: the generation in the handle (ext4, xfs,       *)
(*    btrfs), FS_IOC_GETVERSION (regular files and directories on those),  *)
(*    the birth time. 0 is "unknown", as in the code.                     *)
(*  - Random generations are fresh: a draw never repeats an earlier one   *)
(*    (the design's 2^-32 per reissued nodeid is taken as zero).          *)
(*  - Each request is one step. A probe is one step too: the code reads   *)
(*    everything through one O_PATH descriptor, which pins the inode, so  *)
(*    no recycling can come between its reads. ProbeByName puts back the  *)
(*    alternative (each read resolves the name again; three steps, in     *)
(*    ProbeOrder) to show that the descriptor, not the order, is what     *)
(*    makes the probe sound (the 26.3 question).                          *)
(*  - A cached dentry answers as the probe would (dcfs.tla's              *)
(*    CacheNeverWrong); lookup counts, open files and the held            *)
(*    descriptor are lifetime.tla's: here a removed record answers for an *)
(*    object unlinked through dcfs until the kernel evicts the nodeid.    *)
(*  - A power loss rolls the database back to its last durable commit (a  *)
(*    phase 1, a WAL checkpoint, the start: Persist); the backing         *)
(*    filesystem keeps what it did (dcfs.tla's crash states cover the     *)
(*    rest). A cache wipe happens while dcfs is stopped.                  *)
(*  - The root (nodeid 1, generation 0) is not modelled.                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Objs,          \* backing objects (files and directories, and Boundaries)
    Names,         \* the names of the directory
    InitBName,     \* the object at each name at the start (or None)
    InoOf,         \* each object's inode number
    GenOf,         \* each object's generation (and birth time): unique
    Boundaries,    \* objects that are another filesystem's root (refused)
    Evidence,      \* what identifies an object besides its inode number:
                   \* a subset of {"handle_gen", "getversion", "btime"}
    MaxRowId,      \* rows' nodeids are 1..MaxRowId (today)
    StubIds,       \* boundary stubs' nodeids
    MaxMints,      \* bound on random generations drawn
    MaxHandles,    \* bound on the NFS client's handles
    MaxCrashes,    \* bound on daemon crashes
    MaxPowerLosses, \* bound on power losses
    MaxWipes,      \* bound on cache wipes
    Target,        \* Phase 14's identity instead of today's
    OutOfBand,     \* the backing directory changes behind dcfs's back
    ProbeByName,   \* a probe's reads each resolve the name (not the code)
    ProbeOrder,    \* ... "handle_first" (the code's order) or "gen_first"
    \* Bugs, each put back by one constant (known_bugs/ident_*):
    BugSkipVerify,     \* no identity statx after open_by_handle_at
    BugGenFromId,      \* a row's generation follows from its id (a counter)
    BugStubGenFixed,   \* a stub's generation is not drawn afresh
    BugRowIdFromMax    \* a row's id is MAX(id) + 1, not AUTOINCREMENT

None == "none"
Estale == "estale"

Files == Objs \ Boundaries
RowIds == 1..MaxRowId
Inos == {InoOf[o] : o \in Files}
\* The nodeids of files and directories: row ids, or (Phase 14) inode numbers.
Ids == IF Target THEN Inos ELSE RowIds
AllIds == Ids \cup StubIds

Max(S) == CHOOSE x \in S : \A y \in S : y <= x
Min(S) == CHOOSE x \in S : \A y \in S : x <= y

-----------------------------------------------------------------------------
(* What dcfs can see of an object (backing::ProbeObject): its inode number *)
(* (statx), its file handle (name_to_handle_at: inode number and, where    *)
(* the filesystem encodes one, generation), its generation                 *)
(* (FS_IOC_GETVERSION) and its birth time (statx). 0 is "unknown".         *)

HGen(o) == IF "handle_gen" \in Evidence THEN GenOf[o] ELSE 0
VGen(o) == IF "getversion" \in Evidence THEN GenOf[o] ELSE 0
Btime(o) == IF "btime" \in Evidence THEN GenOf[o] ELSE 0
IdentOf(o) == [ino |-> InoOf[o], hino |-> InoOf[o], hgen |-> HGen(o),
               gen |-> VGen(o), bt |-> Btime(o)]

\* A row (inodes, or stubs): `obj` is the object it was made for (or, for a
\* stub, its name: a stub stands for the refused name), checking only.
NoRow == [ex |-> FALSE, stub |-> FALSE, obj |-> None, ino |-> 0, hino |-> 0,
          hgen |-> 0, gen |-> 0, bt |-> 0, fgen |-> 0, name |-> None]
FileRow(x, i, g) ==
    [ex |-> TRUE, stub |-> FALSE, obj |-> x, ino |-> i.ino, hino |-> i.hino,
     hgen |-> i.hgen, gen |-> i.gen, bt |-> i.bt, fgen |-> g, name |-> None]
StubRow(n, g) == [NoRow EXCEPT !.ex = TRUE, !.stub = TRUE, !.obj = n,
                               !.fgen = g, !.name = n]

\* A kernel inode: the generation it was made with, and the object (or
\* refused name) of the reply that made it (checking only).
NoK == [held |-> FALSE, gen |-> 0, obj |-> None]

\* Two values of one field agree unless both are known and differ.
Agree(x, y) == x = 0 \/ y = 0 \/ x = y

\* VerifyBackingIdentity: the object a reopened handle reached is the row's
\* (inode number; generation and birth time where both are known).
SameObject(row, i) ==
    row.ino = i.ino /\ Agree(row.gen, i.gen) /\ Agree(row.bt, i.bt)

\* FindMatchingRows (UpsertInode): a row with this inode number is this
\* object's if the generations agree, the handle bytes are equal and the
\* birth times agree.
UpsertMatch(row, i) ==
    /\ row.ino = i.ino /\ Agree(row.gen, i.gen)
    /\ row.hino = i.hino /\ row.hgen = i.hgen
    /\ Agree(row.bt, i.bt)

-----------------------------------------------------------------------------

VARIABLES
    bName,     \* the backing directory: the object at each name, or None
    bState,    \* each object: "unborn", "alive" or "dead" (freed)
    rows,      \* rows[r]: nodeid r's row (inodes or stubs), or NoRow
    nextId,    \* the next row id (AUTOINCREMENT)
    stubHigh,  \* the highest stub nodeid handed out (last_stub_id), or 0
    dirty,     \* the dirty set: rows mutated since the last sync point
    dur,       \* the last durable commit: [rows, nextId, stubHigh, dirty]
    rec,       \* rec[r]: the object a removed record answers for, or None
    kin,       \* kin[r]: the kernel's inode for nodeid r (this mount), or NoK
    nfs,       \* the NFS client's handles: [id, gen, obj]
    mints,     \* random generations drawn so far
    probe,     \* a probe by name in flight (ProbeByName), or NoProbe
    run,       \* "up", or "down" (stopped, crashed)
    clean,     \* the last run shut down cleanly (the clean-shutdown flag)
    crashes, powers, wipes,  \* counts, for the bounds
    badMade    \* dcfs made the kernel mark an inode bad (history)

dbVars == <<rows, nextId, stubHigh, dirty>>
countVars == <<crashes, powers, wipes>>
vars == <<bName, bState, rows, nextId, stubHigh, dirty, dur, rec, kin, nfs,
          mints, probe, run, clean, countVars, badMade>>

NoProbe == [n |-> None, st |-> None, ino |-> 0, bt |-> 0, hino |-> 0,
            hgen |-> 0, gen |-> 0, step |-> 0]

Durable == [rows |-> rows, nextId |-> nextId, stubHigh |-> stubHigh,
            dirty |-> dirty]

RowType == [ex : BOOLEAN, stub : BOOLEAN, obj : Objs \cup Names \cup {None},
            ino : Nat, hino : Nat, hgen : Nat, gen : Nat, bt : Nat,
            fgen : Nat, name : Names \cup {None}]

TypeOK ==
    /\ bName \in [Names -> Objs \cup {None}]
    /\ bState \in [Objs -> {"unborn", "alive", "dead"}]
    /\ rows \in [AllIds -> RowType]
    /\ nextId \in Nat /\ stubHigh \in StubIds \cup {0}
    /\ dirty \subseteq Ids
    /\ rec \in [Ids -> Files \cup {None}]
    /\ kin \in [AllIds -> [held : BOOLEAN, gen : Nat,
                           obj : Objs \cup Names \cup {None}]]
    /\ nfs \subseteq [id : AllIds, gen : Nat, obj : Objs \cup Names]
    /\ mints \in 0..MaxMints
    /\ run \in {"up", "down"} /\ clean \in BOOLEAN
    /\ badMade \in BOOLEAN

Up == run = "up"
Idle == probe = NoProbe
\* The durable copy is kept only while a power loss can still come (it
\* changes nothing else, and freezing it keeps the state space small).
Track == powers < MaxPowerLosses

-----------------------------------------------------------------------------
(* Resolution: what a request reaches.                                     *)

\* open_by_handle_at of a row's handle: the live file with its inode
\* number, unless the handle's generation (if it carries one) differs.
Decode(row) ==
    LET c == {x \in Files : bState[x] = "alive" /\ InoOf[x] = row.hino
                            /\ (row.hgen = 0 \/ row.hgen = HGen(x))}
    IN IF c = {} THEN None ELSE CHOOSE x \in c : TRUE

\* Phase 14: the live file with inode number i (opened by number, any
\* generation).
ByIno(i) ==
    LET c == {x \in Files : bState[x] = "alive" /\ InoOf[x] = i}
    IN IF c = {} THEN None ELSE CHOOSE x \in c : TRUE

\* What a request on nodeid r that reaches the backing filesystem resolves
\* to (OpenNode: open_by_handle_at, then VerifyBackingIdentity): the removed
\* record's object, the row's object if its handle reaches one that
\* matches the row, a stub's name (answered from its row), or ESTALE.
\* Phase 14, without a row: the inode found by number (its plan: "any
\* request for an uncached nodeid resolves through 14.2").
Resolve(r) ==
    IF r \in Ids /\ rec[r] # None THEN rec[r]
    ELSE IF ~rows[r].ex
         THEN IF Target /\ r \in Inos /\ ByIno(r) # None THEN ByIno(r)
              ELSE Estale
    ELSE IF rows[r].stub THEN rows[r].obj
    ELSE LET x == Decode(rows[r]) IN
         IF x = None THEN Estale
         ELSE IF BugSkipVerify \/ SameObject(rows[r], IdentOf(x)) THEN x
         ELSE Estale

\* The generation of dcfs's reply to LOOKUP(r, "."), or 0 for ESTALE
\* (RequireAttr: no row). From the row (EntryFor); Phase 14, without a
\* row: the generation of the inode found by number.
DotAnswer(r) ==
    IF rows[r].ex THEN rows[r].fgen
    ELSE IF Target /\ r \in Inos /\ ByIno(r) # None THEN HGen(ByIno(r))
    ELSE 0

\* The kernel accepts an NFS handle: it holds a matching inode, or dcfs's
\* LOOKUP(".") reply carries the handle's generation.
Accepts(h) ==
    \/ kin[h.id].held /\ kin[h.id].gen = h.gen
    \/ DotAnswer(h.id) = h.gen

\* An object (or a stub's name) still exists: alive, or a boundary at that
\* name.
Live(x) == IF x \in Names THEN bName[x] \in Boundaries ELSE bState[x] = "alive"

-----------------------------------------------------------------------------
(* Steps shared by the actions.                                            *)

\* An entry reply handing out nodeid r with generation g for x: the kernel
\* makes its inode (fuse_iget), keeps the one it has if the generation is
\* the same, and marks it bad if not.
Reply(r, g, x) ==
    /\ kin' = [kin EXCEPT ![r] = IF @.held /\ @.gen = g THEN @
                                 ELSE [held |-> TRUE, gen |-> g, obj |-> x]]
    /\ badMade' = (badMade \/ (kin[r].held /\ kin[r].gen # g))

\* An object nothing names and no removed record holds is freed. `bs` is
\* the step's own change of the objects' states.
Freed(bs) ==
    bState' = [o \in Objs |->
                 IF bs[o] = "alive" /\ ~(\E n \in Names : bName'[n] = o)
                    /\ ~(\E r \in Ids : rec'[r] = o)
                 THEN "dead" ELSE bs[o]]

\* The stub rows of name n (stubs is keyed by its parent and name).
StubOf(rs, n) == {s \in StubIds : rs[s].ex /\ rs[s].stub /\ rs[s].name = n}
WithoutStubOf(rs, n) ==
    [i \in AllIds |-> IF i \in StubOf(rs, n) THEN NoRow ELSE rs[i]]

FileRowIds(rs) == {r \in Ids : rs[r].ex /\ ~rs[r].stub}

\* cache::UpsertInode over rows rs, for an object seen as i (ghost x):
\* [rows, nextId, mints, id], and ok FALSE if a bound stops it.
Upsert(rs, i, x) ==
    IF Target
    THEN LET r == i.ino IN
         IF rs[r].ex /\ UpsertMatch(rs[r], i)
         THEN [ok |-> TRUE, rows |-> rs, nextId |-> nextId, mints |-> mints,
               id |-> r]
         ELSE [ok |-> TRUE, rows |-> [rs EXCEPT ![r] = FileRow(x, i, i.hgen)],
               nextId |-> nextId, mints |-> mints, id |-> r]
    ELSE LET m == {r \in FileRowIds(rs) : UpsertMatch(rs[r], i)}
             stale == {r \in FileRowIds(rs) : rs[r].ino = i.ino} \ m
             kept == [k \in AllIds |-> IF k \in stale THEN NoRow ELSE rs[k]]
             live == FileRowIds(kept)
             r == IF BugRowIdFromMax
                  THEN IF live = {} THEN 1 ELSE Max(live) + 1
                  ELSE nextId
             g == IF BugGenFromId THEN r ELSE mints + 1
         IN IF m # {}
            THEN LET e == CHOOSE e \in m : TRUE IN
                 [ok |-> TRUE,
                  rows |-> [rs EXCEPT ![e].bt = IF i.bt # 0 THEN i.bt ELSE @],
                  nextId |-> nextId, mints |-> mints, id |-> e]
            ELSE [ok |-> r \in RowIds /\ (BugGenFromId \/ mints < MaxMints),
                  rows |-> IF r \in RowIds
                           THEN [kept EXCEPT ![r] = FileRow(x, i, g)]
                           ELSE kept,
                  nextId |-> Max({nextId, r + 1}),
                  mints |-> IF BugGenFromId THEN mints ELSE mints + 1,
                  id |-> r]

\* A resolve of name n that found x, seen as i: the stub of n goes (a
\* relisting finds it no boundary), its row is found or made, and its
\* nodeid handed out (LookupOrPopulate, ResolveName, EntryFor, ReplyEntry).
Record(n, i, x) ==
    LET u == Upsert(WithoutStubOf(rows, n), i, x) IN
    /\ u.ok
    /\ rows' = u.rows /\ nextId' = u.nextId /\ mints' = u.mints
    /\ Reply(u.id, u.rows[u.id].fgen, x)

\* A phase 1 marking rows R dirty: a durable commit (kSync) unless every
\* one of them already was.
MarkDirty(R) ==
    /\ dirty' = dirty \cup R
    /\ dur' = IF Track /\ ~(R \subseteq dirty)
              THEN [Durable EXCEPT !.dirty = dirty \cup R] ELSE dur

\* The rows of the object at a name, as dcfs's resolve of it finds them.
RowsOf(o) == {r \in FileRowIds(rows) : UpsertMatch(rows[r], IdentOf(o))}

-----------------------------------------------------------------------------
(* Requests.                                                               *)

\* LOOKUP of n (or a READDIRPLUS entry): the probe, through one descriptor.
Lookup(n) ==
    /\ Up /\ Idle /\ ~ProbeByName /\ bName[n] \in Files
    /\ Record(n, IdentOf(bName[n]), bName[n])
    /\ UNCHANGED <<bName, bState, stubHigh, dirty, dur, rec, nfs, probe, run,
                   clean, countVars>>

\* LOOKUP of a refused name: its stub (cache::SetRefused): the same one if
\* the name has one, else the next nodeid up from the highest ever handed
\* out, with a fresh random generation.
Refuse(n) ==
    /\ Up /\ Idle /\ bName[n] \in Boundaries
    /\ IF StubOf(rows, n) # {}
       THEN LET s == CHOOSE s \in StubOf(rows, n) : TRUE IN
            /\ Reply(s, rows[s].fgen, n)
            /\ UNCHANGED <<rows, stubHigh, mints>>
       ELSE LET s == IF stubHigh = 0 THEN Min(StubIds) ELSE stubHigh + 1
                g == IF BugStubGenFixed THEN 1 ELSE mints + 1
            IN /\ s \in StubIds /\ (BugStubGenFixed \/ mints < MaxMints)
               /\ rows' = [rows EXCEPT ![s] = StubRow(n, g)]
               /\ stubHigh' = s
               /\ mints' = IF BugStubGenFixed THEN mints ELSE mints + 1
               /\ Reply(s, g, n)
    /\ UNCHANGED <<bName, bState, nextId, dirty, dur, rec, nfs, probe, run,
                   clean, countVars>>

\* CREATE (MKDIR, ...) of n: a new object, maybe on a recycled inode
\* number, probed and recorded (RecordNewChild).
Create(n) ==
    /\ Up /\ Idle /\ bName[n] = None
    /\ \E o \in Files :
         /\ bState[o] = "unborn"
         /\ ~\E x \in Files : bState[x] = "alive" /\ InoOf[x] = InoOf[o]
         /\ bName' = [bName EXCEPT ![n] = o]
         /\ bState' = [bState EXCEPT ![o] = "alive"]
         /\ Record(n, IdentOf(o), o)
    /\ UNCHANGED <<stubHigh, dirty, dur, rec, nfs, probe, run, clean,
                   countVars>>

\* UNLINK (RMDIR) of n: phase 1 marks its row dirty; the syscall; a removed
\* record holds the object if the kernel holds its nodeid (HoldForRemoval);
\* phase 3 deletes the row (the object has no link left).
Unlink(n) ==
    /\ Up /\ Idle /\ bName[n] \in Files
    /\ LET o == bName[n]
           R == RowsOf(o)
       IN /\ MarkDirty(R)
          /\ bName' = [bName EXCEPT ![n] = None]
          /\ rec' = [r \in Ids |-> IF r \in R /\ kin[r].held THEN o
                                   ELSE rec[r]]
          /\ rows' = [r \in AllIds |-> IF r \in R THEN NoRow ELSE rows[r]]
          /\ Freed(bState)
    /\ UNCHANGED <<nextId, stubHigh, kin, nfs, mints, probe, run, clean,
                   countVars, badMade>>

\* RENAME of n to a free name m: the object keeps its row and nodeid.
Rename(n, m) ==
    /\ Up /\ Idle /\ n # m /\ bName[n] \in Files /\ bName[m] = None
    /\ MarkDirty(RowsOf(bName[n]))
    /\ bName' = [bName EXCEPT ![m] = bName[n], ![n] = None]
    /\ rows' = WithoutStubOf(rows, m)
    /\ UNCHANGED <<bState, nextId, stubHigh, rec, kin, nfs, mints, probe, run,
                   clean, countVars, badMade>>

\* A request on nodeid r that reaches the backing filesystem (a refresh, an
\* open, ParentOf) and resolves to ESTALE: the row goes (ForgetStale). A
\* request that is served changes nothing here.
Access(r) ==
    /\ Up /\ Idle
    /\ kin[r].held \/ \E h \in nfs : h.id = r
    /\ rows[r].ex /\ ~rows[r].stub /\ Resolve(r) = Estale
    /\ rows' = [rows EXCEPT ![r] = NoRow]
    /\ UNCHANGED <<bName, bState, nextId, stubHigh, dirty, dur, rec, kin, nfs,
                   mints, probe, run, clean, countVars, badMade>>

\* The NFS client presents handle h and the kernel holds no inode with its
\* generation: LOOKUP(h.id, "."). dcfs answers from the row (EntryFor;
\* Phase 14 without a row: by inode number, recording a row). The kernel
\* makes its inode either way (fuse_lookup_name) and then compares
\* generations.
DotLookup(h) ==
    /\ Up /\ Idle /\ h \in nfs
    /\ ~(kin[h.id].held /\ kin[h.id].gen = h.gen)
    /\ DotAnswer(h.id) # 0
    /\ IF rows[h.id].ex
       THEN /\ Reply(h.id, rows[h.id].fgen, rows[h.id].obj)
            /\ UNCHANGED rows
       ELSE LET x == ByIno(h.id) IN
            /\ rows' = [rows EXCEPT ![h.id] = FileRow(x, IdentOf(x), HGen(x))]
            /\ Reply(h.id, HGen(x), x)
    /\ UNCHANGED <<bName, bState, nextId, stubHigh, dirty, dur, rec, nfs,
                   mints, probe, run, clean, countVars>>

\* The NFS client gets a handle of an inode the kernel holds
\* (fuse_encode_fh: its nodeid and generation).
NfsTake(r) ==
    /\ Up /\ kin[r].held /\ Cardinality(nfs) < MaxHandles
    /\ nfs' = nfs \cup {[id |-> r, gen |-> kin[r].gen, obj |-> kin[r].obj]}
    /\ nfs' # nfs
    /\ UNCHANGED <<bName, bState, rows, nextId, stubHigh, dirty, dur, rec, kin,
                   mints, probe, run, clean, countVars, badMade>>

\* The kernel evicts its inode for r (the last FORGET): a removed record
\* for it goes.
Evict(r) ==
    /\ Up /\ kin[r].held
    /\ kin' = [kin EXCEPT ![r] = NoK]
    /\ rec' = IF r \in Ids THEN [rec EXCEPT ![r] = None] ELSE rec
    /\ UNCHANGED <<bName, rows, nextId, stubHigh, dirty, dur, nfs, mints,
                   probe, run, clean, countVars, badMade>>
    /\ Freed(bState)

\* A sync point empties the dirty set (ClearDirty, not durable by itself).
Sync ==
    /\ Up /\ Idle /\ dirty # {}
    /\ dirty' = {}
    /\ UNCHANGED <<bName, bState, rows, nextId, stubHigh, dur, rec, kin, nfs,
                   mints, probe, run, clean, countVars, badMade>>

\* A durable commit (a phase 1 elsewhere, a WAL checkpoint).
Persist ==
    /\ Up /\ Track /\ dur # Durable
    /\ dur' = Durable
    /\ UNCHANGED <<bName, bState, rows, nextId, stubHigh, dirty, rec, kin, nfs,
                   mints, probe, run, clean, countVars, badMade>>

-----------------------------------------------------------------------------
(* A probe by name (ProbeByName): the statx, then the handle and the       *)
(* generation in ProbeOrder, each read resolving the name again, with      *)
(* anything else between them. Not the code, which reads all of it through *)
(* one descriptor (Lookup).                                                *)

ProbeStat(n) ==
    /\ Up /\ Idle /\ ProbeByName /\ bName[n] \in Files
    /\ probe' = [NoProbe EXCEPT !.n = n, !.st = bName[n],
                                !.ino = InoOf[bName[n]],
                                !.bt = Btime(bName[n]), !.step = 1]
    /\ UNCHANGED <<bName, bState, rows, nextId, stubHigh, dirty, dur, rec, kin,
                   nfs, mints, run, clean, countVars, badMade>>

ProbeRead ==
    /\ Up /\ probe.step \in {1, 2}
    /\ LET x == bName[probe.n]
           handle == (probe.step = 1) = (ProbeOrder = "handle_first")
           p == IF handle
                THEN [probe EXCEPT !.hino = InoOf[x], !.hgen = HGen(x)]
                ELSE [probe EXCEPT !.gen = VGen(x)]
       IN IF x \notin Files
          THEN \* The name vanished (ENOENT): nothing is recorded.
               /\ probe' = NoProbe
               /\ UNCHANGED <<rows, nextId, mints, kin, badMade>>
          ELSE IF probe.step = 1
          THEN /\ probe' = [p EXCEPT !.step = 2]
               /\ UNCHANGED <<rows, nextId, mints, kin, badMade>>
          ELSE /\ probe' = NoProbe
               /\ Record(probe.n, [ino |-> p.ino, hino |-> p.hino,
                                   hgen |-> p.hgen, gen |-> p.gen,
                                   bt |-> p.bt], probe.st)
    /\ UNCHANGED <<bName, bState, stubHigh, dirty, dur, rec, nfs, run, clean,
                   countVars>>

-----------------------------------------------------------------------------
(* Changes behind dcfs's back (OutOfBand), while it runs or not.           *)

OobUnlink(n) ==
    /\ OutOfBand /\ bName[n] # None
    /\ bName' = [bName EXCEPT ![n] = None]
    /\ UNCHANGED <<rows, nextId, stubHigh, dirty, dur, rec, kin, nfs, mints,
                   probe, run, clean, countVars, badMade>>
    /\ Freed(bState)

\* A new object at n, maybe on a recycled inode number (or a filesystem
\* mounted there: a boundary).
OobCreate(n) ==
    /\ OutOfBand /\ bName[n] = None
    /\ \E o \in Objs :
         /\ bState[o] = "unborn"
         /\ o \in Files =>
              ~\E x \in Files : bState[x] = "alive" /\ InoOf[x] = InoOf[o]
         /\ bName' = [bName EXCEPT ![n] = o]
         /\ bState' = [bState EXCEPT ![o] = "alive"]
    /\ UNCHANGED <<rows, nextId, stubHigh, dirty, dur, rec, kin, nfs, mints,
                   probe, run, clean, countVars, badMade>>

OobRename(n, m) ==
    /\ OutOfBand /\ n # m /\ bName[n] # None /\ bName[m] = None
    /\ bName' = [bName EXCEPT ![m] = bName[n], ![n] = None]
    /\ UNCHANGED <<bState, rows, nextId, stubHigh, dirty, dur, rec, kin, nfs,
                   mints, probe, run, clean, countVars, badMade>>

-----------------------------------------------------------------------------
(* Runs.                                                                   *)

\* The kernel's mount goes (a new one holds no inode) and dcfs's memory
\* (removed records, a probe in flight) with it.
Reset ==
    /\ kin' = [r \in AllIds |-> NoK]
    /\ rec' = [r \in Ids |-> None]
    /\ probe' = NoProbe
    /\ run' = "down"

\* The daemon dies: the database keeps what it committed.
Crash ==
    /\ Up /\ crashes < MaxCrashes
    /\ Reset /\ clean' = FALSE /\ crashes' = crashes + 1
    /\ bName' = bName /\ Freed(bState)
    /\ UNCHANGED <<rows, nextId, stubHigh, dirty, dur, nfs, mints, powers,
                   wipes, badMade>>

\* A power loss: the database rolls back to its last durable commit.
PowerLoss ==
    /\ Up /\ powers < MaxPowerLosses
    /\ Reset /\ clean' = FALSE /\ powers' = powers + 1
    /\ rows' = dur.rows /\ nextId' = dur.nextId /\ stubHigh' = dur.stubHigh
    /\ dirty' = dur.dirty
    /\ bName' = bName /\ Freed(bState)
    /\ UNCHANGED <<dur, nfs, mints, crashes, wipes, badMade>>

\* DESTROY and FinishRun: the sync point, a checkpoint, the clean flag.
Stop ==
    /\ Up /\ Idle
    /\ Reset /\ clean' = TRUE /\ dirty' = {}
    /\ dur' = IF Track THEN [Durable EXCEPT !.dirty = {}] ELSE dur
    /\ bName' = bName /\ Freed(bState)
    /\ UNCHANGED <<rows, nextId, stubHigh, nfs, mints, countVars, badMade>>

\* The cache database is deleted while dcfs is stopped.
Wipe ==
    /\ run = "down" /\ wipes < MaxWipes
    /\ rows' = [r \in AllIds |-> NoRow] /\ nextId' = 1 /\ stubHigh' = 0
    /\ dirty' = {}
    /\ dur' = IF Track
              THEN [rows |-> [r \in AllIds |-> NoRow], nextId |-> 1,
                    stubHigh |-> 0, dirty |-> {}]
              ELSE dur
    /\ clean' = TRUE /\ wipes' = wipes + 1
    /\ UNCHANGED <<bName, bState, rec, kin, nfs, mints, probe, run, crashes,
                   powers, badMade>>

\* The start (backing::Startup): RecoverDirty empties the dirty set,
\* StartRun's commit is durable, and after an unclean shutdown every row
\* that was dirty is probed by handle (ProbeRecoveredRows: OpenNode) and
\* goes if that resolves to ESTALE (no removed record exists yet, so a live
\* object is a named one: no nlink-0 case).
Start ==
    /\ run = "down"
    /\ run' = "up" /\ clean' = FALSE
    /\ dirty' = {}
    /\ dur' = IF Track THEN [Durable EXCEPT !.dirty = {}] ELSE dur
    /\ rows' = IF clean THEN rows
               ELSE [r \in AllIds |->
                       IF r \in dirty /\ rows[r].ex /\ ~rows[r].stub
                          /\ Resolve(r) = Estale
                       THEN NoRow ELSE rows[r]]
    /\ UNCHANGED <<bName, bState, nextId, stubHigh, rec, kin, nfs, mints,
                   probe, countVars, badMade>>

-----------------------------------------------------------------------------

Init ==
    /\ bName = InitBName
    /\ bState = [o \in Objs |->
                   IF \E n \in Names : InitBName[n] = o
                   THEN "alive" ELSE "unborn"]
    /\ rows = [r \in AllIds |-> NoRow]
    /\ nextId = 1
    /\ stubHigh = 0
    /\ dirty = {}
    /\ dur = Durable
    /\ rec = [r \in Ids |-> None]
    /\ kin = [r \in AllIds |-> NoK]
    /\ nfs = {}
    /\ mints = 0
    /\ probe = NoProbe
    /\ run = "up"
    /\ clean = FALSE
    /\ crashes = 0 /\ powers = 0 /\ wipes = 0
    /\ badMade = FALSE

Next ==
    \/ \E n \in Names : Lookup(n) \/ Refuse(n) \/ Create(n) \/ Unlink(n)
                        \/ ProbeStat(n) \/ OobUnlink(n) \/ OobCreate(n)
    \/ \E n, m \in Names : Rename(n, m) \/ OobRename(n, m)
    \/ \E r \in AllIds : Access(r) \/ NfsTake(r) \/ Evict(r)
    \/ \E h \in nfs : DotLookup(h)
    \/ ProbeRead \/ Sync \/ Persist
    \/ Crash \/ PowerLoss \/ Stop \/ Wipe \/ Start

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties.                                                             *)

\* The references to dcfs's nodeids held outside it: the kernel's inodes
\* (this mount) and the NFS client's handles (across mounts).
Refs == {[id |-> r, gen |-> kin[r].gen, obj |-> kin[r].obj] :
           r \in {s \in AllIds : kin[s].held}} \cup nfs

\* (1) One (nodeid, generation) never stands for two objects: dcfs never
\* hands out a pair the kernel or an NFS client holds for another object.
OneHandleOneObject ==
    \A h1, h2 \in Refs : h1.id = h2.id /\ h1.gen = h2.gen => h1.obj = h2.obj

\* ... and a nodeid the kernel holds resolves to the object it was handed
\* out for, or to ESTALE: never to another.
HeldResolvesToItsObject ==
    Up => \A r \in AllIds : kin[r].held => Resolve(r) \in {kin[r].obj, Estale}

\* ... nor does a handle the kernel accepts.
HandlesResolveToTheirObject ==
    Up => \A h \in nfs : Accepts(h) => Resolve(h.id) \in {h.obj, Estale}

\* (2) ESTALE exactly when gone or replaced: a handle of an object that
\* still exists is accepted and reaches it ...
ServedWhileLive ==
    (Up /\ Idle) => \A h \in nfs : Live(h.obj) =>
                       Accepts(h) /\ Resolve(h.id) = h.obj

\* ... and one of an object that is gone is not served (not for a stub
\* whose boundary went behind dcfs's back: answered from its row, until its
\* parent is listed again; docs/design.md "Boundary stubs").
GoneIsStale ==
    Up => \A h \in nfs :
            ~Live(h.obj) /\ ~(h.obj \in Names /\ OutOfBand)
              => ~Accepts(h) \/ Resolve(h.id) = Estale

\* (3) A row whose object is gone (its inode number maybe recycled)
\* resolves to ESTALE at every entry point: a reopen by handle (OpenNode,
\* and through it ParentOf and the start's probe) and a probe of a name
\* (UpsertInode, which must not take it over for another object).
ReuseDetected ==
    Up => \A r \in Ids :
            rows[r].ex /\ ~rows[r].stub /\ ~Live(rows[r].obj) =>
              /\ Resolve(r) = Estale
              /\ \A o \in Files :
                   bState[o] = "alive" => ~UpsertMatch(rows[r], IdentOf(o))

\* (4) dcfs never replies with a generation other than that of the inode
\* the kernel holds for the nodeid (fuse_iget would mark it bad: EIO).
NoBadInode == ~badMade

\* Every invariant the real configurations check (ServedWhileLive only
\* where nothing loses rows: see the configurations).
IdentSafe ==
    /\ OneHandleOneObject /\ HeldResolvesToItsObject
    /\ HandlesResolveToTheirObject /\ GoneIsStale /\ ReuseDetected
    /\ NoBadInode
=============================================================================
