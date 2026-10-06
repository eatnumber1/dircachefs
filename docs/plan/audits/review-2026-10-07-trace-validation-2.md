# Re-review of trace validation (12.2b), 2026-10-07, dcfs-reviewer, read-only

Original holes closed; new findings fixed in 12.2c before merge.

Re-review of step-12.2 (e526c2b..step-12.2, fixes b123292..c0e2851). I read the code only; I ran neither Bazel nor TLC.

**Verdict.** The original holes are closed: syscall before phase 1, End before the syscall, the unchecked frame-end status, sync-snapshot timing, and the cut allowlist. I found one recorder bug that silently drops an `unexplained` line. Two "failed" cuts can still hide a step the model forbids.

## Findings, ranked

**1. MEDIUM-HIGH (verified): lines written during a listing's reads are lost if the directory dies there.** `trace_recorder.cc:357-361` holds every line while D is `reading`, including the lines written by `Unexplained` (`:416-422`), `Cut` (`:425-430`) and `gone` (`:460`). Each of those also sets `dead`.
- Afterwards, `PopulateRead` does nothing, because of `if (Traced(dir))` at `:853`. The held lines are never written.
- `Close` (`:234-238`) clears `held`, and its `Cut` is a no-op because the directory is already dead.
- The trace then ends at the last line before `PopulateStarted`, with no end line. `trace_validate.sh` reports it as valid, and the cut allowlist and root check pass.
- Reachable in the harness: the `name_to_handle_at` hook runs requests during a population's probes. Example: a create whose syscall comes before its phase 1 (the `MutationSyscallStarting` unexplained line), run inside a readdir's population of the same directory, validates.
- Fix: in `Unexplained`, `Cut` and `gone`, if `reading`, clear `reading` and `held` and write the line directly.

**2. MEDIUM (verified): "failed" cuts that don't depend on the request failing.**
- `Close` (`:234-238`) cuts "failed: a listing failed half-way" whatever the frame's status is. A request that swallows a getdents error and replies OK with a partial listing passes in every guest target.
- `MutationSyscall` (`:1088-1096`) cuts "failed" when the syscall returns an error the model lacks (ENOTEMPTY, EBUSY, ...), before anyone knows how the request ends. If the code ignored that error, ran phase 3 and replied OK, D is already dead and the test passes.
- Fix: treat both like `ended_early`. Remember the condition, cut at `RequestEnd` only if the request failed, and write `unexplained` on success.

**3. MEDIUM-LOW (logic verified, scenario suspected): `dir-itself` covers any phase 1 that names D in a request not about D** (`:1002-1006`). This includes write, release and open requests, or a create in a child of D. A wrong id passed to `Begin*` cuts D (allowed in crash, rename and create) instead of being flagged, and that request's later writes to D go unchecked.
- Fix: allow it only when D's key equals the last recorded value of the request's `(ino,name)` or `(newparent,newname)`; otherwise write `unexplained`.

**4. LOW-MEDIUM: child-fill guard checks still use the code's own rule.** `Fill` (`:821`) judges the reported decision with `cache::CanFill` and the snapshot the code reported. The model's `GetattrWhole` (`Trace.tla:276-282`) only looks at `inflight` at the moment of the fill. So a bug inside `CanFill`, or a `BeginFill` moved late in `ResolveName` or `ParentOf`, is not caught.
- The silent-refresh path already uses the recorder's own counter (`mark`/`mutation_lines`). Use the same for child fills: record `mutation_lines` at the code's snapshot event (`PopulateStarted`, `LookupDecided(kResolve)`, a new event in `ParentOf`).

**5. LOW: the root check only tests that there is no end line** (`trace_validate.sh:205-215`).
- A root trace that just stops (finding 1), or has only its begin line (0 events, counted as empty), "reaches the end of the run".
- `in_list "" ""` is true, so with power's empty `root_cuts` the root may end at `gone`, or at a cut whose reason doesn't parse.
- Fix: require at least one event, and require the root's last line to be the run's final event (`clean` or `stop_clear`).

**6. LOW:**
- The fault regexes (`dcfs/BUILD.bazel` `DIR_CACHE_FS_FAULTS`) don't check the reason text. They match any unexplained line at that callback, including one written by `After`. By my reading only the intended line can occur, but adding the reason text would make the tests prove it.
- `ended_early` → `RequestEnd` (`:1147-1153`, `:543-546`) never runs in practice. `End` changes D's `inflight`, so `After` writes `unexplained` first. The phase-3 fault is rejected that way, which matches its regex.
- Side effect: the legitimate path where `OpenNode` fails after phase 1 (`dir_cache_fs.cc:422`) also fails validation. That is a spurious failure, not a wrong pass, but the comment at `:1148-1151` is wrong.

**7. NIT:** commit 16cc9b3 and `README.md:735-740` say the old and new recoveries "leave the same states reachable". The state counts (368k → 688k) contradict that. The argument that actually holds is that the new behaviours are a superset: `RecoverForgetting(d, {}) = RecoverDirty(d)`.

## Your seven questions

1. **Cuts: holds, apart from findings 2 and 3.**
   - The allowlist is enforced; the harness allows no cuts; `invalidated` and `overlapping-listings` are allowed nowhere.
   - These categories come from the FUSE operation, so the code can't fake them: `cross-directory-rename`, `rename-flags`, `link`, `dir-attrs`.
   - `boundary` and `out-of-band` are decided by the code, but the guest scripts' own assertions would notice a false one.
   - `root_cuts` are per category, not tied to a specific event. Because a cut ends the trace, though, the root's cut is always the first event of that category. In crash.sh that is at line 285 of 310.
   - I couldn't confirm without runs that each listed category is needed. Spot check: crash needs `dir-attrs` (setxattr on `$MNT/d`).
2. **Syscall-start events: holds.** `MutationSyscallStarting` is right before the syscall and after `Begin*` (`dir_cache_fs.cc:425,717,846,993`). `SyncfsStarting` comes right after `SyncSnapshotTaken`. Two of the new fault builds are rejected by the new checks: `syscall_before_phase1` (the regex requires `MutationSyscallStarting`) and `snapshot_after_syncfs`. The third, `phase3_before_syscall`, is rejected by `After`'s line at `MutationEnded`, as its regex says.
3. **`InodeForgetting`: precise.** The whole state is compared; only the forgotten names may go from a key to unknown; any other change is unexplained (`ForgottenInodeInATransactionHidesNothing`). It can't produce a pass anyway, because `invalidated` is allowed nowhere.
4. **Fills: holds, except finding 4.** `filled` is the code's own decision, and `T_GetattrWhole` uses it. The silent-refresh rule counts the directory's phase1 and end lines from every slot, so it doesn't miss another slot's mutation.
5. **Origins: complete by construction** (any other cause is `unexplained`).
   - `mkdir`, `listing` and `parent` are checked.
   - `existing` is trusted by design: `BeginAll`, and `RunStarted`, which also covers rows first seen after a restart. Recovery makes those conservative.
   - A new row's `valid` flag is never checked, but a new id can't hold a guard.
6. **dcfs.tla: holds.** `CrashSafe` still uses `RecoverDirty` (`dcfs.tla:958`), so it checks the least recovery forgets. The new `Recover` contains the old one (`forget = {}`), so no invariant or `Bug*` test is weakened. `T_Recover` is narrower than before.
7. **Fault wraps:** they are linked only into `dir_cache_fs_fault_*_test`. Neither `dir_cache_fs_test` nor `trace_recorder_test` links them.

## Production changes

There are more than the two syscall-start events:
- two more events: `ChildRowRecorded` and `InodeForgetting`;
- frame-end statuses through `Scope::Finish` (`FreshAttr`, `RefreshAttrs*`, `LookupOrPopulate`, `SyncBacking`);
- `filled` pulled out into a variable in `RecordChild` and `ParentOf`;
- signature changes to `PopulateCommitted` and `ResolveCommitted`;
- BUILD visibility for `//dcfs/testonly`.

All behave the same as before as far as I can read: same short-circuit order, same returned statuses.

## Files

- dcfs/testonly/trace_recorder.cc
- formal/trace_validate.sh
- dcfs/BUILD.bazel
- test/qemu/BUILD.bazel
- formal/Trace.tla
- formal/dcfs.tla
