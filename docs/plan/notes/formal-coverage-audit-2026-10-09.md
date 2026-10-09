# Formal coverage audit (step 12.12), 2026-10-09

Read-only audit of what `formal/` (dcfs.tla, reval.tla, lifetime.tla,
ident.tla and the four trace specs) covers against what the C++ does and
what the tests check. Main at 5afb814 (code and `formal/` unchanged since:
4567080 and 3f2011e are plan-only); the unmerged 23.10 model at
`step-23.10` (d49aa87, lane-1, based on 638590d, whose `formal/` equals
main's but for `trace.bzl`). No Bazel and no TLC were run: every claim that
a variant would or would not produce a counterexample is marked
**(needs a run)** with the target that would settle it. Guest-trace numbers
quoted below come from a lane's test logs of 2026-10-09 04:16 (output base
8a1dcc43..., branch not recorded), read from disk.

Risk order used throughout: crash safety (a power loss or crash leaves the
cache ahead of or behind the backing filesystem), then consistency (a wrong
answer while running), then diagnostics (logs, reply details, cost).

## Summary

1. The protocol of **writable opens** (BeginWriting's phase 1 that ends at
   once, `open_for_write` keeping attributes unknown, `EndWrites` as a guard
   event, ClearDirty keeping open-for-write rows, the last release's record,
   ReconcileWritten at the last FORGET) is the largest crash-relevant code
   with **no model and no trace**; it rests on three harness tests, the
   guest crash.sh and the checker's writable-open rule. 12.11b cannot
   validate file traces until `dcfs.tla`'s F has writable opens modelled the
   way the code does them (not 23.10's "fset spans the open").
2. Under `KernelDirLock` the concurrency guards (`Owns`, U1/R1's
   verification, the fills' `CanFill` for D's lookups and listings) are
   **vacuous**, and the medium tier checks the real model only with the
   lock. `EffectAtSyscall`, `BackingAtSyscall` and `CacheLearnsAtCommit` are
   **never model-checked without the lock** (MC_nolock and
   MC_interrupt_nolock check only `ReplyObservable`); only the harness
   traces check them there.
3. The **database half of crash safety** (two durability levels, the WAL
   keeps a prefix) is assumed by every crash property and never tested
   against storage that reorders: the guests' cuts drop writes in order, and
   the harness cannot roll the database back at all. 12.14 should use
   dm-log-writes replay, not drop-writes.
4. A **created object's row is unguarded until phase 3**: a fill of the
   parent between the create's syscall and its phase 3 records the new row
   clean with valid attributes at normal durability (RecordChild's
   `CanFill` of a never-touched row is true). A power loss that keeps that
   commit and loses both phase 3 and the create leaves a clean row of an
   object the backing filesystem lost, answered by nodeid (an NFS handle's
   `LOOKUP(".")`, GETATTR) until an open gets ESTALE. Unreachable with the
   kernel's directory lock; reachable in the harness and under parallel
   dirops. No model (children are not modelled), no test. It is exactly the
   case 23.11's born-dirty argument ("the row and its mark in one commit")
   must handle.
5. 22 properties have **no variant that fails them** (list in section 3);
   most have a cheap one. Two are decorative as modelled (`clean_shutdown`
   changes nothing in dcfs.tla; the completeness **epoch is vacuous** in the
   model, load-bearing in code only for the out-of-band relist).
6. The model checks **safety only**: any change that makes the cache know
   *less* (an End after its refresh, a skipped commit) violates nothing.
   8.2's mutation survivors were this class; 12.13 will report many more.
   Progress properties (a fill whose guard holds records) would kill them.
7. **Trace validation** compares the cached state of one directory after
   every event, the guard decisions, the errno class and a lookup's answer.
   It cannot see files, children's rows and dirty marks, attribute values,
   listings sent, xattrs, cross-directory steps, or (in guests) any
   concurrency at all. Its guard-decision checks have **no negative test**.
   Three of four guest root traces end early at a link or a cross-directory
   rename (crash.sh's at the run's trace line 127 of 261, rename.sh's at 236 of 686,
   create.sh's at 110 of 302); a per-directory projection removes those cuts.
8. Of 23.10: merge F's names and hard link (`bCur.fl`, `dbCur.fDent`,
   `FLive`), the ghost `fs` (two-sided `FileOK`), the ext4 item ordering and
   its cheaper enumeration, `GuardsBalanced` over E, and the three named
   conditions with their premise tests, re-expressed per inode. Keep the
   three "known parents" counterexamples as known_bugs once F has names.
   Drop the home, directory marks, three-way `RecoveredFile`, recovery over
   the backing state, and the `dirset_*` configurations.
9. Order: 12.13's tool and a small "oracle hygiene" step (12.12a) in
   parallel with 12.14 and 12.17; 12.13's first sweep after 23.8b settles
   the VIEW question; then 23.11 (model), 12.11b (with F's writable opens),
   12.11c in two halves (projection first), 12.15, 12.10 (crash-point replay
   first), 12.9, the joint two-directory trace, 12.16 last.

## 0. Ranked gap list

Each: the gap, why it ranks there, the proposed step.

| # | Risk | Gap | Why | Step |
|---|---|---|---|---|
| G1 | High (crash) | Writable opens: BeginWriting (a mutation that ends at once), `open_for_write` keeping attributes unknown (WriteAttrs, FillHeldAttrs), `EndWrites`'s touch, ClearDirty's keep of `open_for_write` at the snapshot or now, the last writable Release's record, Flush/Fsync's store-but-unknown, ReconcileWritten (phase 1 with no syscall, refresh through the held fd) and DESTROY's reconciliation: no model (dcfs.tla's F is read-only; reval.tla has the fds, lifetime.tla the `written_` entry, neither the cache records), no trace (files are not traced; Write, Fallocate and CopyFileRange emit no `MutationSyscallStarting`/`MutationSyscall`) | every write goes through it; its soundness is an argument in design.md plus ReleaseDuringASyncPointKeepsTheDirtyRow, ReleaseEndsTheWritesForAFillThatBeganBefore, WritableCreateIsDirtyWhenReplied, crash.sh and the writable-open checker rule | 12.11b's prerequisite: F's writable opens in dcfs.tla with the code's mechanism (a `wopen` count, not an in-flight mutation), passthrough writes as kernel steps changing F's attributes, EndWrites, the keep rule, the record; known_bugs EndWrites without touch, keep not applied at the snapshot, record before EndWrites, BeginWriting not durable; then the events above and a file trace |
| G2 | High (crash) | The database's durability abstraction (`Commit(new, sync)`: a normal commit may be lost, a synced one not, survivors are a prefix) is never tested where writes reorder; fault_power/fault_ace drop writes in order ("neither drop-writes nor a kill loses a write the disk acknowledged"), and the harness has no database rollback (its "power loss" is Restart over a hand-edited backing) | every crash property quantifies over `dbOpts`; a wrong abstraction voids all of them silently | 12.14 with dm-log-writes replay of FLUSH/FUA-respecting subsets (CrashMonkey's method) on the cache disk; plus a harness facility that copies the WAL at each transaction (the `SqliteTransaction` hook exists) and restores a chosen one, for 12.10 |
| G3 | High (model soundness) | VIEW exactness: MC_atime's distinct-state count depends on the worker count with `FSnapView` (23.10's finding); if the cause is `IdleView` or `CrashImage`, every VIEW configuration may skip reachable states. `tlc_test` runs `-workers auto` | a view that merges non-equivalent states hides counterexamples; expected counterexamples are unaffected, "no error" results are not | 23.8b (running in lane-1); 12.13's first sweep and 23.11's model results wait for it |
| G4 | Medium-high (concurrency, tiering) | Under the kernel lock `Owns` is always true, U1/R1's verification always passes, and the fills of D's lookups and listings never meet a mutation: their mutants survive the medium tier (the nolock known_bugs pass whatever else breaks). The effect-point properties are never model-checked without the lock | the coroutine future is where these guards matter; presubmit does not check them; 12.13's per-mutant budget cannot afford the large tier | `MC_nolock_small` (medium tier: 1-2 names, 2 slots, 2 mutations, no crash, all invariants and the four action properties); add `EffectAtSyscall`, `BackingAtSyscall`, `CacheLearnsAtCommit` to MC_nolock |
| G5 | Medium (crash, latent) | The created object's row is outside the create's mutation (`BeginCreate`'s ids are `{parent}`): a listing of the parent (or a resolve of the new name) between the create's syscall and RecordNewChild upserts the new row with valid attributes and no dirty mark, at normal durability (RecordChild: `child_ok` is true for a never-touched row); the crash state "listing committed, phase 3 lost, create lost on the backing" leaves a clean, valid row of a nonexistent object, served by nodeid | unreachable today (one thread, and the kernel's lock keeps D's lookups and listings out of a create in D); reachable in the harness (NameToHandleHook inside RecordNewChild's probe) and under parallel dirops; it breaks 23.11's premise | 23.11's model: F's row may be created by a fill before phase 3 (nolock); code candidates: RecordChild records a child's attributes only if `dir_ok` too, or marks the row dirty when its parent is in flight; harness test with NameToHandleHook running a Readdir inside Mkdir's probe |
| G6 | Medium (consistency) | Syscall failures other than create's EEXIST and unlink's/rename's ENOENT (ENOTEMPTY, EACCES, EBUSY, EXDEV, EPERM) are not in the model and cut traces (`failed`); `UnlinkFailed`, `RenameFailed`, `RenameFailed2` are dead in every configuration and every trace | the failure path (End, ReresolveAfterFailure, reply) is common (rmdir of a non-empty directory) and is checked only by guest scripts' results | model "a syscall that fails without changing anything" (errno class `other`) for create, unlink, rename, attrchange; the recorder stops cutting them (12.11c, first half) |
| G7 | Medium (consistency) | Links into a directory, cross-directory renames and rename flags cut traces; crash.sh's root trace ends at its first cross-directory rename, rename.sh's and create.sh's at their first link | most of each guest run's root directory is never validated | 12.11c first half: per-directory projection onto the one-directory model (below, section 7) |
| G8 | Medium (oracle) | 22 properties with no failing variant (section 3) | a property nothing shows can fail may be vacuous or mis-stated | 12.12a (new): premise configurations reusing existing variants, a few new ones |
| G9 | Medium (oracle) | Safety only: "unknown is always safe", so a fill that never records, an End after its refresh, a phase 3 that records nothing all pass | 8.2's survivors (End after refresh) were caught only by harness tests; 12.13 will flood with these | progress action properties: a fill whose guard holds commits (`RefreshRecords`, `ResolveRecords`, `PopulateRecords`), a phase 3 that `Owns` records; with known_bugs from 8.2's survivors (`attr_change_end_skipped` already) |
| G10 | Medium (oracle) | Trace.tla's guard-decision checks (`FillOK`'s `recorded`, `OwnedOK`, `SyncedOK`, populate's `recorded`, `LookupOutcome`, `T_Recover`'s `dirty_keys`) have no negative test: the fault builds test ordering (phase 1 vs syscall, snapshot vs syncfs) and a swallowed error only | a recorder or Trace.tla change that stops comparing a decision would pass every test | hand-edited negative logs in `formal/trace_tests/` (as the `reply_*` ones), and Trace*.tla in 12.13's scope |
| G11 | Medium (trace scope) | Trace.cfg checks `GuardsBalanced` and the four action properties only; `TriState`, `CacheNeverWrong`, `CrashSafe`, `DurableSetSound`, `CleanMeansNoDirty` are not checked on traced behaviors, which exceed the model-checking bounds (dozens of names, many mutations, three crashes in one trace) | any violation there is a real model violation outside the bounds, found for free | add them to Trace.cfg (and TLC time per trace stays small) |
| G12 | Medium (plan drift) | 12.2's "the Phase 11 crash, stress and failure tests and the fsstress runs also run with the recording build": only crash.sh, power.sh, rename.sh, create.sh are traced; fault_power and fault_ace (real power cuts, real database rollback) and stress are not | real crash recoveries are never matched against `dbOpts`; fsstress's sequential variety never reaches the model | after G6/G7: traced fault_power and a short seeded fsstress (large tier) |
| G13 | Medium-low (model fidelity) | The completeness epoch is vacuous in the model: only `RecoverD` (and `BugRestoreComplete`) bumps it, and no population spans a crash; in the code it is bumped by RecoverDirty (equally vacuous) and by `ForgetNegativeDentries` (ReconcileAttrs's out-of-band relist, which does not touch the guard), where it is load-bearing | 12.13 will report `dbCur.epoch = r.esnap` as a survivor; the reason is out-of-band | accepted, documented as an equivalent mutant with that reason; or model ReconcileAttrs's relist under an `OutOfBand` flag as reval.tla does |
| G14 | Medium-low (consistency) | Xattr records (per-name tri-state, FillXattr(s) guards, Setxattr's read-back under `Owns`, ForgetXattr in phase 1, side-effect xattrs of setattr and writes, ACL and mode coupling, RecoverDirty deleting a dirty inode's xattr rows) and symlink targets (FillSymlink): no model, no checker rule, no trace field | same protocol shape as names, so a model adds little; the side-effect rules are where bugs would be | accepted for the model; a checker rule in 12.15 (xattr rows of an inode with a mutation in flight are unknown); 12.11b's file trace shows xattr changes as attribute changes |
| G15 | Medium-low (crash, latent) | ClearDirty's fast path (`nothing_moved`: one `DELETE FROM dirty`, put back kept rows) rests on "rows are added only by a phase 1, a phase-3 MarkDirty before its End, or MarkAtimeDirty"; RecordTmpfile's MarkDirty has no mutation, no touch and no `inserts` count | unreachable today (no suspension point between RecordTmpfile and BeginWriting); the model has only the per-row clear | harden in code (MarkDirty counts in `ctx.dirty.inserts`) with a metadata_cache_test case; or accept with the reason |
| G16 | Medium-low (drift) | Phase 13 (running, lane-2, told to keep off `formal/`) changes OpenNode: a present dentry opens by name under the connected parent, an out-of-band mismatch falls back to the handle. ident.tla's `Access`/`Resolve` and IdentTrace's `resolve` outcomes model the handle path only | AGENTS: a protocol change updates its model in the same change; identity resolution is ident.tla's subject | ident.tla update in Phase 13's review or right after it (I did not read lane-2) |
| G17 | Medium-low (oracle) | `GuardsBalanced` counts D's guard only; F's setattr (`fset`) never sets `mseq`, so a skipped `FileSetEnd` leaves `fm.inflight` raised unseen (23.10's review, item 8) | the file analogue of 8.2's survivors | `GuardsBalanced` adds `fm.inflight`; known_bug `file_setattr_end_skipped` |
| G18 | Low-medium | Directory streams: cookies are dentry rowids, a continuation is a readdir, PutDentry's upsert keeps rowids (ListDirOrderSurvivesStateChanges, ListDirCursorSurvivesRenameOverAndFailedRemove, readdir_boundary.sh) | no model of "untouched entries exactly once" | 12.9 as planned, after 12.11c |
| G19 | Low | `clean_shutdown` is decorative in dcfs.tla (StartRun always recovers); the code gates ProbeRecoveredRows on `unclean or recovered > 0` and FinishRun leaves the flag unset with dirty rows; `CleanMeansNoDirty` and the kSync of StartRun and StopFlag are unconstrained | lifetime.tla does gate its probe on `clean` | model the probe's gate in dcfs.tla, or document the flag as decorative there |
| G20 | Low | File, nodeid and identity traces come only from the harness (the guest recorder writes none): the real kernel's FORGET counts, `LOOKUP(".")` reconnections and NFS handles are unvalidated | removed.sh, handles.sh, nfs.sh check results | accepted for now (README states it) |
| G21 | Low | Interrupts of F's requests, of sync points (Fsync/Fsyncdir checkpoint before their sync point) and of a cold open are not modelled | nothing changed before those checkpoints | accepted, documented |
| G22 | Low | FillGuards' `floor` pruning; ParentOf of a non-root directory; inode recycling inside dcfs.tla; out-of-band changes in dcfs.tla; rename of two links of one inode | tested (ClearDirtyKeepsEverythingPastTheFloor, ParentOf tests, ident.tla) or unreachable from the kernel | accepted, documented (all are in formal/README.md's abstractions already) |
| G23 | Low (diagnostics) | Reply contents beyond the errno class and a lookup's answer: an entry's nodeid and attributes, the entries a readdir sends, attribute values; EntryAfterPhase2's fallback replies the row's last attributes (timeout 0) | a reply built from the wrong row passes if the cache state matches | readdir entries in the reply line with 12.9; attributes accepted |
| G24 | Low | RecoveryIdempotent with a crash during recovery is checked under `seq` only (MC_recovery); MC_crash_ext4/metaprefix have one crash | the regime argument covers it | optional config |

## 1. Coverage matrix

Columns: **Model** (module: action), **Property** (which constrains it; kb =
a known_bug fails it, lim = a limitation, none = no variant), **Trace**
(event line, `Trace.tla` action), **Tests** (harness `dcfs:dir_cache_fs_test`
unless marked; guest scripts end in `.sh`), **Runtime** (checker rule,
`dcfs/testonly/invariant_checker.h`), **Gap**. An empty cell is "-".

### Mutations

| Element | Model | Property | Trace | Tests | Runtime | Gap |
|---|---|---|---|---|---|---|
| MKNOD/MKDIR/SYMLINK/CREATE in D: phase 1, syscall, probe, phase 3, refresh | dcfs: Arrive create (C1From), CreateSyscall, CreateProbe, CreatePhase3, CreateStat/Fill, CreateFailed | TriState (kb crash_f3), CrashSafe/CacheNeverWrong (kb crash_f1), EffectAtSyscall (kb effect_after_syscall), ReplyObservable (kb reply_after_failed_syscall) | phase1, syscall, probe, end, stat, fill, reply: T_ArriveCreate ... T_CreateFill, T_CreateFailed; faults skip_mark_unknown, create_syscall_before_phase1 | CreateMarksItsNameUnknown, Create*RepliesEexist/Enoent, MkdirDuringASyncPointKeepsItsDirtyRows; create.sh, fault_backing.sh, fault_power.sh, fault_ace.sh | dirty-set (in flight implies dirty), no-transaction | the new object's own row (G5); failures other than EEXIST (G6) |
| LINK of an unnamed O_TMPFILE file; LINK of a closed one (LinkRemoved, 23.9) | dcfs: linkcreate (C_sys straight to C_rec) | as create | T_ArriveCreate (linkcreate), T_CreatePhase3 | TmpfileLinkedIntoANameIsACreate, LinkOfAClosedTmpfileGivesItsNodeidAName, LinkOfARemovedObjectAnswersAsTheBacking; copy.sh | as create | - |
| UNLINK/RMDIR | dcfs: LK, UnlinkPhase1 (verify, retry, EAGAIN, ENOENT), UnlinkSyscall, UnlinkPhase3, UnlinkStat/Fill, UnlinkFailed (dead) | CacheNeverWrong, TriState, EffectAtSyscall (kb effect_before_syscall), CrashSafe (kb recovery_clears_dirty) | lookup, resolved, phase1 (begun/aborted), syscall, end, ...; faults syscall_before_phase1, phase3_before_syscall, swallow_syscall_error | UnlinkMarksWhatItRemovesUnknown, UnlinkGivesUpWhileItsParentKeepsChanging, CrashBetweenUnlinkAndPhase3LeavesNoRow; rename.sh, removed.sh | dirty-set | the child's row (nlink, SettleUnlinkedFile, HoldForRemoval): lifetime.tla only; ENOTEMPTY/EBUSY (G6); verification failing for the child alone is a step the model forbids (README finding) |
| RENAME within D, flags 0 | dcfs: RenameResolveDst, RenamePhase1 (verify), RenameSyscall, RenamePhase3, RenameStat/Fill, RenameFailed/2 (dead) | CacheNeverWrong (kb rename_stale_source, nolock), TriState (kb interrupt_undo) | T_RenameResolveDst ... T_RenameFill | RenameStaleSourceTest.*, RenameGivesUpWhile..., RenameOverAnOpenFileRetiresItAtTheLastRelease; rename.sh, pjdfstest_rename.sh | dirty-set | the replaced object's row (lifetime.tla's Remove(n, src) only); a moved directory |
| RENAME across directories, RENAME_EXCHANGE, RENAME_NOREPLACE | none | - | cut (cross-directory-rename, rename-flags) | rename.sh, pjdfstest_rename.sh, stress.sh; metadata RenameAcrossParents | dirty-set | G7 |
| LINK (hard link) into D | lifetime: Link(i, n) | none for link itself | cut (link) in D's trace; LifetimeTrace T_LifeLink | create.sh, RenameStaleSourceTest.OldObjectWithAnotherLinkIsNotLinked; metadata HardLinksShareOneRow | dirty-set | G7 |
| Attribute change of D (SETATTR, SETXATTR, REMOVEXATTR, SETFLAGS/FSSETXATTR of D) | dcfs: attrchange (A1From, AttrChangeSyscall, AttrChangePhase3, Stat, Fill) | GuardsBalanced (kb attr_change_end_skipped), TriState | T_ArriveAttrChange ... T_AttrChangeFill; trace_tests attr_change(_end_skipped).log | CommonRequestsMatchTheModel (chmod, xattr of a directory); setattr.sh | dirty-set | a failed one is cut (the model's never fails) |
| Attribute change of a file (SETATTR, xattrs, flag ioctls) | dcfs: F's fset (FSetFrom, FileSetSyscall/End/Stat/Fill); reval: ChmodF, SetFlagsF | FileExact (kb atime_*), none for fset's End (G17) | reval lines chmod/setflags (fd semantics only); none for the cache records | SetattrEndsItsMutationBeforeItsRefresh, RemovexattrEndsItsMutationBeforeItsRefreshes, SetxattrRecordsWhatTheBackingFilesystemStored; setattr.sh, write.sh | dirty-set, tri-state (attribute columns) | G1, G14, G17 |
| Fallback WRITE, FALLOCATE, COPY_FILE_RANGE | dcfs: `write` (data only, for the litmus tests); reval: WriteF | WritesUseAWritableFd (none) | reval `write` line; no `MutationSyscall` events at these call sites | CopyFileRangeEndsItsMutationBeforeItsRefreshes, FallocateEndsItsMutationBeforeItsRefreshes, TraceScenarioCopyFileRange; copy.sh, write.sh | writable-open | G1 |
| Writable OPEN/CREATE (BeginWriting), FLUSH, FSYNC, last writable RELEASE (EndWrites, record), ReconcileWritten at the last FORGET and at DESTROY | lifetime: written_ and the held fd; reval: fds; dcfs: none (F is read-only) | lifetime WrittenUntilLastForget (kb nonfinal_forget_drops_held), HeldOnlyWhileWritten (none) | WritesEnded not modelled; ReconcileWritten's MutationBegun "a file's"; FileOpened/FileReleased to reval | ReleaseDuringASyncPointKeepsTheDirtyRow, ReleaseEndsTheWritesForAFillThatBeganBefore, WritableCreateIsDirtyWhenReplied, LastForget*, ForgetBatchReconcilesInOnePhase1, DestroyReconcilesWrittenFiles; crash.sh, destroy.sh | writable-open, held-fds | G1 |
| Read-only open, held fills, atime-only marks (23.8) | dcfs: F (fopen, FRead, frelease, fgetattr, SyncKeepsHeld, AtimeExpiry) | CrashSafe (kb atime_held_fill_no_touch, atime_fill_no_touch), FileExact (kb atime_sync_clears_held, atime_open_not_dirty), FileExactStrict (lim) | none (held fills reuse the refresh events, which are traced for directories only) | AtimeTest.*, ClockTest.Atime*, CrashWhileAFileIsReadForgetsItsAtime; atime.sh, fault_power.sh (atime scenario) | open-file, dirty-set (atime flag) | no trace; no mark event (23.10 review Q6) |
| TMPFILE | lifetime: Tmpfile; dcfs: no request of D's | UnnamedRowsSwept (kb lifetime_tmpfile_row_survives_crash) | LifetimeTrace T_LifeTmpfile | TmpfileNeverLinkedLeavesNoRow, TmpfileRowGoesAtTheStartAfterACrash, TmpfileUndo*; copy.sh | writable-open | G15 |

### Fills, guards, completeness

| Element | Model | Property | Trace | Tests | Runtime | Gap |
|---|---|---|---|---|---|---|
| ResolveName (probe, commit under CanFill) | dcfs: ResolveProbe, ResolveCommit | TriState (kb tristate_f1 via BugUnguardedFills) | probe (`what` against the object map), resolve_commit (`recorded` = CanFill) | FillDuringAMutationDoesNotCache, FillAfterACompletedMutationDoesNotOverwrite (metadata) | - | vacuous under the lock (G4); "records less" (G9) |
| PopulateDirectory (snapshot, epoch, reads, commit) | dcfs: PopulateRead, PopulateCommit; UnrecordedAnswer | TriState (kb tristate_f1), ReplyObservable (kb reply_unknown_as_negative, nolock) | populate_read (listing and keys), populate_commit (`recorded`); held lines; cut overlapping-listings | TraceScenarioMkdirDuringListing, InterruptedPopulationRecordsNothing, ReaddirGivesUpWhile... | - | epoch vacuous (G13); vacuous under the lock (G4) |
| Attribute fills of D (getattr, readdirplus's ".", mutation refreshes) | dcfs: GetattrStat/Fill, ReaddirplusStat/Fill, *Stat/*Fill | TriState (kb tristate_f1), ServedFromCacheIsCurrent (kb readdirplus_unlocked) | attr_check, rdp_attr_check, stat, fill (`recorded`) | UnknownAttributesAreRefreshed, Readdirplus*IsNotServedWhileAMutationIsInFlight | tri-state (columns, nlink) | values never compared (section 2) |
| Child, parent and root row fills (RecordChild, ParentOf, InitRoot) | Trace.tla: GetattrWhole (not a model action) | - | child_fill (`filled`; the recorder rejects a fill against its own record of the guard) | EstaleParentTest.*, ParentOf (metadata) | tri-state | G5: a child's own row is not modelled at all |
| Xattr and symlink fills | none | - | - | Xattr* (metadata), Symlinks; write.sh, names.sh | - | G14 |
| Owns at phase 3; phase-1 verification of resolves | dcfs: Owns, U1/R1 verify | CacheNeverWrong (kb rename_stale_source), TriState (kb tristate_f4), both nolock | end (`owned` = Owns), phase1 aborted (retry, EAGAIN) | RenameStaleSourceTest.*, *GivesUpWhile..., OverlappingMutationsDoNotOwnTheirPhase3, Begin{Rename,Remove}RefusesAStaleResolution | - | vacuous under the lock (G4); other inodes' verification (README finding) |
| Completeness (IsDirComplete, ListCached's single check, CompleteNeverHides) | dcfs: complete, DirListable, RDFrom | CompleteNeverHides (none), ServedFromCacheIsCurrent (kb readdirplus_unlocked) | list_check (`complete` = DirListable); db's complete and epoch every line | ReaddirListsWhatWasCompleteWhenChecked, IsDirComplete (metadata) | - | G8 (CompleteNeverHides), G13 |

### Durability, sync points, crashes, recovery

| Element | Model | Property | Trace | Tests | Runtime | Gap |
|---|---|---|---|---|---|---|
| The dirty set and its reasons; `Context::dirty.durable` and phase 1's fast path | dcfs: dirty (D), fDirty no/atime/mut (F), durableD, fm.durable, BeginMutation | CrashSafe (kb crash_f1, sync_during_mutation), DurableSetSound (none), CleanMeansNoDirty (none) | phase1 `synced` (SyncedOK), db `dirty` and `durable` every line | BeginMutationIsDurableUntilIdsAreKnownDirty, EveryMutationKindDirtiesWhatItChanges, MarkDirtyIsNotDurableAndClearDirtyKeeps; DirCacheFSDeathTest.*Dirty* | dirty-set (four rules) | children's dirty rows (G5); G8 |
| Sync points: BeginSync, syncfs, ClearDirty, keeps (in flight, open for writing, open) | dcfs: S1From (SyncBarrier = Syncfs), S2 (with SyncKeepsHeld, coveredF), SyncDriven, AtimeExpiry, StopSync/StopClear | CrashSafe (kb sync_during_mutation, sync_by_file_fsync, atime_held_fill_no_touch), FileExact (kb atime_sync_clears_held) | sync_begin, syncfs (stutter), sync_clear; recorder: snapshot right before SyncfsStarting (fault snapshot_after_syncfs) | MkdirDuringASyncPointKeepsItsDirtyRows, ClockTest.*, ClearDirty* (6, metadata), SyncPoint*ReadOnly* | dirty-set (a deleted row was not in flight, open for writing or durable) | the fast path (G15); the open-for-write keep (G1); StillWritable (EROFS) outside the model by design |
| Power loss versus daemon crash | dcfs: Crash = PowerLoss or DaemonCrash (12.6b) | CrashSafe (kb recovery_clears_dirty: daemon crash then power loss) | crash (either), restart | Restart in the harness (daemon crash; "power loss" = hand-edited backing, no database rollback); crash.sh (SIGKILL), power.sh (fabricated), fault_power.sh and fault_ace.sh (real, untraced) | full check at RunStarted | G2, G12 |
| Recovery: StartRun, RecoverDirty, ForgetUnnamedRows, ProbeRecoveredRows, crash during recovery | dcfs: Restart, Recover (RecoverForgetting), StartRun, ProbesDone, CrashRecovering; lifetime: Restart, ProbeRow, SyncPoint | RecoveryIdempotent (kb recover_clears_dirty_first), CrashSafe (kb recovery_clears_dirty); lifetime RowsNameLiveObjects (kb crash_before_settle, probe_list_in_memory), RecoveryIdempotent (none) | recover (`dirty_keys`), start_run, recovery_done; trace_crash_during_recovery_test; trace_tests recover_* | CrashDuringRecoveryRecoversAgain (+ faults recover_dirty_fails_once, probe_fails_once), StartupAfterACrashForgetsUnnamedRows, Recovery* | full check at RunStarted | G24 |
| Clean shutdown (FinishRun) | dcfs: BeginShutdown ... StopFlag; lifetime: Destroy (DestroyWithOpens) | CleanMeansNoDirty (none); lifetime UnnamedRowsSwept (kb destroy_with_open_files) | shutdown, stop_sync, stop_clear, checkpoint, clean | DestroyWithAFileStillOpenForWriting, CleanStartSweepsTheRowOfAFileOpenAtDestroy; lifecycle.sh, destroy.sh | full check at DESTROY | G19 |
| Backing crash regimes (12.8) | dcfs: bSeq, BCrash, MetaCrash, Force, FsyncOnly | CrashSafe and CrashRefines under ext4 and metaprefix (MC_crash_*), litmus (MC_litmus_*), kb sync_by_file_fsync, lim litmus_* | `seq` only (valid under weaker regimes) | fault_ace.sh, fault_power.sh (in order only) | - | G2 (reorderings untestable with drop-writes) |
| SQLite's durability levels | dcfs: Commit(new, sync), dbOpts | presupposed by every crash property | phase1 `synced` | fault_cache.sh (write errors), fault_power.sh | - | G2 |

### Cancellation, identity, lifetime, boundaries, out-of-band, replies, streams

| Element | Model | Property | Trace | Tests | Runtime | Gap |
|---|---|---|---|---|---|---|
| Cancellation checkpoints | dcfs: Interrupt at RN_probe, PD_read, PD_commit, C_sys, U_sys, R_sys, A_sys | CacheNeverWrong (kb interrupt_after_syscall), TriState (kb interrupt_undo), GuardsBalanced (kb interrupt_leaks_guard), ReplyObservable (EINTR only without effect) | interrupt (T_Interrupt) | Interrupted*, MutationInterrupted*, ForgedFuseInterruptStopsAPopulation, CheckpointTest; cancel.sh | - | G21 |
| Identity and generations | ident: Lookup, Refuse, Create, Unlink, Rename, Access, DotLookup, NfsTake, Evict, crashes, power loss, wipe, out-of-band, Phase 14 target | OneHandleOneObject, HeldResolvesToItsObject, NoBadInode (kb x6); ServedWhileLive, ReuseDetected (lim); HandlesResolveToTheirObject, GoneIsStale (none) | IdentTrace (harness only): reply, forget, resolve, gone, run | IdentityCheckRefusesAnotherObjectBehindTheHandle, OutOfBandReplacementGetsEstaleFromItsHandle, Estale*; handles.sh, nfs.sh | identity (generation range) | ESTALE on an unreadable inode not expressible (README); G16; G20 |
| FORGET and lifetime | lifetime: Lookup, Create, Tmpfile, Link, Open, Release, Remove/Settle, Forget, ForgetMulti, Destroy, Crash | NodeidStable, ReferencedServed, WrittenUntilLastForget, LookupsExact, UnnamedRowsSwept, RowsNameLiveObjects (kb x8); NotRetiredWhileReferenced, HeldOnlyWhileWritten, ForgetKnown, NothingLeaks (none) | LifetimeTrace (harness only) | NonFinalForgetKeepsTheHeldDescriptor, RemovedFileIsServedUntilItsLastForget, ForgetMultiTakesOffEachEntrysCount, DestroyLetsGoOfEveryNodeid; removed.sh, release_leak.sh | lookup-count, removed-record, held-fds | G8, G20 |
| Boundaries and stubs | lifetime (stub ids, StubGone), ident (stubs config); dcfs: none | NodeidStable (kb stub_nodeid_reused), OneHandleOneObject (kb stub_generation_reused) | cut (boundary) | BoundaryIsAStubDirectory, EveryOperationOnAStubIsRefused, ForgottenStubsKeepTheirNodeids; boundary.sh | tri-state (stub and refused dentry) | - |
| Out-of-band detection (ReconcileAttrs) | reval, ident, lifetime: `OutOfBand` flags; dcfs: none | lim out_of_band_*, ident_target_* | cut (out-of-band) | OutOfBandReplacementGetsEstaleFromItsHandle; crash.sh's second half | - | G13 |
| Replies | dcfs: Reply, rep/rb ghost | ReplyObservable (kb reply_after_failed_syscall, reply_unknown_as_negative) | reply (`errno` class, a lookup's `ans`); trace_tests reply_*; 12.7c sentinel | most tests check their replies | - | G23 |
| Directory streams, readdir cookies | none (12.9 planned) | - | a continuation is a readdir | ListDir* (metadata), ReaddirWorkTest.*; readdir_boundary.sh | - | G18 |

## 2. What trace validation checks, per event, and what it cannot see

After **every** line, `DbMatches` compares the model's cached state of the
traced directory with the line's `db`: each name's state (no row, unknown,
absent, or an object's key, which `okey` keeps consistent within a run),
`complete`, `epoch`, `valid` (D's attributes), `dirty` (D's row), `clean`,
`durable` (D in `Context::dirty.durable`) and `inflight`. On every traced
behavior TLC also checks `GuardsBalanced`, `EffectAtSyscall`,
`BackingAtSyscall`, `CacheLearnsAtCommit` and `ReplyObservable` (Trace.cfg).
Per line, besides that:

| Line | Model step | Also compared |
|---|---|---|
| begin | TraceInit | `origin` (mkdir, listing, parent, existing) fixes the start's shape; names observed before any syscall start as observed |
| lookup | Arrive (lookup, unlink, rename) or LookupStep | `out` (found, neg, resolve, populate) against the cache read; a found key against the object map |
| probe | ResolveProbe, CreateProbe | `what` (key or absent) against the model's read |
| resolve_commit | ResolveCommit | `recorded` = CanFill(snapshot) |
| populate_read | PopulateRead, placed at PopulateStarted (lines of what ran during the reads are held) | the listed names and their keys |
| populate_commit | PopulateCommit | `recorded` = CanFill and epoch unchanged |
| child_fill | GetattrWhole or a stutter | `filled`; a fill against the guard is turned into `unexplained` by the recorder from its own record |
| list_check | Arrive (readdir) or ReaddirStep | `complete` = DirListable |
| rdp_attr_check, attr_check | stutter; Arrive (getattr) | `valid` |
| stat | the *Stat actions | nothing (values are not compared) |
| fill | the *Fill actions | `recorded` = CanFill(snapshot); a silent refresh of valid attributes is no step, and the recorder checks its decision |
| phase1 | Arrive (create, attrchange), UnlinkPhase1, RenamePhase1 | `outcome` begun or aborted; `synced` must be true wherever the model fsyncs (SyncedOK) |
| resolved | UnlinkPhase1's ENOENT branch, RenameResolveDst | `found` |
| syscall | Create/Unlink/Rename/AttrChangeSyscall | errno 0 against the success branch; the recorder requires phase 1 before `MutationSyscallStarting` |
| end | the Phase3 actions, the Failed actions | `owned` = Owns (not asked for an attribute change) |
| sync_begin, syncfs, sync_clear (stop_*) | S1From, stutter, S2 (StopSync, StopClear) | the recorder requires the snapshot right before `SyncfsStarting` |
| interrupt | Interrupt | - |
| reply | stutter (the model's request must have replied) | errno class; a lookup's answer, its key through the object map |
| crash, restart, recover, start_run, recovery_done | Crash, Restart, Recover, StartRun, ProbesDone | `recover`'s `dirty_keys` bound which present dentries may become unknown; `restart` resets the object map |
| shutdown, checkpoint, clean | BeginShutdown, StopCkpt, StopFlag | - |
| cut, gone, unexplained | end of trace, or rejected | each test's `allow_cuts`; the root must reach the run's final event or a `root_cuts` category |

What it cannot see:

- **Per-directory scope.** Each directory's trace is checked alone. Its
  `db` has its own dentries and its own row's validity and dirty flag, not
  its children's rows: a missing dirty mark on a created child (RecordNewChild's
  MarkDirty), a child's attributes recorded at the wrong moment, or a stale
  child row (G5) are invisible. The recorder's "no other directory's state
  changed" check covers only directories' states.
- **File-level changes.** Files have no `dcfs.tla` trace: writable opens,
  file setattrs, writes, fallocate, copy_file_range, held fills, atime-only
  marks, EndWrites, ReconcileWritten (G1). reval's file traces check the
  shared fd and permission decisions; lifetime's and ident's traces check
  nodeid bookkeeping and reopen outcomes; none checks a file's cached
  attributes or dirty row.
- **Cross-directory interleavings.** A link into, a rename across, a rename
  with flags, a mutation of the directory itself: cut, and nothing after the
  cut is checked (G7). Two directories' traces of one rename are never
  matched against each other.
- **Concurrency.** Guest runs are one thread: no request ever interleaves
  with another, so guest traces validate sequential behaviors and crashes
  only (`KernelDirLock` TRUE is the model's permission, not something the
  code exercises). The harness interleaves at four wrapped calls
  (`open_by_handle_at`, `name_to_handle_at`, `syncfs`, `statx`), so only the
  interleavings its hooks place are validated, against a model that allows
  one at every syscall. `populate_read`'s reordering can only reject.
- **Reply contents.** The errno class and a lookup's answer only: not an
  entry's nodeid, generation or attributes, not the entries a readdir or
  readdirplus sends (the model's listing comes from the rows at
  `list_check`), not getattr values, not xattr or readlink replies (G23).
- **Values.** Attribute values, the guards' clock, xattrs, symlink targets,
  the database's other tables (`stubs`, `xattrs`, `symlinks`, `inodes` but
  for D's own validity).
- **Crash states actually produced.** Guest traces have SIGKILLs and a
  fabricated backing; no traced run has a real database rollback (G2, G12).
- **Invariants beyond the action properties and GuardsBalanced** (G11).

## 3. Properties that cannot bite

No `known_bugs/` or `limitations/` test expects these to fail (checked
against every `expect_violation` in `formal/BUILD.bazel`). For each: whether
a cheap variant would show it bites.

| Property | Module | Cheap variant | Expect |
|---|---|---|---|
| CompleteNeverHides | dcfs | `known_bugs/tristate_f4_restore_complete.cfg` with this invariant alone | violated: the unlink's phase 3 restores `complete` over the create's NoRow name, then the create's syscall makes it exist (needs a run) |
| CrashRefines | dcfs | none of the existing crash variants: it accepts serving any state the regime admits, and every one-name counterexample serves such a state (sync_by_file_fsync's recovered cache is the backing's pre- or post-create state) | needs a two-name variant where the recovered cache mixes names from two backing states; whether one exists in bounds needs a run (crash_f1 with Names {a, b}, two creates, `CrashRefines` alone). If none does, say in the README that it is redundant by construction |
| DurableSetSound | dcfs | S2 that keeps `durableD` (the code's `ctx.dirty.durable.clear()` in ClearDirty dropped) | violated at the clear's commit; the checker's "durable implies a dirty row" catches the code mutant at runtime |
| CleanMeansNoDirty | dcfs | StopClear skipped | violated, but the flag changes nothing in dcfs.tla (G19) |
| BackingAtSyscall | dcfs | none | holds by construction (only syscall actions write `bCur`); accepted, documented |
| CacheLearnsAtCommit | dcfs | `known_bugs/interrupt_after_syscall.cfg` (or interrupt_undo) with this property alone: the undo's commit at an Interrupt step makes the source name known | violated (needs a run) |
| RecoveryTerminates | dcfs | ProbesDone waits for an empty dirty set (only a sync point while serving empties it) | violated; TLC runs with deadlock checking on (`tlc_test` passes no `-deadlock`), so expect "Deadlock reached" unless the variant stutters |
| OpenExact, OpenModeExact, HeldFlagsLegit, HeldModeLegit, WritesUseAWritableFd | reval | the existing variants with one invariant each (reval_no_recheck, reval_no_write_fd, ...): today the first-listed invariant shadows the rest | violated by the existing bugs (needs a run) |
| CachedModeCurrent, KernelModeCurrent | reval | ChmodF that leaves `cF` (or `kAttr`) as before | violated; it is Setattr's refresh after a chmod |
| NotRetiredWhileReferenced | lifetime | Settle that deletes the row of a held object without a removed record | violated (ReferencedServed too; a config with this invariant alone) |
| HeldOnlyWhileWritten | lifetime | Release that takes a held descriptor for a read-only open | violated |
| ForgetKnown | lifetime | a lookup path that does not count (Readdirplus's `++lookups_` loop) | violated at the FORGET; the checker's lookup-count rule catches the code mutant |
| NothingLeaks | lifetime | the last FORGET keeps the removed record | violated |
| KernelForgotAfterCrash | lifetime | none | by construction; accepted |
| RecoveryIdempotent | lifetime | `known_bugs/lifetime_probe_list_in_memory.cfg` with this invariant alone | violated at the first unclean start, as the README says (needs a run) |
| HandlesResolveToTheirObject | ident | `known_bugs/ident_skip_identity_statx.cfg` with this invariant alone | probably violated (a handle the kernel accepts after the recycling) (needs a run) |
| GoneIsStale | ident | Unlink's phase 3 keeps the row (no RetireRemoved) | violated: a handle of the freed object is answered from the row |

Also with nothing to fail them as configured: `FileOK` (only inside
`Correct`), `LitmusOK`/`LitmusBackingOK` bite through the limitations. The
trace specs RevalTrace, LifetimeTrace and IdentTrace check no invariant at
all (only that the trace is a behavior).

Properties that bite only from the large tier, or through a known_bug
that passes whatever else breaks: everything that depends on `Owns` and
the U1/R1 verification (G4).

## 4. Code paths with neither a model nor a harness test

Ranked; "harness test" includes `metadata_cache_test`.

1. **Crash safety: a fill that records a created object's row before the
   create's phase 3** (G5). `backing.cc` RecordChild (`child_ok`,
   `filled`) for a row UpsertInode just made, while `BeginCreate`'s ids are
   only the parent. No test runs a listing or resolve of the parent inside
   RecordNewChild's probe (the hooks exist: NameToHandleHook).
2. **Crash safety: the database rollback itself** (G2): no harness test can
   roll the WAL back; RecoverDirty against a prefix database is exercised
   only by real guest cuts (in order) and by the model.
3. **Crash safety (latent): ClearDirty's fast path with a row MarkDirty
   added without moving the clock** (G15): `metadata_cache.cc` ClearDirty
   `nothing_moved`, RecordTmpfile's MarkDirty. ClearDirtyWhenNothingMovedIsExact
   tests the fast path, not this premise.
4. **Consistency: a cross-directory rename's phase 3 per parent** (own
   `Owns` for each, `exchange`, `same_inode`) and RENAME_EXCHANGE's two
   LinkDentry calls: guest-tested (rename.sh, pjdfstest_rename.sh), no
   harness test with an overlapping mutation of one parent only, no model.
5. **Consistency: ReconcileAttrs's relist** (ForgetNegativeDentries, epoch
   bump, no guard touch) racing a population of the same directory: the
   epoch check is its only defence, and no test or model has the race
   (out-of-band, so low).
6. **Consistency: xattr side effects** (XattrsChangedBySetattr,
   ResolveSideEffectXattrs after writes, the ACL and mode coupling): tested
   for setxattr's read-back and removexattr's End; the side-effect paths
   only by guest scripts' digests (stress.sh).
7. **Diagnostics: EntryAfterPhase2's fallback reply** (a refresh that
   failed replies the row's pre-mutation attributes, timeout 0): CreateWhoseRefreshFailsIsRepliedFromTheRow
   covers create; Link's use is untested.

Everything else in `DirCacheFS`, `backing.cc` and `metadata_cache.cc` that
carries protocol state has a harness test; the model gaps are listed in
section 0.

## 5. Reachability: what would show the model's states are the code's

Today the code is one thread: in production no two requests interleave, so
the model's concurrency is the coroutine future and the harness's forced
interleavings; what production reaches is sequential behavior plus crashes
at any point. That orders 12.10's targets:

1. **Crash-point replay** (today's risk). For each step of each request in a
   small configuration (1-2 names, 1 slot), a power loss or a daemon crash
   there, with each choice of `dbOpts` (a WAL prefix since the last fsync)
   and of `BCrash` (the backing state). The generated test builds the
   backing state the model chose directly (as power.sh does by hand: it
   knows which names exist) and restores the database copy taken at the
   chosen commit (copy the WAL in the `SqliteTransaction` hook), then
   restarts and checks the served state and the trace. This tests the
   database half of `CrashSafe` exactly, which nothing does today (G2).
2. **Known-bug counterexample replay.** For each `known_bugs/` variant,
   replay its counterexample's prefix against the real code: the trace must
   be valid against the real model up to the bug's divergence point. That
   shows each scenario the model's tests rely on is one the code can be
   driven into (the reachability direction for the oracle itself).
3. **Concurrency-only branches** (the future): `Owns` false at each
   phase 3; `CanFill` false at each fill kind (resolve, population,
   getattr, readdirplus, refreshes, child fills); S2 keeping D dirty
   because a mutation began or ended since its snapshot; U1/R1's retry and
   EAGAIN; RD's EAGAIN; PopulateCommit's unrecorded answer; Interrupt at
   each pc, PD_commit and A_sys included; a getattr whose window spans
   another request's syscall (`win`). Interleaving points: the 26.6 fault
   sweep already enumerates backing call sites (`BackingCall(site)`); a
   generated test says "run request Q at the k-th backing call of request
   P", generalising the four hooks.
4. **Dead actions.** `UnlinkFailed`, `RenameFailed`, `RenameFailed2`
   report 0 in every configuration: nothing can reach them in the model,
   so nothing can be generated for them. G6's "syscall fails without
   changing anything" makes them live; until then, say they are
   unreachable rather than covered.

Generation: TLC's simulation mode with a fixed seed and a coverage target
(one behavior per action and branch not yet taken by a handwritten trace)
on a small configuration, serialized with CommunityModules' JSON (as
MongoDB's trace generation does), rather than the whole state graph of
MC_small (1.48M states). Each generated test's own trace is validated, which
closes the loop: a generated test whose trace is rejected means the hooks
did not force the model's path or the code diverges.

## 6. The unmerged step-23.10 model: what to merge

On its own, worth merging (as structure, before or with 23.11 and 12.11b):

- **F's names and hard links**: `bCur.fl` (the directories naming F, its
  link count), `dbCur.fDent` (F's cached dentry in D and in E), `FLive` (F
  exists while named or held), `FInitLinks`/`FInitDent`/`FInitOpen` (an F
  that starts as an open O_TMPFILE file), the flookup/flink/funlink
  requests. 23.11 needs F's name (the parent's mark covers it) and an
  unnamed F (O_TMPFILE); a hard link in E exercises RecoverDirty's "every
  dentry pointing at a dirty inode becomes unknown", which `dcfs.tla` today
  only approximates with `RecoverForgetting`'s arbitrary subset.
- **The ghost `fs`** (the stamp of F's last non-read change) and the
  two-sided `FileOK` (`b.fs <= fAttr <= b.f`): today's `FileOK` cannot see a
  cached F *behind* on size or mtime; with writes (12.11b) it must.
- **The ext4 regime's item ordering** (F's attributes in order with its
  names, rules (1) and (3); a link or unlink kept for both or neither) and
  the per-item `PosOpts`/`Choices` enumeration, which computes the same set
  as before without enumerating every function.
- **The three named conditions as action properties**, re-expressed per
  inode (D and F, each with its own row): `DirtyBeforeChange` (every
  backing change covered by a record comes when every crash state has that
  record's inode dirty for a mutation), `ClearOnlyAfterSync` (a mark goes
  only when nothing it covers is unsynced and nothing of it is in flight or
  open), `RecoveryForgetsDirty`; with the premise tests (crash F1,
  sync_during_mutation, a recovery that forgets less). They localise a
  `CrashSafe` failure to the premise that broke, and each maps to a checker
  rule for 12.15 ((i) is "in flight or durable implies a dirty row", (ii)
  "a deleted dirty row was not in flight, open for writing or durable").
  Cost: 23.10 measured MC_small at 12 min with (iii) against 7 without; keep
  (iii) in the smaller configurations.
- **`GuardsBalanced` over each guard** (E's there; add F's, G17), and
  `dirset_sync_clears_with_mutation` re-expressed as "S2 clears F's row with
  F's mutation in flight" (F's sync_during_mutation).
- **The three "known parents" counterexamples** (handle-only access, a hard
  link in another directory, an unnamed O_TMPFILE file) as known_bugs of
  the status quo once F has names: "a mutation of a file marks the
  directories the cache has its dentries in instead of the file's own row"
  violates `CrashSafe` in those three scenarios. They guard against any
  future fast path that drops a file's own dirty mark. Until F has names,
  record them in the README's findings.
- `ViewExactF`, only if 23.8b concludes `FSnapView` is inexact.

Specific to the rejected home design (do not merge): `FHome`, `FMarks`,
`FCovering`, `MarkDirs`, directory atime-only marks (`dAtime`, `eAtime`),
the removal of `fDirty` (file rows never dirty), `RecoverNames` and
`RecoveredFile`'s three disjuncts, recovery as a function of the backing
state (`RecoverDirty(d, t)`, and Trace.tla's `T_Recover` with it),
`CrashImage`'s home-based forgetting, the nine `MC_dirset_*`
configurations, `dirset_child_not_relisted`, `FInitMarks`, and the
design.md/README text about homes and schema v7.

## 7. Order and shape of the approved steps

1. **In parallel now**: 12.13's tool (dcfs-implementer; touches
   `tools/mutation/` only), **12.12a oracle hygiene** (new, small,
   dcfs-protocol: section 3's premise configurations; Trace.cfg checks
   `TriState`, `CacheNeverWrong`, `DurableSetSound`, `CleanMeansNoDirty`
   and, if cheap enough per trace, `CrashSafe`; `MC_nolock_small` in the
   medium tier and the action properties in MC_nolock (G4); `GuardsBalanced`
   with `fm.inflight` and its known_bug (G17); a coverage gate that runs each
   configuration's TLC with `-coverage` and fails if an action of `Next`
   other than the listed dead ones reports 0 in every configuration, which
   turns README's "keep every action reachable" into a test and kills every
   "drop an action from Next" mutant), 12.14 (dcfs-protocol, a guest test
   in another lane; dm-log-writes replay, G2) and 12.17 (its own module, no
   overlap). Reason: 12.13 measures the oracle; the hygiene step removes the
   survivors this audit can already predict, so the sweep reports what it
   alone can find.
2. **12.13's first sweep** after 23.8b settles the VIEW question (G3):
   until then a mutant "survives" possibly because the view skips the
   state that kills it. Shape:
   - Scope: dcfs.tla, lifetime.tla, reval.tla, ident.tla actions and the
     definitions they use, **and Trace*.tla** (G10); never the properties
     themselves (weakening a property always passes).
   - Kill set per mutant: the module's medium-tier configurations, its
     known_bugs, `MC_nolock_small`, and the trace log tests; not the large
     tier (MC_large is ~2000 s on the runner).
   - Operators that will find real under-specification here: drop a
     conjunct of a guard (`dbCur.epoch = r.esnap` in PDCommit, G13; `Owns`'s
     `inflight = 1` and U1/R1's verification, G4, survive without
     `MC_nolock_small`); swap two steps (A3's refresh snapshot before
     `AttrChangeEnd`: the refresh is refused, unknown is safe, G9); replace
     a durability level (StartRun's and StopFlag's kSync to normal: the
     clean flag is decorative, G19; a fill's normal to kSync: equivalent,
     more durable never fails safety, so an `equivalent.txt` rule for
     "normal to kSync"); drop a step from a sequence (a fill's `Commit`,
     RecoverD's epoch bump: survivors, G9 and G13); drop an action from
     `Next` (survives without the coverage gate: `CrashRecovering` gone
     leaves `RecoveryIdempotent` passing, since it is a state invariant over
     `dbOpts`); negate or drop `FillOK`'s, `OwnedOK`'s, `SyncedOK`'s
     comparisons in Trace.tla (survive until G10's negative logs exist).
   - Survivors of the "knows less" class get progress properties (G9), not
     equivalence entries.
3. **23.11 model half** (dcfs-protocol, lane-1 after 23.8b). What the model
   needs: F's row existence in `DBStates` (no row, or a row) with the
   insert as a normal commit a crash may drop; F's create as a request in D
   (phase 1 on D, the syscall, phase 3 inserting F's row, F's dentry if
   `Owns(D)` and F's mark in one normal commit, after which F counts as
   durably dirty); `DurableSetSound` restated as "every crash state has F's
   mark or no F row"; the plan's invariant "no crash state holds the row
   without its mark until a sync point covered the create"; ProbeRecoveredRows
   for F (a born-dirty row of an object the backing lost is deleted at the
   start); `CrashSafe`, `RecoveryIdempotent`, `CrashRefines`,
   `ReplyObservable` under seq, metaprefix and ext4; a nolock
   configuration. Known_bugs: the plan's two (row and mark in two commits;
   the parent's phase 1 not durable), plus **a fill that records F's row
   before phase 3** (G5, nolock: the born-dirty claim fails unless that
   fill leaves F's attributes unknown or marks the row), **a sync point
   between phase 3 and BeginWriting that clears the mark while F stays
   "durably dirty"** (ClearDirty's `durable.clear()` dropped), and
   RecordTmpfile's mark against ClearDirty's fast path (G15). Take F's
   names and `FLive` from 23.10 (section 6) rather than inventing them.
   Then the code, as the plan says.
4. **12.11b** (dcfs-protocol), after 23.11 because both rewrite F and
   BeginWriting's durability changes with born-dirty. First the model
   (G1's prerequisite, with the code's mechanism: `open_for_write` as its
   own state, BeginWriting a mutation that ends at once, the keep rule, the
   record after EndWrites, ReconcileWritten as a phase 1 with no syscall and
   a refresh through the held descriptor; the ghost `fs` from 23.10; four
   known_bugs). Then the events: `MutationSyscallStarting`/`MutationSyscall`
   at Write, Fallocate and CopyFileRange; a mark event for MarkAtimeDirty
   (23.10's review, question 6); a file's `db` (valid, dirty reason,
   durable, in flight, open for writing). Then a FileTrace spec mapping a
   file's lines onto F's actions. 23.10's "fset spans the whole open" will
   not match a trace: the code's `inflight` is 0 during the open.
5. **12.11c**, in two halves. First (cheap, removes G6 and G7): project
   cross-directory steps onto the one-directory model per directory: a
   link into D is a linkcreate (the object map is already non-injective); a
   rename out of D is an unlink (resolve, phase 1, syscall, phase 3, D's refresh); a
   rename into D is a new request kind `renamein` (a create whose object is
   known and which may replace the name's object); RENAME_NOREPLACE is a
   `renamein` that fails EEXIST; RENAME_EXCHANGE replaces in both; U1/R1
   may abort nondeterministically ("another inode changed since the
   resolve", README's finding); and syscalls may fail with an errno class
   `other` changing nothing. Then trace a short seeded fsstress and
   fault_power (G12). Second (only if the first or 12.13 shows the need,
   after 12.15): one joint trace of two directories, which needs a model
   with E's own names; 23.10's E (which holds only F's name) is the start.
   Interleavings worth it: the four-inode verification, `Owns` per parent,
   a moved directory's `..` (ParentOf), the replaced object's row.
6. **12.15** (runtime checks from the model), after 12.11b so file rules
   pair with F's model. Pair each checker rule with its invariant and a
   known-bad fixture; add the rule the model has and the checker lacks:
   at every backing call, every inode in `fills.inflight` has its
   attributes unknown (`TriState` for attributes; it catches crash F3's
   class in every guest run of the checking build; by reading it holds today: every
   `Begin*` marks each id it registers unknown, and no backing call comes
   between a phase-3 record and its End); and an xattr rule (G14).
7. **12.10** (generated tests), after 12.11b/c: crash-point replay first
   (section 5, item 1, with the WAL-copy facility), then known-bug
   counterexample replay, then the concurrency branches. Not checked in, as
   approved; the generator tested on a fixed small behavior set.
8. **12.9** directory streams, then **12.11c's second half**, then
   **12.16** (the refinement target) last: an ideal-filesystem refinement
   against a one-directory, read-only-file model would state little;
   after F's writes, names and a second directory it states most of what
   "behaves like a plain filesystem" means.

12.14 and 12.17 stay off the critical path (their own modules); 12.14 is
crash-safety foundation and should land before 12.10's crash-point replay
relies on the same abstraction.

## Runs that would settle what this audit could not

All through Bazel in a lane (`sg kvm -c '...'`); each needs a small
`tlc_test` target or config first.

- Section 3's "needs a run" rows: one `tlc_test` per premise configuration
  (`config = "known_bugs/<variant>_<property>.cfg"`, `expect_violation =
  "Invariant <P> is violated"` or `"Action property <P> is violated"`),
  then `bazel test //formal:<target>`.
- G4: `MC_nolock_small.cfg` as a medium `tlc_test`; and MC_nolock with the
  three action properties (`bazel test //formal:nolock_test`, eternal).
- G13 and the 12.13 predictions: the PDCommit epoch conjunct removed, run
  `//formal:small_test` and `//formal:nolock_test`: expect both to pass.
- G5: a harness test (NameToHandleHook runs `Readdir(kRootInode)` inside
  `Mkdir(kRootInode, "new")`'s probe; expect the new row valid and not dirty
  between the two commits) and `bazel test //dcfs:dir_cache_fs_test
  --test_arg=--gtest_filter=<name>`.
- Action coverage per configuration: `tlc_args = ["-coverage", "1"]` on a
  `tlc_test` (the attribute exists; no target uses it). The README's by-hand recipe outside
  Bazel was refused to the 23.10 reviewer.
- G3: 23.8b's bisection (running).
