# dcfs regression-test coverage audit

Read-only audit. Nothing in any repository was modified, committed, built, or run under
QEMU/Bazel. **Main was audited at `f6c4f31`** ("4.10: typed cache_state table replaces meta").
Main advanced twice during this audit (`03aa81b` -> `f6c4f31`, adding all five 4.10 commits);
this report covers everything merged to `main` as of `f6c4f31`. `step-4.8` (submount refusal)
and `step-4.11` (xattr tri-state) are separate worktrees, not yet merged into `main`, and are
out of scope.

Sources read: full `git log` on `main` (81 commits), `status.md`, `audit-races.md`,
`audit-crash.md`, `audit-tristate.md`, `audit-style.md`, and the diffs/tests for every
fix-shaped commit below (`git show <hash>`), plus the current test files
(`dcfs/*_test.cc`, `test/qemu/guest/*.sh`, `test/qemu/BUILD.bazel`).

Style-audit findings (dead code, clang-format, naming, BUILD hygiene) are not included: they
are not behavioral bugs, so "does a regression test cover them" does not apply.

---

## Coverage table

| # | Bug | Fix commit(s) | Test | Verdict | Reasoning |
|---|---|---|---|---|---|
| 1 | Errno table missing `EOPNOTSUPP` | `029309d` (1.1) | `ErrnoNameRoundTripTest.ExhaustiveOverAllErrnoValues`, `status_test.cc:40` | **COVERED** | Exhaustively round-trips every errno 1..134 `strerrorname_np()` names; a missing entry fails immediately. |
| 2 | `fgetxattr`/`flistxattr`: ERANGE misattributed to the query call instead of the read call | `bdfd61c` (1.2) | none found | **MISSING** | `SyscallsTest.FsetxattrFgetxattrFremovexattr`/`FlistxattrSplitsNulList` only exercise the happy path (value fits on first read); nothing shrinks/grows the xattr between query and read to force the ERANGE-retry branch. |
| 3 | `readlinkat`: silent truncation instead of `ENAMETOOLONG` at the `PATH_MAX*4` cap | `bdfd61c` (1.2) | none found | **MISSING** (low value) | No test creates a target long enough to hit the cap. Note: real filesystems cap symlink targets at `PATH_MAX` (4096), so this branch may be practically unreachable on any real backing fs — low priority to fix. |
| 4 | `FileDescriptor::Close()` not idempotent | `bdfd61c` (1.2) | `FileDescriptorTest.CloseIsIdempotent`, `dcfs/fd_test.cc:84` | **COVERED** | Calls `Close()` twice and asserts the second succeeds. |
| 5 | `Connection::Transaction`: unwind skipped when `body()` succeeds but `COMMIT`/`RELEASE` fails | `15f2ac9` (1.3) | `TransactionUnwindTest.FailedCommitUnwindsAndConnectionStaysUsable`, `dcfs/sqlite_test.cc:464` | **COVERED** | Forces a real `SQLITE_BUSY` on COMMIT (two connections, a held cursor, `busy_timeout=0`), then asserts `InTransaction()` is false and a subsequent transaction on the same connection succeeds. Directly exercises the exact failure path the fix touches; would fail on revert (`InTransaction()` would stay true and the second transaction would error). |
| 6 | Hand-written `SplitStatements()` SQL parser mishandled comments containing `;` | `a07a3f6` (2.1) | `ExecScriptTest.RunsMultipleStatementsAndSkipsCommentSemicolons`, `dcfs/sqlite_test.cc:279` | **COVERED** | Replaces the parser with `ExecScript`; test runs a script containing a comment with a semicolon and checks the row after it was still seen. The old code path no longer exists to regress, but the replacement's correctness on this exact input is directly asserted. |
| 7 | Zero-arg `BindAll()` triggered `-Wunused-but-set-variable` under `-Werror` | `c64f56c` (2.2) | build itself (`-Werror`) | **COVERED** | Compile-time issue; any CI build regresses immediately if reintroduced. |
| 8 | `FileHandle::FromDirEntry` broken on symlinks (ioctl/ELOOP on O_PATH fd); errno table missing 47 names found by an exhaustive check | `ff5b538` (1.4) | `FileHandleValueTest.FromDirEntryAcrossMountBoundaryDoesNotCrash` (`file_handle_test.cc:202`), `FileHandleTest.FromDirEntryOnSymlink` (`:177`); errno names via `ErrnoNameRoundTripTest.ExhaustiveOverAllErrnoValues` | **COVERED** | The mount-boundary test exercises the mount-id-comparison path the fix introduced (crossing into procfs); the symlink test exercises `FromDirEntry` on a symlink without the old O_PATH ioctl. The errno gap is covered by the same exhaustive round-trip as #1. |
| 9 | `readonly.sh` used GNU-only `mountpoint` / `find -ls`, not available in busybox | `a9567ac`, `526efec` (3.2) | `//test/qemu:readonly_test` itself | **COVERED** | Infrastructure fix — the test script does not run at all under busybox without it; any regression breaks the whole test visibly. |
| 10 | `open_by_handle_at` rejects `O_PATH` mount fds (EBADF); boundary dir reopen raced a rename via a second by-name lookup | `6f39bc5`, `0ee7778` (3.2) | `//test/qemu:readonly_test` (spans a real submount, vdb+vdc) | **COVERED** | The commit message states the bug was *found by* `readonly_test` failing this exact way once a subdirectory needed populating; the test still exercises populate-across-the-submount-boundary today, so a revert reproduces the original EBADF failure. |
| 11 | `Readdir`/`Readdirplus` pulled every directory entry from cache per call instead of stopping at the reply-buffer boundary | `49a9b9c` (3.2) | none found | **MISSING** | Author's own commit message: "No unit test added ... nothing to usefully assert without a live mount." No guest test creates a directory with enough entries to force more than one `getdents`-sized FUSE reply, so the boundary logic (`DirEntrySize`/`DirEntryPlusSize`) is never actually exercised at its edge. |
| 12 | `handles_test`'s `recycled-inode-estale` check was a false PASS (an `exec 3<` pinned the backing file across the restart, so the inode could never actually be recycled) | `ebd4969` (3.4b) | n/a — this commit *is* the test fix | **COVERED** (test-infra fix; see #13 for the real bug it uncovered) | Removing the pin is what let the real bug in #13 be observed at all; nothing regresses this specific script bug without reintroducing the `exec 3<` line, which is easy to eyeball but has no independent check. Low risk. |
| 13 | `Open()` returned plain `ENOENT` (not `ESTALE`) on the kernel's `LOOKUP_REVAL` retry against an already-invalidated nodeid | `b636371` (3.6) | `handles.sh` `recycled-inode-estale`, `test/qemu/guest/handles.sh:408` | **COVERED** | End-to-end reproduction of the exact interleaving described in the commit message (stale nodeid -> first OPEN ESTALE and row drop -> kernel LOOKUP_REVAL retry -> second OPEN). Fails ENOENT without the fix, as literally observed before the fix landed (see `ebd4969`'s commit message for the pre-fix failure text). |
| 14 | `syscalls::ioctl()` used `absl::ErrnoToStatus` (no errno payload), so `ReadGeneration()` couldn't tell ENOTTY from a real failure and aborted daemon startup on tmpfs | `0795d12` (3.6) | `SyscallsTmpfsTest.GetInodeGenerationOnTmpfsIsEnotty`, `dcfs/syscalls_test.cc:384`; `BackingTest` addition in the same commit for `ReadGeneration` | **COVERED** | Directly asserts `GetErrnoFromStatus()` recovers `ENOTTY` from a real tmpfs ioctl failure; fails on revert since `absl::ErrnoToStatus` carries no payload. |
| 14b | Same commit also switched several `FuseRequest::Reply*()` helpers from `absl::ErrnoToStatus` to `dcfs::ErrnoToStatus` | `0795d12` (3.6) | none found | **WEAK/MISSING** | These reply helpers only fail on rare conditions (double-reply, a dead fuse channel) that no test triggers; a revert of just this part would not be caught by anything in the suite. Low priority: the failure mode is "a log line loses its errno name," not wrong behavior. |
| 15 | `-o max_read=N` mount option rejected by libfuse (`conn.max_read` never copied from the parsed option) | `d560e94` (3.6) | `lifecycle.sh` `fuse-opt-good-mount`/`fuse-opt-good-visible`, `test/qemu/guest/lifecycle.sh:207-214` | **COVERED** | `fuse-opt-good-mount` asserts the daemon actually mounts with `--fuse_opt=max_read=65536`; without the fix, `do_init()` rejects the mount outright (verified against the commit's own description of the pre-fix error). |
| 16 | busybox `truncate -s N` opens O_WRONLY, which dcfs's `Open()` rejected until step 4.4, so `setattr.sh`'s truncate checks silently never reached `Setattr(FUSE_SET_ATTR_SIZE)` | `8be5799` (4.1) | `setattr.sh` `truncate-shrink`/`truncate-grow`/`truncate-EISDIR` via `tools/testutil.c`'s `truncate` subcommand | **COVERED** | `testutil truncate` calls `truncate(2)` directly (no `open()`), so these checks now genuinely exercise `Setattr`. This is a test-only fix, but it converts a previously-vacuous check into a real one; `setattr_test` is 37/37 today with this path live. |
| 17 | A freshly `mkdir`'d directory required a real listing to become `children_complete`; a create/link in an already-complete directory left it incomplete afterward (spurious full repopulate) | `c17f7f8` (4.2, within the initial merge, not a separate fix commit) | `create.sh` `warm-after-all-vdb`/`warm-after-all-vdc` (zero backing sectors read after a full metadata pass), `listing-matches` | **COVERED** | Commit message states these two fixes were "needed for the create_test acceptance checks (listing-matches, warm-after-all staying at zero backing reads)" — i.e. the test is what drove the fix, and would fail on revert (extra backing I/O from an unnecessary repopulate). |
| 18 | Two concurrent opens of one file: the kernel allows only one passthrough backing file per inode, so the second open got EIO | `6a473b1` (4.4) | `write.sh` `concurrent-opens-two-readers`, `concurrent-reader-and-writer`, `test/qemu/guest/write.sh:283-317` | **COVERED** | Directly reproduces two simultaneous opens of the same file and asserts both succeed; this is exactly the scenario found broken in step 4.3 (per `status.md`) and fixed by sharing one O_RDWR backing fd per inode. |
| 19 | `LookupOrPopulate`'s complete-directory fast path returned `ENOENT` instead of `ENAMETOOLONG` for a name > `NAME_MAX` | `fb089f0` (4.5) | `pjdfstest_test`'s `pjdfstest-no-regressions` check against `pjdfstest.expected_failures` | **COVERED** | Found by pjdfstest's `chmod/02.t` et al.; the baseline-diff mechanism (see #21) would surface this as a new dcfs-specific failure if it regressed. |
| 20 | Daemon's own process umask (typically 022, inherited from the launching shell) re-masked an already-final create mode a second time | `d136601` (4.5) | same `pjdfstest-no-regressions` mechanism | **COVERED** | Found by pjdfstest's `open/02.t`/`open/03.t`; same baseline-diff argument as #19. |
| 21 | `pjdfstest.sh`'s set-difference (`awk NR==FNR` / `grep -vFxf`) silently produced zero output when the baseline file was empty, so the very first real run wrongly PASSed with 175 dcfs-specific failures unreported | `5264954` (4.5) | `pjdfstest.sh`'s own `set_diff()` plus `pjdfstest-no-regressions`/`pjdfstest-no-out-of-band` | **COVERED** | The commit says this was caught specifically by testing with a genuinely empty first argument, which is exactly the shape of the bug; the new helper is exercised on every run since the baseline always starts empty for a schema/behavior change. `pjdfstest-no-out-of-band` (added in the same commit) is a real, currently-zero assertion that would catch a false-positive out-of-band warning from pjdfstest's own churn. |
| 22 | Writable-open attrs stayed "valid" (stale) across a crash until Flush/Fsync/Release; no out-of-band-change detection | `37cdcb1` (4.6) | `crash.sh` `crash-open-file`/`crash-created-file` (src/mnt size+mtime comparison after SIGKILL + restart), plus `crash-no-out-of-band-first-run`, `out-of-band-logged-once`, `no-false-positives` | **COVERED** | Per `status.md`: "crash_test proven to fail on the pre-fix daemon via tools/testutil writehold." `check_same` directly compares cached vs backing size/mtime post-crash; would serve stale (pre-crash) values without the fix. |
| 23 | `Release()` could return early on an attribute-refresh failure, leaking the passthrough registration/fd and never dropping bookkeeping | `2a4f672` (4.6) | none found | **MISSING** | No fault-injection: nothing in the suite makes `RefreshAttrsFromFd`/`MarkAttrsUnknown` fail during a real `Release()`, so the refactor from `ABSL_RETURN_IF_ERROR` to "log and always proceed" is not exercised on its failure branch by any existing test. `dcfs/dir_cache_fs.cc` has no unit test file at all (`DirCacheFS` is only reachable through the QEMU e2e tests). |
| 24 | Power loss/kernel crash could roll back the WAL tail after a backing syscall committed (or vice versa), leaving the cache ahead or behind the backing filesystem in both directions (race-audit F5, crash-audit F1) | `46d6218`, `a07b3f6` (4.10) | `power.sh` / `//test/qemu:power_test`: `recovery-logged`, `lost-create-forgotten`, `lost-unlink-forgotten`, `lost-rename-forgotten`, `lost-chmod-forgotten`, `lost-listing-relisted`, `sync-point-emptied-dirty-set` | **COVERED** | Test SIGKILLs the daemon with the dirty set intact, then *undoes each mutation directly on the backing filesystem* to simulate the exact "backing lost the op, DB kept it" direction, and restarts. Commit message: "With recovery disabled the six lost-mutation checks fail." This is a true fault-injection test, not just a happy-path e2e run. |
| 25 | Sequential `gen_counter`-derived generations meant a power-loss-rolled-back insert could reissue an old `(nodeid, generation)` pair for an unrelated new object (crash-audit F2) | `e973c20` (4.10) | `MetadataCacheTest.ReusedIdAfterRollbackGetsNewGeneration`, `dcfs/metadata_cache_test.cc` (new test added in this commit) | **COVERED** | Directly simulates the rollback (`db_.Transaction` that inserts a row then returns `AbortedError`, forcing SQLite to roll it back), then upserts a new row and asserts it reuses the same id but gets a *different* generation. This is the exact scenario the audit describes, reproduced deterministically without needing real power loss. |
| 26 | `(device, ino, generation)` alone did not identify an object: generation 0 covers symlinks/specials/no-ioctl filesystems, and btrfs can reissue an identical handle after its own power loss (crash-audit F5/F6) | `3ec9a92` (4.10) | `MetadataCacheTest.DifferentHandleOrBirthTimeIsANewObject`, `dcfs/metadata_cache_test.cc` | **COVERED** | Exercises all three sub-cases the fix introduces: different handle bytes at the same (dev,ino,gen=0) creates a new row and invalidates the old one; identical handle but different birth time also creates a new row; and a birth time missing on either side matches (no false invalidation). |
| 27 | `RecordAttrs` could cache `nlink=0` as "current" for a still-open unlinked file; a crash between that and `Release`'s row deletion left it served as valid instead of unknown (crash-audit F8) | `3ec9a92` (4.10) | `BackingTest.AttrsWithNoLinksLeftStayUnknown`, `dcfs/backing_test.cc` | **COVERED** | Opens a file, unlinks it, refreshes attrs from the fd, and asserts the cached row is `valid=false` (unknown) with `nlink=0` — i.e., never served as current. |
| 28 | `test/qemu/BUILD.bazel` left in a broken state by a bad rebase-conflict resolution | `9052480` (3.5) | whole test suite (build must succeed) | **COVERED** | Infra fix; nothing builds without it. |
| 29 | libfuse's carried `FUSE_ATTR_GENERATION` patch used a stale bit (pre-7.46-io_uring-rebase) instead of matching the actual kernel's bit 44 (protocol 7.47) | `36e5fa9` (0.3) | none found directly; indirectly, any test relying on NFS generation numbers (`handles_test`, `nfs_test`) | **WEAK** | If the kernel and the libfuse patch both used a *consistently* wrong bit, no test would notice (both sides would silently agree on the wrong meaning) since the kernel source and the carried patch are both vendored together at a matching commit. Nothing directly asserts the capability bit value itself; coverage is incidental at best. |
| 30 | Guest test scripts truncated their last serial output line on `reboot -f` right after `sync` (which does not flush the emulated UART), causing flaky `ALL-TESTS-PASSED` detection | `771a008`, `75ec504` (5.1b) | every `qemu_test`'s verdict-detection mechanism in `run-qemu.sh` | **COVERED** | Infra/flake fix; verified in the commit by 3x back-to-back runs of 5 different tests with zero flakes after the change. A regression would show as intermittent CI flakiness across the whole suite, which is the kind of thing that "visibly breaks" over a few runs, if not deterministically on the first one. |
| 31 | Missing `/etc/mtab` crashed `rpc.mountd` via a NULL deref; `nfsd` pins the old vfsmount across a dcfs restart, requiring `exportfs -f` | `111502a` (5.3) | `nfs.sh`/`//test/qemu:nfs_test` — the entire test depends on `rpc.mountd` starting; `restart_daemon`'s `exportfs -f` call is exercised by the restart-then-access checks | **COVERED** | Both fixes are infra baked into the same commit that introduces the test; without the `/etc/mtab` symlink, `nfs_test` cannot boot `rpc.mountd` at all (visible, total failure); without `exportfs -f` after restart, the post-restart NFS access checks would see stale export state. |

### Findings from the audits already fixed by the above

- **Crash-audit F1** (durability ordering across power loss) → fixed by `46d6218`/`a07b3f6`, COVERED (#24). Note: the audit's recommended fix was a *global* unclean-shutdown invalidation; what landed is a *bounded* dirty-set recovery (only inodes touched since the last sync point). This closes the "phase 1 lost, backing kept" direction for tracked mutations, which is what `power_test` checks. The audit's residual "cache ahead of the backing" direction (a **committed and sync-pointed** mutation, where the *backing* filesystem's own journal commit is later lost) is narrower than before but not fully eliminated by design (`SyncBacking`'s `syncfs()` reduces but doesn't zero this window) — this residual is not tested and, per the audit, is architecturally hard to close without a per-op backing flush.
- **Crash-audit F2** (generation reissue after power loss) → fixed by `e973c20`, COVERED (#25).
- **Crash-audit F5/F6, tristate-audit F9** (generation-0 / recycled-handle identity) → fixed by `3ec9a92`, COVERED (#26).
- **Crash-audit F8** (nlink-0 served as current) → fixed by `3ec9a92`, COVERED (#27).
- **Crash-audit F3 / race-audit F6 / tristate-audit F5** (`CreateChild`/`BeginCreate` phase 1 does not mark the parent's attrs unknown) — **STILL UNFIXED on `main` at `f6c4f31`.** Verified directly: `BeginCreate` (`dcfs/metadata_cache.cc:945-949`) only calls `MarkUnknown(ctx, parent, names)` (the dentry), never `MarkAttrsUnknown(ctx, parent)`, unlike `BeginRemove` two functions below it which does both. This is out of scope for the coverage table (not yet fixed), but flagged here since all three audits called it out independently and it is a one-line fix.

---

## Prioritized MISSING / WEAK list with concrete test proposals

1. **`Release()` fd/refcount leak on attribute-refresh failure (#23, `2a4f672`).** MISSING.
   - **Kind:** unit test, but it requires fault injection that the current `BackingTest`
     fixture (real files on a real tmpfs/testdir) cannot easily provide, since
     `RefreshAttrsFromFd`'s only failure modes are a bad fd or a SQLite error.
   - **Setup:** the cleanest injection point is SQLite, not the fd. In a `BackingTest`-style
     fixture, open a real file, mark it dirty/being-released, then force the `MarkAttrsUnknown`/
     `UpdateAttr` commit inside `RecordWrittenAttrs`'s path to fail — e.g. close the underlying
     DB file out from under the connection, or (simpler) add a narrow seam: expose
     `RecordWrittenAttrs`-equivalent logic at the `backing::` layer (it may already be
     reachable as `RefreshAttrsFromFd` plus `MarkAttrsUnknown`) and call it after revoking
     write access to the DB file (`chmod 0400` won't fail an already-open fd; instead, run the
     DB on a `tmpfs` mounted read-only after open, or inject a `PRAGMA` that makes the next
     write fail, e.g. filling `SQLITE_FULL` via `PRAGMA max_page_count`).
   - **What to assert:** after the forced failure, (a) the function returns/logs rather than
     propagating an error that would abort the caller, (b) the open-file bookkeeping
     (`open_files_`/`backing_files_`, or their `backing::`-layer equivalents) is still fully
     torn down — no leaked fd, no leaked passthrough id — and (c) the attrs are left/re-marked
     unknown, not silently valid.
   - **Demonstrate it fails without the fix:** revert `2a4f672`'s hunk in
     `dcfs/dir_cache_fs.cc` (the `ABSL_RETURN_IF_ERROR(backing::RefreshAttrsFromFd(...))` calls
     in `Release`) on a scratch branch and confirm the new test now leaks (e.g. `backing_files_`
     still contains the entry, or the fd is still open via `/proc/self/fd`).
   - Alternative, lower-effort: an e2e `crash.sh`-style check isn't a good fit since this needs
     an in-process failure, not a kill; a unit test is the right level here even though it
     needs a small testing seam to be added to `Context`/`backing.h`.

2. **`CreateChild`/`BeginCreate` doesn't mark the parent's attrs unknown (unfixed; three audits agree).** Not yet fixed, flagged for when it is.
   - **Kind:** unit test (crash-window) plus one guest-test line.
   - **Setup (unit):** in a `BackingTest`/`MetadataCacheTest`-style fixture, call
     `RecordAttrs`/`UpdateAttr` to mark a parent's attrs valid with a known mtime, then call
     `BeginCreate(ctx, parent, name)`, then assert `GetAttr(ctx, parent).valid` is now false.
     Today this would fail (attrs stay valid), which is exactly the bug.
   - **Setup (guest):** in `create.sh` or a new `crash.sh` case, `mkdir` (or `touch`) a new
     entry, `SIGKILL` before any subsequent access to the parent, restart, and assert the
     parent's cached mtime moved to match the backing filesystem's post-create mtime rather
     than serving a stale pre-create mtime marked valid. This would need the daemon to expose
     a hook to crash mid-op (as `testutil writehold` does for writes) or simply crash right
     after the create syscall returns, before any other request — feasible by racing a
     background `kill -9` immediately after the `mkdir` call returns in the shell.
   - **Demonstrate it fails without the fix:** the unit test above fails today (bug is present)
     without any reverting needed — this is a genuine gap, not a false negative.

3. **`Readdir`/`Readdirplus` buffer-boundary logic (#11, `49a9b9c`).** MISSING.
   - **Kind:** guest test (needs FUSE wire-format-sized replies, which the author's own
     commit message says can't be usefully unit-tested).
   - **Setup:** extend `create.sh` (or add a new small guest test) to create a directory with
     enough entries that a single `getdents64()` buffer (typically 32KB from a shell
     `ls`/`find`) cannot hold them all in one FUSE `READDIR`/`READDIRPLUS` reply — a few
     hundred entries with moderately long names is enough (e.g. 500 files named
     `file-000` .. `file-499` plus some longer names to push total size past 32KB).
   - **What to assert:** a full listing (`ls -a` or `find`) still enumerates every entry exactly
     once, matching a normalized listing of the same tree on the backing filesystem directly
     (reuse the existing `listing-matches` comparison machinery in `create.sh`). This is a
     correctness assertion, not just a crash check.
   - **Demonstrate it fails without the fix:** revert `49a9b9c`'s `DirEntrySize`/
     `DirEntryPlusSize` early-stop logic back to "pull every entry from the cache regardless of
     reply size" on a scratch branch; if the pre-fix code had a real correctness bug (not just
     a performance one) at the boundary, this test would catch it. If the pre-fix behavior was
     merely slower but still correct, this test would not show a regression — worth confirming
     which is actually true (the audit did not analyze this path) before investing in the test.

4. **`fgetxattr`/`flistxattr` ERANGE-retry race (#2, `bdfd61c`).** MISSING.
   - **Kind:** unit test.
   - **Setup:** in `SyscallsTest`, set a small xattr value, then — between the size-query call
     and the read call — grow the value. This can't be done via the public `syscalls::`
     wrapper alone (it does both steps internally), so either (a) test the two-step behavior
     at a lower level by calling the underlying `fgetxattr`/`flistxattr` twice manually to
     confirm the wrapper's retry-on-ERANGE-from-read (not from-query) logic, or (b) add a
     `syscalls_test.cc` case that pre-sizes a buffer artificially small via a test-only
     override, if the code has a seam for that (may not currently exist — likely needs a
     small refactor to inject the buffer size, which is a larger change than the bug
     warrants).
   - **Simpler alternative:** at minimum, add a test with a large xattr value (a few KB) to
     confirm the size-query-then-read path successfully round-trips a value that requires
     more than one default-sized buffer, even without deliberately racing a concurrent writer.
     This at least exercises the "query returns size > 0, allocate, read" path end-to-end,
     which today's tests (small, fixed values) do not.

5. **`0795d12`'s `FuseRequest::Reply*()` helpers (#14b).** WEAK, low priority.
   - Not worth a dedicated test: the failure paths (`fuse_reply_*` returning nonzero) require
     simulating a broken FUSE channel, which is out of proportion to the bug (a lost errno name
     in a log line). Documented here for completeness; recommend leaving as-is unless a real
     incident surfaces needing it.

6. **`36e5fa9`'s FUSE protocol bit correctness (#29).** WEAK, low priority.
   - If ever worth hardening: a small unit test could statically assert
     `FUSE_ATTR_GENERATION == (1ULL << 44)` against the vendored kernel header's
     `include/uapi/linux/fuse.h` value (a `static_assert` comparing the two macros, if both
     are visible in one translation unit) so a future libfuse bump can't silently drift back
     to a stale bit without a build break.

---

## Summary

Of the 31 fixed-bug/fix-commit entries examined, **26 are COVERED** by a test that would
concretely fail if the fix were reverted (verified either by reading the fix diff and reasoning
about the failure mode, or, where stated by the commit itself, by the fact that the test is
what originally found the bug). **2 are WEAK** (exercise adjacent code but not the specific
failure branch: `#14b`, `#29`). **3 are MISSING** (`#2`/ERANGE retry, `#3`/readlinkat cap,
`#11`/readdir boundary, `#23`/Release leak — four, correcting the count) with no test that
would catch a regression at all. Additionally, one bug flagged by three independent audits
(`CreateChild`/`BeginCreate` not marking the parent's attrs unknown) remains **unfixed** on
`main` as of `f6c4f31`, confirmed directly against `dcfs/metadata_cache.cc:945-949`.

The strongest tests in the suite are the ones that reproduce the actual bug mechanism rather
than just re-running the feature: `power_test`'s backing-filesystem rollback simulation,
`MetadataCacheTest.ReusedIdAfterRollbackGetsNewGeneration`'s explicit transaction-abort
simulation, `TransactionUnwindTest`'s real two-connection `SQLITE_BUSY`-on-COMMIT, and
`crash.sh`'s `testutil writehold` for crash-mid-write. The weakest spots are consistently
error-injection gaps in `DirCacheFS` (which has no unit test file at all — every path through
it is only reachable via QEMU e2e, so any failure-path bug inside it, like the `Release` leak,
has no fast, deterministic regression test) and defensive code paths that are difficult or
impossible to reach with real syscalls (the `readlinkat` cap, the ERANGE race).
