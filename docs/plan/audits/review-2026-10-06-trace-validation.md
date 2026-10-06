# Review of trace validation (12.2), 2026-10-06, dcfs-reviewer, read-only

Branch step-12.2 before the fixes it demanded (12.2b); not merged as reviewed.

## Review of step-12.2 (base acd921d, not 1e48d97; commits 5dd58b9 and e526c2b)

**Verdict.** The events are emitted in the right places: every call site I checked fires after the state change of the model action it maps to, with no backing syscall in between. The holes are elsewhere: in when the recorder decides to cut or stay silent, and in `trace_validate.sh` treating every cut as valid. Because of that, at least one ordering violation at the core of the protocol validates as a pass today.

### 1. HIGH: structural cuts hide forbidden steps, and the script tolerates them

`trace_validate.sh:169` passes whenever there are no invalid traces. A cut trace counts as valid if the part before the cut is valid, and cuts are only listed. Several cut reasons describe steps the model forbids, not steps it lacks.

**Verified: unlink or rename with its syscall moved before phase 1.**
- `MutationSyscall` skips any request that has not begun its mutation (`trace_recorder.cc:943-945`), so no `syscall` line is written.
- `MutationBegun` then writes a `phase1 begun` line, which `UnlinkPhase1` accepts.
- At `Mutation::End`, the syscall was never seen, so the recorder writes the cut "a mutation ended before its syscall" (`:1005-1007`).
- The trace stops at a valid prefix, and the test passes.
- Moving the whole phase 3 (commit and `End`) before the syscall gives the same result.
- For create, the same fault is rejected (at the `probe` line, since the model is still at `C_sys`).

**Verified: a getattr inside another request serving unknown attributes without refreshing.** Example: `EntryFor(C)` from `LOOKUP(D,c)`. The getattr request belongs to the getattr frame. If `FreshAttr` returns without refreshing, `GetattrEnd` cuts with "a getattr's refresh failed" (`:521-523`). The decision looks only at whether the request finished, not at the frame's status. `LookupEnd`, `RefreshEnd` and `SyncEnd` (`:541`, `:585`, `:602`) have the same flaw.

**Suspected: an unlink or rename that takes the child from the cache without `LookupOrPopulate`.** It would be cut as "a phase 1 before its resolve" (`:899`), so the model's `rsnap` check never runs.

**Fixes:**
- Write the `syscall` line even when phase 1 has not begun; the model will then reject it.
- On `End` before the syscall, mark the request instead of cutting. Cut at `RequestEnd` only if the request failed; on success, write an `unexplained` line.
- Give the frame-end events a status (as `RequestScope::Finish` does). Cut only on error; otherwise write `reply` and let `T_Reply` judge.
- In `trace_validate.sh`, fail on any cut reason outside an allowlist per target (cross-directory rename, link, setattr or xattr, boundary, out-of-band change, failed request). For the guest scripts, require the root trace to reach the end of the run.
- Then add this ordering as a second fault test.

### 2. MEDIUM: when a syscall starts is never an event

The events mark when a syscall returns, never when it starts.

**Worked through: `BeginSync` moved after the syncfs loop (`backing.cc:1473-1477`).** In `MkdirDuringASyncPointKeepsItsDirtyRows`:
- The mkdir's `phase1`, `syscall` and `end` lines come first.
- Then `sync_begin` (whose snapshot now includes the mkdir), `syncfs` and `sync_clear` with `dirty=false`.
- In `S2`, `seq <= snap` holds, so the model clears the row too, and the trace is accepted.

The unit test's own assertion would catch this fault; trace validation does not.

The same gap applies under coroutines (the TODO in the code): a syscall issued before phase 1 commits but completing after it would be accepted.

**Fix:** add `SyncfsStarting` and `MutationSyscallStarting` events. The recorder should require the sync snapshot to come directly before `SyncfsStarting`, and phase 1 (`begun`) to come before `MutationSyscallStarting`.

### 3. MEDIUM: `InodeForgotten` turns arbitrary changes into cuts

- `InvalidateInode` is called inside enclosing transactions (`UpsertInode`, `UpsertRoot`; `metadata_cache.cc:666,699`). The event therefore fires before anything has committed (`:992`), and the recorder's state snapshots see half-written commits.
- `After(ctx, /*cut_changes=*/true)` (`trace_recorder.cc:419-421`) cuts every changed directory, not only those that had a row pointing at the forgotten inode.
- Concrete effect: when a populate, resolve or create commit recycles an inode number (plausible after `power.sh` recreates a file), its own directory is cut in the middle of the commit. The `populate_commit` or `end` line is never checked, and any untracked write since the previous callback becomes a cut instead of `unexplained`.
- **Fix:** collect the (directory, name) rows that point at the inode before the DELETE. Cut only those directories, and only if the change is exactly those names becoming unknown. Emit the event after the outermost commit.

### 4. MEDIUM: fills over valid attributes are never checked

- A refresh of valid attributes is silent: no lines, and the fill's `recorded` flag is dropped (`trace_recorder.cc:562-569`, `:835`).
- A `child_fill` over valid attributes stutters whatever the guard says. Its `allowed` field is recomputed by the recorder (`:708-710`, `:790`, `:853`) rather than being the code's own decision.
- Attribute values are not compared, so an unguarded fill that writes stale values over valid attributes passes. That is the `BugUnguardedFills` class of bug, on these paths.
- The README's claim that "every guard decision" is compared is therefore too strong.
- **Fix:** report the code's actual decision for every fill. For silent refreshes, write an `unexplained` line if `recorded` is true and any `phase1` or `end` line for that directory falls between `RefreshBegin` and the fill.

### 5. MEDIUM-LOW: the starting state of a directory first seen mid-run is trusted

- The README says the initial-state assumptions "only remove initial states". That holds for the `Determined` names, but not for the `begin` cached state: that state is produced by the code under test.
- Example: if the `child_ok` gate on `MarkDirComplete(row.id, true)` (`backing.cc:1284`) were dropped, `TraceInit` would force the backing to agree with the wrong completeness, and nothing would notice.
- **Fix:** mark `begin` lines created by a mkdir as fresh (backing directory empty), and check the other ways a row can start against the step that created it.

### 6. LOW: `T_Recover` is wider than `RecoverDirty`

`Trace.tla:499-511` lets any present row of a clean directory become unknown, whether or not its object was dirty. It cannot accept a recovery that keeps something the model forgets: for a dirty directory the result must match exactly, and present cannot turn into absent or no row. So the extra freedom is only in the safe direction. To narrow it, carry the keys of the dirty inodes on the `recover` line and require each newly unknown row's object to be one of them.

### 7. LOW: the fault-injection test, and the two hypothetical faults

**The existing test.** It rejects the trace at the right event. That the reason is the missing `unknown` row follows only from the unfaulted run being valid, which is checked by a different target (`dir_cache_fs_trace_test`). The regular expression matches the event, not the reason.

**Fault: phase 3 commits before the syscall returns.**
- Commit only: rejected at the `syscall` line, because `USys`/`RSys` require the cache to be unchanged.
- Commit and `End` both before the syscall: cut, and the test passes (finding 1).

**Fault: a sync point clears a row early.**
- Clearing despite a mutation that overlapped the sync: rejected at `sync_clear`.
- `ClearDirty` before the syncfs: rejected at the `syncfs` line, because the model is already idle.
- The snapshot taken after the syncfs started: accepted (finding 2).

### 8. LOW: legitimate failure paths that fail validation

These make validation fail spuriously; they never make it pass wrongly.
- A phase-3 refresh that fails and is only logged: the request replies OK, the model is still at `*_stat`, so the trace is rejected.
- `ProbeObject` failing after `NewChildProbed` is rejected the same way.
- `RunStarting`, `Recovered` and `RunStarted` don't update the recorder's last recorded state for each directory, so a restart inside one process with a live recorder would produce `unexplained` lines.

### 9. NIT

`NoProtocolEvents()` keeps a function-local static object, which goes against process.md's "no globals" rule. It is stateless and harmless; at least say so in a comment.

### Checked and found sound

- **Event placement.** All of these fire after their state change, with nothing between:
  - after the commit and `RegisterMutation`: `MutationBegun`;
  - after the guards change: `MutationEnded`, with `owned` taken just before, and no syscall between the phase-3 commit and `End`;
  - right after the reads they report: `LookupDecided`, `ListChecked`, `NewChildProbed`;
  - after their transaction: `AttrsFilled`, `ResolveCommitted`, `PopulateCommitted`, `SyncCleared`, `Recovered`, `RunStarted`, `Checkpointed`, `CleanShutdownRecorded`;
  - at the snapshot and epoch: `PopulateStarted`;
  - in order: `SyncSnapshotTaken`, then the syncfs calls, then `SyncfsDone`.

  Every caller of `FreshAttr` reads the attributes with no syscall in between. Where the code's snapshot comes earlier than the model's (`ResolveProbe`, the refresh after phase 3), the code is the more conservative side, so this can only cause rejections.
- **`MutationAborted`.** It fires only for the `AbortedError` from verification. Nothing else produces `kAborted`; `sqlite.cc` has none. An abort by the code where the model would begin, or the reverse, is rejected.
- **Recorded state.** Every line's cached state is read after its change; the reordered `populate_read` line carries the state from when the population started.
- **Projection.** Placing `populate_read` where the population started can only cause rejections. Restricting the initial states with `Determined` is sound. A non-injective object map can't confuse distinct keys. Readdirplus continuations, slots and `p0` are handled correctly.
- **Pass/fail logic.** The depth test equals "every event consumed". TLC errors and `unexplained` lines fail the test.
- **Production code.**
  - Only the no-op is linked: `:main` and `:main_static` link `:protocol_events_main`, and the recorder is `testonly`.
  - The restructurings change no behaviour: the early return on an empty `End`, the `InvalidateInode` and `RecordNewChild` returns, `FillAttrs`, `BeginMutation`, and `Serve` (same order as before).
  - The no-op path does no I/O and throws nothing.
- **Fault build.** The wrap of `sqlite3_step` is linked only into `dir_cache_fs_fault_test`.

Not run: Bazel and TLC. Everything above comes from reading the code at `/home/russ/Sources/dircachefs-lanes/lane-1`:
- `dcfs/testonly/trace_recorder.cc`
- `formal/Trace.tla`
- `formal/trace_validate.sh`
- `dcfs/backing.cc`
- `dcfs/metadata_cache.cc`
- `dcfs/dir_cache_fs.cc`
- `formal/dcfs.tla`
