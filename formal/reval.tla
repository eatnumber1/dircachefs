------------------------------- MODULE reval -------------------------------
(***************************************************************************)
(* The revalidation model: what dcfs reuses across a change of the        *)
(* backing filesystem's state that should have invalidated it. README.md   *)
(* ("The revalidation model") explains it; this comment says what is in    *)
(* it.                                                                     *)
(*                                                                         *)
(* dcfs holds things that grant something: a shared backing descriptor and *)
(* its access mode (every open of an inode shares one, opened as root),   *)
(* the write descriptor kept beside a read-only one, the cached mode that *)
(* the kernel's default_permissions check reads, and the cached negative  *)
(* entries and complete listings that answer lookups. Each was right when *)
(* it was made. The class of bug this model is about is reusing one after *)
(* the backing filesystem changed in a way that should have invalidated   *)
(* it: Phase 23 found two (a writable open reusing a read-write shared    *)
(* descriptor opened before chattr +i, and chattr +F making a cached      *)
(* directory case-insensitive under its byte-keyed entries).               *)
(*                                                                         *)
(* The invariant (the phase's): nothing dcfs holds grants more than the   *)
(* backing filesystem would grant at the moment of use, except where      *)
(* POSIX grandfathers it (a descriptor keeps the rights it was opened     *)
(* with). So a dcfs OPEN for writing succeeds exactly when the backing    *)
(* filesystem would let the caller open the file for writing at that      *)
(* moment (OpenExact), every writable open dcfs holds was legitimately    *)
(* granted (HeldFlagsLegit, HeldModeLegit), the descriptor dcfs writes    *)
(* through for an open can carry the write (WritesUseAWritableFd), and a  *)
(* cached lookup answer agrees with the backing directory's               *)
(* (DirCacheNeverWrong).                                                   *)
(*                                                                         *)
(* Objects: one regular file F (its permission state: whether its mode    *)
(* lets the caller write, and its immutable and append-only flags) and    *)
(* one directory D (its casefold flag and the names stored in it). The    *)
(* dcfs functions each action stands for are named on it.                 *)
(*                                                                         *)
(* Changes to the backing state come through dcfs (SETATTR of the mode,   *)
(* FS_IOC_SETFLAGS, creates and unlinks in D) or, if OutOfBand, behind    *)
(* its back. dcfs requires exclusive access to the backing filesystem     *)
(* (docs/design.md, "Assumptions"); OutOfBand = TRUE says what breaks     *)
(* without it: the flags part survives (the GETFLAGS re-check reads the   *)
(* backing file's flags, so it sees a chattr made by anyone), the cached  *)
(* mode and the cached directory answers do not (the configurations       *)
(* limitations/out_of_band_*.cfg check exactly that).                     *)
(*                                                                         *)
(* Abstractions:                                                           *)
(*  - Each request is one step (its effect point): dcfs serves one        *)
(*    request at a time, and a request concurrent with another may be     *)
(*    ordered before or after it, as on a local filesystem.               *)
(*  - The caller is one user; "mode" is whether the file's mode bits (and *)
(*    ACL) let that user write. dcfs opens backing files as root, so the  *)
(*    mode never refuses dcfs's own opens; only the kernel's              *)
(*    default_permissions check refuses for the mode, and it reads the    *)
(*    attributes the kernel has cached, or else asks dcfs (GETATTR).      *)
(*  - The kernel's attribute cache (kAttr) is model-only: no protocol     *)
(*    event observes it, and trace validation leaves it free.             *)
(*  - dcfs's own writes (fallback writes, fallocate, copy_file_range) go  *)
(*    through BackingFile::WriteFd(); passthrough writes are the kernel's *)
(*    and are allowed by the FUSE file's own mode, which is the open's.   *)
(*    A write is not re-checked against the flags (POSIX: an open         *)
(*    descriptor keeps its rights); where the kernel does check at write  *)
(*    time, that only refuses more.                                       *)
(*  - D's names are "a" and "A", which a case-insensitive (casefold)      *)
(*    directory treats as one. A complete listing records every name it   *)
(*    did not list as absent (no row and children_complete).              *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Handles,      \* dcfs's open file handles (fi.fh) of F
    OutOfBand,    \* whether the backing state may change behind dcfs's back
    MaxChanges,   \* bound on the changes of the backing state
    \* Historical bugs, each put back by one constant (known_bugs/reval_*):
    BugNoRecheck,             \* a writable open reuses the shared fd's mode
    BugRecheckOnlyIfChanged,  \* ... only after a flag change through dcfs
    BugCasefoldPassedThrough, \* SETFLAGS changing FS_CASEFOLD_FL is forwarded
    BugNoWriteFd,             \* no write fd beside a read-only shared fd
    BugWriteFdLastWriter,     \* each writable open replaces the write fd
    BugWriteFdNeverDropped    \* the last writable release keeps the write fd

None == "none"

-----------------------------------------------------------------------------
(* The backing file F, and what the backing filesystem would allow.        *)

\* An open's access mode: O_RDONLY; O_WRONLY or O_RDWR; either with
\* O_APPEND.
Modes == {"r", "w", "wa"}
Writes(m) == m # "r"

\* F's permission state: whether its mode lets the caller write (w), and
\* its FS_IMMUTABLE_FL and FS_APPEND_FL flags.
BStates == [w : BOOLEAN, imm : BOOLEAN, app : BOOLEAN]

\* The backing filesystem's open-time check of the flags (may_open): an
\* immutable file cannot be opened for writing, an append-only one only
\* with O_APPEND.
FlagsAllow(b, m) == Writes(m) => (~b.imm /\ (b.app => m = "wa"))
\* Its permission check on the mode (generic_permission), for the caller.
ModeAllows(b, m) == Writes(m) => b.w
\* Whether dcfs's own open O_RDWR (as root: no mode check) succeeds.
RootRW(b) == FlagsAllow(b, "w")

-----------------------------------------------------------------------------
(* The backing directory D.                                                *)

Names == {"a", "A"}
\* The casefolded form of a name.
Fold(x) == IF x \in {"a", "A"} THEN "a" ELSE x
\* Whether a lookup of x in the backing directory d finds something.
BLookup(d, x) ==
    IF d.cf THEN \E y \in d.ents : Fold(y) = Fold(x) ELSE x \in d.ents

-----------------------------------------------------------------------------

VARIABLES
    bF,        \* F's backing permission state (BStates)
    cF,        \* dcfs's cached attributes of F: [valid, w] (attrs_valid, mode)
    kAttr,     \* the kernel's cached attributes of F: [valid, w]; model-only
    sfd,       \* F's shared backing fd: [st: "none", "rw" or "ro", chg]
    wfd,       \* the write fd beside a read-only sfd: None, "plain", "append"
    hs,        \* hs[h]: dcfs's open handle h of F
    lastOpen,  \* the outcome of the last OPEN of F (history)
    writeErr,  \* "none", or how a write through dcfs went wrong (history)
    bD,        \* D on the backing filesystem: [cf: casefold, ents: names]
    cD,        \* dcfs's cached answer for each name of D
    changes    \* changes of the backing state so far (for the bound)

\* sfd.chg: a flag change went through dcfs since sfd was opened. The code
\* keeps no such thing; BugRecheckOnlyIfChanged's re-check would need it.
\* (Every variable keeps one type: TLC cannot compare a string with a
\* record, hence NoFd and NoOpen rather than None.)
\* hs[h] = [st: "open" or "closed", m: its mode, fl, ml: whether the
\* backing filesystem's flags and mode allowed it when it was granted].
\* lastOpen = [m: mode, by: who refused it ("granted": nobody; "kernel": the
\* default_permissions check, EACCES; "dcfs": the GETFLAGS re-check, EPERM;
\* "backing": the backing filesystem's own open, EPERM; "-": no OPEN yet),
\* fl, ml: whether the backing filesystem's flags and mode allowed it at
\* that moment].

fileVars == <<bF, cF, kAttr, sfd, wfd, hs, lastOpen, writeErr>>
dirVars == <<bD, cD>>
vars == <<bF, cF, kAttr, sfd, wfd, hs, lastOpen, writeErr, bD, cD, changes>>

Unknown == [valid |-> FALSE, w |-> FALSE]
Known(v) == [valid |-> TRUE, w |-> v]
Closed == [st |-> "closed", m |-> "r", fl |-> TRUE, ml |-> TRUE]
NoFd == [st |-> "none", chg |-> FALSE]
NoOpen == [m |-> "r", by |-> "-", fl |-> TRUE, ml |-> TRUE]
Held(s) == s.st # "none"
RW(s) == s.st = "rw"

OpenHandles == {h \in Handles : hs[h].st = "open"}
\* The writable opens: while there is one, F is in Context::open_for_write
\* and its cached attributes stay unknown (DirCacheFS::BeginWriting).
Writers == {h \in OpenHandles : Writes(hs[h].m)}

TypeOK ==
    /\ bF \in BStates
    /\ cF \in {Unknown} \cup {Known(v) : v \in BOOLEAN}
    /\ kAttr \in {Unknown} \cup {Known(v) : v \in BOOLEAN}
    /\ sfd \in [st : {"none", "rw", "ro"}, chg : BOOLEAN]
    /\ wfd \in {None, "plain", "append"}
    /\ hs \in [Handles -> {Closed} \cup
                 [st : {"open"}, m : Modes, fl : BOOLEAN, ml : BOOLEAN]]
    /\ lastOpen \in [m : Modes,
                     by : {"-", "granted", "kernel", "dcfs", "backing"},
                     fl : BOOLEAN, ml : BOOLEAN]
    /\ writeErr \in {"none", "ebadf", "appends"}
    /\ bD \in [cf : BOOLEAN, ents : SUBSET Names]
    /\ cD \in [Names -> {"present", "absent", "unknown"}]
    /\ changes \in 0..MaxChanges

-----------------------------------------------------------------------------
(* What the kernel's permission check reads.                              *)

\* What dcfs answers a GETATTR of F with: its cached attributes if valid,
\* else a fresh statx (DirCacheFS::FreshAttr; while F is open for writing,
\* a statx of the shared fd).
Served == IF cF.valid THEN cF.w ELSE bF.w
\* Whether the kernel's default_permissions check finds the mode lets the
\* caller write: from its cached attributes, or else a GETATTR.
Decision == IF kAttr.valid THEN kAttr.w ELSE Served
\* After a GETATTR (made when the kernel has nothing cached): dcfs records
\* what a fresh statx read (a fill), except while F is open for writing
\* (open_for_write keeps the attributes unknown); the kernel keeps the
\* answer for the attribute timeout, except while F is open for writing
\* (the timeout is 0 then: AttrTimeoutFor).
CacheAfterAsk ==
    IF ~kAttr.valid /\ ~cF.valid /\ Writers = {} THEN Known(bF.w) ELSE cF
KernelAfterAsk ==
    IF kAttr.valid THEN kAttr
    ELSE IF Writers = {} THEN Known(Served) ELSE Unknown

\* Whether a writable open sharing a read-write fd asks the backing
\* filesystem again (the GETFLAGS re-check of step 23.7, review L-b).
Recheck(s) ==
    /\ ~BugNoRecheck
    /\ BugRecheckOnlyIfChanged => s.chg

\* The write fd after an allowed writable open in mode m beside a read-only
\* shared fd: the first writer's, replaced only by one without O_APPEND
\* (review L-a).
WfdAfter(m) ==
    IF BugNoWriteFd THEN wfd
    ELSE IF wfd = None \/ BugWriteFdLastWriter \/ (wfd = "append" /\ m = "w")
         THEN IF m = "wa" THEN "append" ELSE "plain"
         ELSE wfd

-----------------------------------------------------------------------------
(* The file's steps.                                                       *)

\* An OPEN of F in mode m, as handle h: the kernel's permission check
\* (default_permissions), then DirCacheFS::Open.
OpenF(h, m) ==
    /\ hs[h].st = "closed"
    /\ UNCHANGED bF
    /\ LET fl == FlagsAllow(bF, m)
           ml == ModeAllows(bF, m)
           c1 == CacheAfterAsk
           k1 == KernelAfterAsk
           fresh == ~Held(sfd)
           \* The shared fd this open uses: one made now (MakeBackingFile:
           \* O_RDWR as root, falling back to O_RDONLY if the flags refuse
           \* it), or the one already there.
           rw == IF fresh THEN RootRW(bF) ELSE RW(sfd)
           by == CASE Writes(m) /\ ~Decision -> "kernel"
                   [] ~Writes(m) -> "granted"
                   \* An O_RDWR open made for this one: the backing
                   \* filesystem just allowed writing.
                   [] rw /\ fresh -> "granted"
                   \* A read-write fd that was already there: the GETFLAGS
                   \* re-check (EPERM as may_open would), unless skipped.
                   [] rw -> IF Recheck(sfd) /\ ~fl THEN "dcfs" ELSE "granted"
                   \* A read-only one: reopened through /proc/self/fd with
                   \* this open's mode, which the backing filesystem checks.
                   [] OTHER -> IF fl THEN "granted" ELSE "backing"
       IN /\ lastOpen' = [m |-> m, by |-> by, fl |-> fl, ml |-> ml]
          /\ kAttr' = k1
          /\ IF by = "granted"
             THEN /\ sfd' = IF ~fresh THEN sfd
                            ELSE [st |-> IF rw THEN "rw" ELSE "ro",
                                  chg |-> FALSE]
                  /\ wfd' = IF Writes(m) /\ ~rw THEN WfdAfter(m) ELSE wfd
                  /\ hs' = [hs EXCEPT ![h] = [st |-> "open", m |-> m,
                                               fl |-> fl, ml |-> ml]]
                  \* A writable open is phase 1 of the writes through it
                  \* (BeginWriting): F's attributes unknown until the last
                  \* writable release.
                  /\ cF' = IF Writes(m) THEN Unknown ELSE c1
             \* Refused: a shared fd made for this open is closed again.
             ELSE /\ UNCHANGED <<sfd, wfd, hs>>
                  /\ cF' = c1
    /\ UNCHANGED writeErr

\* A RELEASE of handle h (DirCacheFS::Release). The last writable one is
\* phase 3 of the writes (EndWriting, then RecordWrittenAttrs from the fd)
\* and drops the write fd; the last one closes the shared fd, refreshing
\* unknown attributes from it first.
ReleaseF(h) ==
    /\ hs[h].st = "open"
    /\ UNCHANGED bF
    /\ LET rest == OpenHandles \ {h}
           lastWriter == Writes(hs[h].m) /\ \A g \in rest : ~Writes(hs[g].m)
       IN /\ hs' = [hs EXCEPT ![h] = Closed]
          /\ cF' = IF lastWriter \/ (rest = {} /\ ~cF.valid)
                   THEN Known(bF.w) ELSE cF
          /\ wfd' = IF lastWriter /\ ~BugWriteFdNeverDropped THEN None ELSE wfd
          /\ sfd' = IF rest = {} THEN NoFd ELSE sfd
    /\ UNCHANGED <<kAttr, lastOpen, writeErr>>

\* dcfs writes for handle h through BackingFile::WriteFd() (a fallback
\* WRITE, FALLOCATE, COPY_FILE_RANGE): the shared fd if read-write, else
\* the write fd. None: EBADF (review L1). An O_APPEND one for an open
\* without O_APPEND: copy_file_range refuses it and pwrite lands at the
\* end (review L-a).
WriteF(h) ==
    /\ hs[h].st = "open" /\ Writes(hs[h].m)
    /\ UNCHANGED bF
    /\ LET fd == IF RW(sfd) THEN "rw" ELSE wfd
       IN writeErr' = CASE fd = None -> "ebadf"
                        [] fd = "append" /\ hs[h].m = "w" -> "appends"
                        [] OTHER -> writeErr
    /\ UNCHANGED <<cF, kAttr, sfd, wfd, hs, lastOpen>>

\* A GETATTR of F (a stat, or the attributes of a LOOKUP reply).
GetattrF ==
    /\ ~kAttr.valid
    /\ UNCHANGED bF
    /\ cF' = CacheAfterAsk
    /\ kAttr' = KernelAfterAsk
    /\ UNCHANGED <<sfd, wfd, hs, lastOpen, writeErr>>

\* A chmod through dcfs (DirCacheFS::Setattr: phase 1, the syscall, a
\* refresh); the reply's attributes go into the kernel's cache, for the
\* attribute timeout (0 while F is open for writing). The backing
\* filesystem refuses a mode change of an immutable or append-only file.
ChmodF(v) ==
    /\ ~bF.imm /\ ~bF.app
    /\ bF' = [bF EXCEPT !.w = v]
    /\ cF' = IF Writers = {} THEN Known(v) ELSE Unknown
    /\ kAttr' = IF Writers = {} THEN Known(v) ELSE Unknown
    /\ UNCHANGED <<sfd, wfd, hs, lastOpen, writeErr>>

\* An FS_IOC_SETFLAGS of F through dcfs (DirCacheFS::Ioctl: a mutation of
\* its attributes, then a refresh). The kernel's cached attributes are not
\* invalidated (a kernel gap, docs/design.md; the mode does not change).
SetFlagsF(imm, app) ==
    /\ bF' = [bF EXCEPT !.imm = imm, !.app = app]
    /\ cF' = IF Writers = {} THEN Known(bF.w) ELSE Unknown
    /\ sfd' = IF Held(sfd) THEN [sfd EXCEPT !.chg = TRUE] ELSE sfd
    /\ UNCHANGED <<kAttr, wfd, hs, lastOpen, writeErr>>

\* A SETATTR or SETFLAGS through dcfs that the backing filesystem refused:
\* its phase 1 and its refresh, nothing else (for trace validation).
FailedAttrChangeF ==
    /\ UNCHANGED bF
    /\ cF' = IF Writers = {} THEN Known(bF.w) ELSE Unknown
    /\ UNCHANGED <<kAttr, sfd, wfd, hs, lastOpen, writeErr>>

\* An FS_IOC_GETFLAGS through dcfs: reads the backing file, changes nothing.
GetFlagsF ==
    /\ UNCHANGED bF
    /\ UNCHANGED <<cF, kAttr, sfd, wfd, hs, lastOpen, writeErr>>

-----------------------------------------------------------------------------
(* The directory's steps.                                                  *)

\* A lookup of x that the cache cannot answer: a probe, recorded
\* (backing::ResolveName).
LookupD(x) ==
    /\ cD[x] = "unknown"
    /\ cD' = [cD EXCEPT ![x] = IF BLookup(bD, x) THEN "present" ELSE "absent"]
    /\ UNCHANGED bD

\* A complete listing (backing::PopulateDirectory): the names getdents64
\* returned are present, every other name absent.
ListD ==
    /\ cD' = [x \in Names |-> IF x \in bD.ents THEN "present" ELSE "absent"]
    /\ UNCHANGED bD

\* A create of x through dcfs (phase 1, the syscall, phase 3); EEXIST if a
\* lookup of x finds something, and then the name is resolved again.
CreateD(x) ==
    /\ IF BLookup(bD, x) THEN UNCHANGED bD
       ELSE bD' = [bD EXCEPT !.ents = @ \cup {x}]
    /\ cD' = [cD EXCEPT ![x] = "present"]

\* An unlink of x through dcfs: removes what a lookup of x finds.
UnlinkD(x) ==
    /\ BLookup(bD, x)
    /\ bD' = [bD EXCEPT !.ents = {y \in @ : ~(IF bD.cf THEN Fold(y) = Fold(x)
                                                ELSE y = x)}]
    /\ cD' = [cD EXCEPT ![x] = "absent"]

\* An FS_IOC_SETFLAGS of D changing FS_CASEFOLD_FL through dcfs. dcfs
\* refuses it (EOPNOTSUPP, review M1): a stutter. With the bug it is
\* forwarded, and the backing filesystem allows it only on an empty
\* directory (ENOTEMPTY otherwise); the refresh changes no entry.
SetCasefoldD(v) ==
    /\ v # bD.cf
    /\ BugCasefoldPassedThrough /\ bD.ents = {}
    /\ bD' = [bD EXCEPT !.cf = v]
    /\ UNCHANGED cD

-----------------------------------------------------------------------------
(* The specification.                                                      *)

Init ==
    /\ bF \in BStates
    \* Whatever dcfs has cached is right (the main model's business).
    /\ cF \in {Unknown, Known(bF.w)}
    /\ kAttr \in {Unknown, Known(bF.w)}
    /\ sfd = NoFd /\ wfd = None
    /\ hs = [h \in Handles |-> Closed]
    /\ lastOpen = NoOpen /\ writeErr = "none"
    \* dcfs refuses casefolded directories (Phase 16); D starts without.
    /\ bD \in [cf : {FALSE}, ents : SUBSET Names]
    /\ cD \in {c \in [Names -> {"present", "absent", "unknown"}] :
                 \A x \in Names : c[x] # "unknown" =>
                   ((c[x] = "present") = BLookup(bD, x))}
    /\ changes = 0

Change == changes < MaxChanges /\ changes' = changes + 1

Open(h, m) == OpenF(h, m) /\ UNCHANGED <<dirVars, changes>>
Release(h) == ReleaseF(h) /\ UNCHANGED <<dirVars, changes>>
Write(h) == WriteF(h) /\ UNCHANGED <<dirVars, changes>>
Getattr == GetattrF /\ UNCHANGED <<dirVars, changes>>
Chmod(v) ==
    /\ v # bF.w /\ Change /\ ChmodF(v) /\ UNCHANGED dirVars
SetFlags(imm, app) ==
    /\ <<imm, app>> # <<bF.imm, bF.app>>
    /\ Change /\ SetFlagsF(imm, app) /\ UNCHANGED dirVars

\* The kernel's attribute timeout passes.
Expire ==
    /\ kAttr.valid /\ kAttr' = Unknown
    /\ UNCHANGED <<bF, cF, sfd, wfd, hs, lastOpen, writeErr, dirVars, changes>>
\* dcfs forgets F's cached attributes (cache::InvalidateInode, a cache
\* rebuild), or D's answer for a name.
EvictF ==
    /\ cF.valid /\ cF' = Unknown
    /\ UNCHANGED <<bF, kAttr, sfd, wfd, hs, lastOpen, writeErr, dirVars,
                   changes>>
EvictD(x) ==
    /\ cD[x] # "unknown" /\ cD' = [cD EXCEPT ![x] = "unknown"]
    /\ UNCHANGED <<fileVars, bD, changes>>

DirStep(x) ==
    \/ LookupD(x) /\ UNCHANGED <<fileVars, changes>>
    \/ Change /\ (CreateD(x) \/ UnlinkD(x)) /\ UNCHANGED fileVars
DirChange(v) == Change /\ SetCasefoldD(v) /\ UNCHANGED fileVars

\* A change behind dcfs's back: a chmod or chattr of F, or chattr +F/-F of
\* the (empty) directory D, made directly on the backing filesystem.
OutOfBandChange ==
    /\ OutOfBand /\ Change
    /\ \/ \E b \in BStates : b # bF /\ bF' = b /\ UNCHANGED dirVars
       \/ bD.ents = {} /\ bD' = [bD EXCEPT !.cf = ~@] /\ UNCHANGED <<bF, cD>>
    /\ UNCHANGED <<cF, kAttr, sfd, wfd, hs, lastOpen, writeErr>>

Next ==
    \/ \E h \in Handles, m \in Modes : Open(h, m)
    \/ \E h \in Handles : Release(h) \/ Write(h)
    \/ Getattr \/ Expire \/ EvictF
    \/ \E v \in BOOLEAN : Chmod(v)
    \/ \E imm, app \in BOOLEAN : SetFlags(imm, app)
    \/ \E x \in Names : DirStep(x) \/ EvictD(x)
    \/ ListD /\ UNCHANGED <<fileVars, changes>>
    \/ \E v \in BOOLEAN : DirChange(v)
    \/ OutOfBandChange

\* The kernel's attribute timeouts do expire.
Spec == Init /\ [][Next]_vars /\ WF_vars(Expire)

-----------------------------------------------------------------------------
(* Properties.                                                             *)

\* A dcfs OPEN succeeds exactly when the backing filesystem would let the
\* caller open F in that mode at that moment.
OpenExact ==
    lastOpen.by # "-" =>
      ((lastOpen.by = "granted") <=> (lastOpen.fl /\ lastOpen.ml))
\* Its two halves. The flags: once past the kernel's check, dcfs grants an
\* open exactly when the backing file's flags allow it (what the shared fd
\* could otherwise grant past). The mode: the kernel's check, which reads
\* attributes cached by dcfs and the kernel, refuses exactly when the
\* backing file's mode does.
OpenFlagsExact ==
    lastOpen.by \notin {"-", "kernel"} =>
      ((lastOpen.by = "granted") <=> lastOpen.fl)
OpenModeExact ==
    lastOpen.by # "-" => ((lastOpen.by = "kernel") <=> ~lastOpen.ml)
\* The direction that grants more than the backing filesystem would: an
\* open granted although the mode refuses it.
OpenModeNeverGrantsMore == lastOpen.by = "granted" => lastOpen.ml
\* Every writable open dcfs holds was granted legitimately (POSIX
\* grandfathers it after a later change: it is checked as of its grant).
HeldFlagsLegit == \A h \in Writers : hs[h].fl
HeldModeLegit == \A h \in Writers : hs[h].ml
\* Every write dcfs made for an open went through a descriptor that can
\* carry it, at the offset asked for.
WritesUseAWritableFd == writeErr = "none"
\* The state behind it: with a writable open, dcfs has a descriptor to
\* write through, and one that does not append if some writable open does
\* not.
WriteFdHeld ==
    Writers # {} => (RW(sfd) \/ (Held(sfd) /\ wfd # None))
\* ... and only then: the write fd exists only beside writable opens over a
\* read-only shared fd (it goes with the last writable release).
WriteFdOnlyBesideWriters == wfd # None => (sfd.st = "ro" /\ Writers # {})
WriteFdKeepsOffsets ==
    (sfd.st = "ro" /\ \E h \in Writers : hs[h].m = "w")
      => wfd = "plain"
\* A cached answer for a name of D agrees with a lookup on the backing
\* directory (negative entries and complete listings included).
DirCacheNeverWrong ==
    \A x \in Names :
      cD[x] # "unknown" => ((cD[x] = "present") = BLookup(bD, x))
\* dcfs's cached mode, and the kernel's (model-only), agree with the
\* backing file's.
CachedModeCurrent == cF.valid => cF.w = bF.w
KernelModeCurrent == kAttr.valid => kAttr.w = bF.w

\* The phase's invariant, all of it.
NothingGrantsMore ==
    /\ OpenExact /\ HeldFlagsLegit /\ HeldModeLegit
    /\ WritesUseAWritableFd /\ DirCacheNeverWrong

\* Eventual consistency of what dcfs and the kernel have cached: once the
\* changes stop, the decision the kernel's permission check would take and
\* every cached answer agree with the backing filesystem from some point
\* on. With changes only through dcfs it is never otherwise (each change
\* refreshes or invalidates what it touches); with an out-of-band change, a
\* cached record stays wrong until something happens to invalidate it,
\* which nothing has to (limitation_out_of_band_stale.cfg).
CachedDecisionsConverge ==
    <>[](/\ Decision = bF.w
         /\ CachedModeCurrent
         /\ DirCacheNeverWrong)
=============================================================================
