# Gemini consistency sweep, re-verified (step 26.31)

**What was swept.** The tree at main c645222: README.md, docs/design.md, the
`dcfs/` sources and tests, `test/qemu/`, `bench/`, `tools/` and the plan files.
**By whom.** Gemini 4 Argon through `agy`, one prompt with five questions: (1)
statements in the README or design.md that the code contradicts, (2) README
promises no test checks, (3) stale references, (4) dead code, (5) duplicated
logic that could drift. It returned 35 findings, all marked `[certain]`.
**How each was checked.** `docs/plan/process.md`, "Using Gemini through agy",
rule 5: the findings are leads. For every one of the 35 I opened both cited
sides myself (the file and line it names, and the code or document it is said
to contradict), `grep`ped for callers and tests, and ran `sh -n` on the shell
claims. `bazel query` was tried for the dead-code claims but the Bazel
repositories cannot be fetched on this machine (no network), so reachability
rests on `git grep` over `dcfs/`, `bench/`, `tools/`, `test/` and every
`BUILD.bazel`/`.bzl`. Nothing was built or run, and nothing was fixed here: the
fixes get their own steps. The raw report stays in the git-ignored
`.scratch/`.

**Verdicts.** CONFIRMED: the core claim holds (a wrong citation or a wrong
sub-claim is noted in the row). PARTLY: part of the claim holds and part is
false. WRONG: the claim does not hold. Fix sizes: mechanical (doc or comment
edit, pattern deletion), implementer (code and tests, no protocol), protocol
(none needed).

**How reliable the citations were.** 17 of the 35 findings cite a function,
file, quote or line that does not exist or does not say what Gemini says
(README text in category 2 that is nowhere in the README; `DiscoverMountsBelow`,
`SourceIsDirectory`, `ret_check.cc`, `status_errno.h`, `sql_get_gen_` and
`tools/coverage/` do not exist; a quoted design.md sentence about
`wal_autocheckpoint` is not in design.md). Where the substance was right
anyway, the row gives the real location.

## Urgent: a real bug, not a doc nit

**3.1 is why every xfstests shard is red.** Since 15.11 (95dafad) `RunDaemon`
opens SOURCE with `O_RDONLY|O_DIRECTORY` (`dcfs/main.cc:322-325`), so a block
device is refused with `ENOTDIR`. `test/qemu/guest/xfstests.sh:50-51` sets
`TEST_DEV=/dev/vdb`, `SCRATCH_DEV=/dev/vdc`, and the `mount` wrapper in
`test/qemu/scripts/mkxfstests_rootfs.py:219-284` runs
`mount -t fuse.dcfs ... DEV DIR` with the device as SOURCE. Nothing mounts the
device first, because the native-type capture that did was deleted. This
matches `docs/plan/log.md:3396-3402` ("Every xfstests shard on all three
filesystems failed within a minute, which points at setup"; 26.30 is
triaging). I did not run it; the cause is established by reading both sides
and by the diff of 95dafad's `main.cc` (`OpenBacking(args, options)`, which
took devices, replaced by the direct `openat`).

**4.1 is a test that has never fully run.** `test/qemu/guest/names_random.sh`
fails `sh -n` at line 119 (`Syntax error: "fi" unexpected`). The script has
carried a duplicated, unparseable tail since it was created (cf6af89,
2026-10-05; `sh -n` fails on every revision). Shells parse and run one command
at a time, so `exit "$FAILED"` at line 118 ends the script before the parser
reaches line 119, and the guest never notices. The cost: the check
`dcfs-created-matches-reference` (lines 137-140) has never run. It is the only
script of 151 shell files in the tree that fails `sh -n` (I ran it, with bash
for the `#!/bin/bash` ones), and no check in the repository runs `sh -n` or
shellcheck on guest scripts (style.md lists shellcheck as not yet pinned).

## Findings

### 1. README or design.md contradicted by the code

| # | Gemini's claim | Verdict | Evidence | Fix and size |
|---|---|---|---|---|
| 1.1 | design.md says schema version 4, code is 6 | CONFIRMED | `docs/design.md:491` "schema version 4", `:494-495` "to version 4 ... The step to version 4 adds the `stubs` table"; `dcfs/migrate.h:40` `kSchemaVersion = 6` (v5 `last_stub_id`/`inodes_unlinked`, v6 `dirty.atime_only`, `migrate.h:34-39`). design.md itself knows v6 (`:650` "schema v6"). Also `dcfs/schema.sql:2` "dcfs schema v4". | Say "version 6" and list v5 and v6 steps in design.md; fix the schema.sql header. Mechanical. |
| 1.2 | README gives baseline 92.40/73.30, file has 96.50/81.24 | CONFIRMED | `README.md:886` "`lines 92.40`, `branches 73.30`"; `dcfs/coverage_baseline.txt` is `lines 96.50`, `branches 81.24`. `README.md:881` ("The first baseline ... 92.4%") is history and true. Same stale numbers in the comment at `tools/coverage_gate.sh:9`. | Drop the numbers from the README sentence (or point at the file), fix the gate comment. Mechanical. |
| 1.3 | design.md says `FuseRequest` is movable, code deletes the moves | CONFIRMED | `docs/design.md:1348-1349` "`FuseRequest` is movable and owns exactly one reply"; `dcfs/fuse_request.h:73,75` `FuseRequest(FuseRequest &&) = delete;` and `operator=(FuseRequest &&) = delete;`, with the comment at `:68-71` saying it is deliberately neither copied nor moved. | Reword design.md ("not movable: one per libfuse callback"). Mechanical. |
| 1.4 | design.md says SOURCE is opened `O_PATH` and reopened via `/proc/self/fd`; code opens it directly (and submounts via `open_by_handle_at`) | PARTLY | Holds: `docs/design.md:2838-2839` "dcfs opens SOURCE with `O_PATH` before mounting", and `:2058-2060` "After opening, it turns the descriptor into a real directory descriptor" are stale; `dcfs/main.cc:322-325` opens `O_RDONLY\|O_DIRECTORY` in one call. False: no design.md sentence says SOURCE is reopened through `/proc/self/fd` (`:312-315` says the mount fd must be a real directory fd, not `O_PATH`, and is accurate); `DiscoverMountsBelow` does not exist (`mounts_below.h` has `MountsBelow`, `RefuseMountsBelow`), and nothing opens submounts. | Fix the two design.md passages. Mechanical. |
| 1.5 | design.md says failing to restore root is fatal; code only poisons; `CheckNotPoisoned` is in 6 wrappers | PARTLY | Holds: `docs/design.md:1872` "failing to restore is fatal", while `dcfs/backing.cc:278-290` (`RestoreRoot`) logs, calls `syscalls::PoisonBacking()` and `fuse_session_exit`, no abort (25.9); design.md says nothing of poisoning (`grep -i poison docs/design.md` is empty). False: there is no `CheckNotPoisoned` and the poison check is not in `syscalls.cc`; it is `IsBackingPoisoned()` in `dcfs/syscalls_backing.cc`, and all 42 backing wrappers start with it (42 `if (IsBackingPoisoned())` for 42 wrappers: every wrapper from `openat` at `:47` to `umount2` at `:566`), including `statx`, `getdents64`, `fsetxattr`, `fsync`. Gemini read the wrong file. | Reword design.md:1872 and document the poison. Mechanical. |
| 1.6 | Logging table promises FATAL/`CHECK`; README promises one log per error; code logs twice via `RetCheckFail` | PARTLY | Holds: `docs/design.md:2398` FATAL row's example `check.cc] Check failed: fs != nullptr` is stale: no production `CHECK`/`LOG(FATAL)` exists (`git grep` over `dcfs/*.cc,*.h` minus tests and `testonly/`: zero; `GetFS` at `dcfs/fuse_ops.cc:31-36` uses `RET_CHECK_NE`; no `check.cc`). FATAL survives only in the testonly invariant checker (`dcfs/testonly/invariant_checker.cc:318`, design.md:2571). False: `dcfs/ret_check.cc` does not exist; `RetCheckFail` (`dcfs/ret_check.h:31-35`) only builds an `INTERNAL` `StatusBuilder` and logs nothing, so `README.md:730`'s "logged once by the request that failed" holds. | Rewrite the FATAL row (testonly checker only). Mechanical. |
| 1.7 | Docs say a failed `Init` refuses the connection; code does not when all 64 `capable_ext` bits are offered | PARTLY | Holds as code: `dcfs/fuse_ops.cc:96-104` logs "cannot refuse the INIT: every flag is offered" and returns. Not a contradiction in practice: `README.md:1079-1081` is the OSV table, the only doc text is `docs/design.md:1919-1921` ("libfuse then refuses the INIT"), and the kernel's `capable_ext` never has 64 bits set. The branch is untested (`session_loop_test.cc:74` fakes Init without it) and has no reachable cause: a style 1.10a finding rather than a doc one. | Delete the branch or RET_CHECK the invariant (25.9 introduced it). Implementer. |
| 1.8 | design.md says the WAL is only checkpointed passively by SQLite and at shutdown; code also runs a periodic passive checkpoint | PARTLY | The code is as described: `dcfs/dir_cache_fs.cc:625-640` `MaybeCheckpointWal`, called at `:644` when no sync is due, once per `sync_interval` (comment: russ 2026-10-12, permitted exception to the no-timer rule), tested at `dir_cache_fs_test.cc:3290-3307`. But the quoted design.md text ("wal_autocheckpoint ... `FinishRun()` runs `wal_checkpoint(TRUNCATE)`", "At most one sync point per ...") is not in design.md (`grep -i autocheckpoint` over README, design.md, style.md: empty); design.md `:505`, `:1218` only describe the shutdown checkpoint. A doc gap, not a contradicted sentence. | Add the steady-state checkpoint to design.md's sync-point section. Mechanical. |
| 1.9 | design.md orders `FS_IOC_GETVERSION` before `name_to_handle_at` in Populate; code does the opposite | WRONG | `docs/design.md:669-679` lists "`openat(O_PATH\|O_NOFOLLOW)`, `statx`, `name_to_handle_at`, the generation, the symlink target and every xattr", which is the code's order (`dcfs/backing.cc:567-640`, `ProbeChild`), and says the order does not matter ("whatever their order", pointing at `formal/README.md`, "The order of the handle and the generation"). design.md:715-719 does not say what Gemini quotes. | None. |
| 1.10 | design.md's `mount_dcfs_test` row describes tests the code and `mount_dcfs.sh` no longer have | CONFIRMED | `docs/design.md:2610`: "`none`, its old name, refused" (no test or code names `none`: `mount_dcfs.sh:363` says "step 15.9: was none"), and "`fsck.dcfs` (step 15.5: the flags and device reach a stand-in checker, its status is relayed and or'ed with the cache's ...)" while `dcfs/fsck.cc:397-413` prints "dcfs serves a directory: there is no device to check" and runs no backing checker, and `mount_dcfs.sh:820-905` has no `fsck.fakefs`. The `ro`/`dcfs.ro` part is accurate (`mount_dcfs.sh:429`, `dcfs/mount_dcfs.cc:260`). | Rewrite the row. Mechanical. |

### 2. README promises no test checks

| # | Gemini's claim | Verdict | Evidence | Fix and size |
|---|---|---|---|---|
| 2.1 | README promises idle NFS clients cause zero disk I/O; no test does it | PARTLY | The test gap is real and self-documented: `test/qemu/guest/idle.sh:41-42` "Not covered: an NFS client idling on an export", and `docs/plan/phases/10-benchmarks.md:20-24` lists it in the planned idle test. But the README makes no such promise: the quoted "(including when exported over NFS)" and the "Idle test" paragraph are not in `README.md` (`grep -n idle_short README.md` is empty; the README says only that disks spin up for contents, `:12-15`). `docs/design.md:1647` cites `idle_short_test` for the `FORGET` case only. | Either add the NFS-idle test (needs the Debian rootfs of `nfs_test`) or strike it from phase 10. Implementer. |
| 2.2 | README promises multi-device btrfs behaviour; no multi-device test | WRONG | No such text: `grep -in "multi-device\|degraded\|raid" README.md docs/design.md` finds nothing relevant. `README.md:547-557` is the NFS export section. `11.3c` is the "event-driven fault window", not a multi-device test. | None. |
| 2.3 | README says ZFS is refused with `ENOTTY`; the ZFS test never runs | CONFIRMED | `README.md:79-80` and `:1464-1465` say ZFS is refused. `dcfs/device_id_test.cc:180-190` `GetDeviceIdZfsIsUnimplemented` skips unless `DCFS_TEST_ZFS_PATH` is set, and that variable is set nowhere (`git grep DCFS_TEST_ZFS_PATH`: the test, design.md:2683, history). It asserts `kUnimplemented`, not `ENOTTY`. `docs/design.md:2682-2683` documents the gap, `docs/coverage.md` does not. The refusal path itself is tested through tmpfs and the NFS magic (`device_id_fault_test.cc:152`). | Delete the skipped test (a test-only knob) and list real ZFS in `docs/coverage.md`. Implementer. |
| 2.4 | README promises disk quotas (`EDQUOT`) are enforced; no test configures quotas | WRONG | `grep -in quota README.md docs/design.md` finds nothing. The quoted sentence is not in the README. Phase 18 (`docs/plan/phases/18-disk-quota-edquot.md`) is a planned phase, not a README promise. | None. |
| 2.5 | README promises `st_blocks` is 0 after `close()` until refresh; no test | PARTLY | No test characterizes it (`git grep` for blocks in `test/qemu/guest` and `dcfs/*_test.cc`: only the fallocate punch-hole checks, `write.sh:430-451`). But `README.md:1425-1427` says only that `st_blocks` "can lag behind delayed allocation until the file's attributes are next refreshed": a "can", dependent on writeback timing, which a test could not assert deterministically. Gemini's quoted wording is not the README's. | None recommended (a limitation, not a promise). |
| 2.6 | README's two RLIMIT_NOFILE WARNINGs are never produced by a test | PARTLY | `README.md:731` (correct line). "holding no descriptors on written files" is tested: `dir_cache_fs_test.cc:4383` `NoHeldDescriptorsIsAWarningAtStartup` asserts it once. "could not raise RLIMIT_NOFILE" is not: `RaiseFileLimit` (`dcfs/main.cc:236-269`, branches `:262-270`) is in an anonymous namespace, `RunDaemon` is run by no unit test, and no guest test lowers the hard limit (`git grep -n "ulimit\|prlimit\|nr_open" test`: empty). The functions `main_test.cc` supposedly tests (`MaxHeldWrittenFds`, `RaiseNofileLimit`) do not exist, and `docs/coverage.md:50` is about branch counters. | Test the failure branches (a `setrlimit` fault wrap, or a guest `prlimit`). Implementer. |

### 3. Stale references

| # | Gemini's claim | Verdict | Evidence | Fix and size |
|---|---|---|---|---|
| 3.1 | `xfstests.sh` and `mkxfstests_rootfs.py` still pass block devices to `mount.dcfs`, which now requires a directory | CONFIRMED | `test/qemu/guest/xfstests.sh:8-12` ("mount.dcfs, whose native mount of the device picks up ext4, xfs or btrfs"), `:50-51` `TEST_DEV=/dev/vdb`, `SCRATCH_DEV=/dev/vdc`, `:146-150` `local.config`; `mkxfstests_rootfs.py:279` `exec /bin/mount -t fuse.dcfs -o dcfs.cache_db=... "$@"` with the device as SOURCE; `dcfs/main.cc:322-325` opens SOURCE `O_DIRECTORY`, so `/dev/vdb` fails `ENOTDIR`. `SourceIsDirectory` does not exist (cited `mount_dcfs.cc:24-40` is the option-name table); the refusal is in `RunDaemon`. See "Urgent". | Mount the devices at a raw path in the guest and give that directory as SOURCE (and update `reset_scratch`, the cache names and the comments). Implementer. |
| 3.2 | Many references to the deleted native-type capture (`mount(8)`, `blkid`, `fsck.<type>`, `fsck.fakefs`, `dcfs.fstype`) | CONFIRMED | Stale: `dcfs/fsck.h:10` ("like the capture's /bin/mount"), `:63-64` (`backing_flags`: "the backing's fsck gets them"); `dcfs/fsck_test.cc:443-450` (FsckMain "runs findmnt, blkid and the backing's fsck"; `StandInPrograms`); `dcfs/syscalls_process.h:12-14` ("runs mount(8) in its capture helper"); `dcfs/mount_dcfs.h:157-169` and `mount_dcfs.cc:359` (`NativeMountError` "of a failed native mount(8)": its only caller is `umount_helper.cc:195`, for `umount`'s status); `test/qemu/guest/mount_dcfs_systemd.sh:433-438` ("runs fsck.ext4 on the device (blkid says the type)"; the checks below it assert "no cache database", rc 0); `test/qemu/README.md:2022-2026` and `docs/design.md:2610` (stand-in `fsck.fakefs`); `docs/plan/phases/21-real-hardware-checks.md:60,64,67` (`dcfs.fstype=...`, a removed option); also `bench/process.h:16` ("`mount.dcfs -o dcfs.fstype=bind,...`"), which Gemini missed. Wrong sub-claim: `docs/plan/README.md:51`, `mount_dcfs.cc:270-273` and `main.cc:301` do not cite "dropped" steps: 15.3 (instance identity) is still planned and 15.4 (stubs) is partly open (`phases/15-mount-dcfs-wrapper.md:323-327`). | Reword the comments, rename `NativeMountError`, delete the blkid/backing-fsck stand-ins in `fsck_test.cc`, fix the docs. Mechanical for comments and docs; implementer for the rename and test cleanup. |
| 3.3 | Stale references to removed flags `--fuse_opt`, `--source`, `--cache_db`, `--entry_timeout_sec`, `--sync_interval_sec` | PARTLY | Holds for `--source`, `--cache_db`, `--fuse_opt`: about 50 comment lines still name them as command-line flags (`dcfs/main.cc:115,170,207,335,337,428,440`, `mount_options.h:23,29,32`, `dir_cache_fs.h:53`, `mounts_below.h:14,24`, `backing.cc:527,2097`, `schema.sql:31,121`, `style.md:587,936`, `test/qemu/guest/cache_permissions.sh:20-358`, and more); `docs/design.md:2018` and `:2304` are correct history. False for `--entry_timeout_sec` and `--sync_interval_sec`: they are live Abseil flags (`dcfs/main.cc:77,79`, plus `--attr_timeout_sec` at `:75`), so design.md's uses are right. `lifecycle.sh:225` `--fuse_opt=` is the test helper's own vocabulary (`lib.sh:338-345` turns it into `dcfs.fuse_opt`), not a stale flag. | Replace the three names with `SOURCE`, `dcfs.cache_db`, `dcfs.fuse_opt` in comments. Mechanical. |
| 3.4 | A list of eleven stale names and comments | CONFIRMED (9 of the 11 sub-items; 2 are wrong) | Confirmed: `dcfs/schema.sql:2` "schema v4" vs `PRAGMA user_version = 6`; `dir_cache_fs.cc:2047` `backing::RecordAttrs` (now `WriteAttrs`, `backing.cc:385`); `backing.h:541-544` `StartupPurge` "forgets every non-source filesystem that is no longer mounted ... registers a mount fd" vs `backing.cc:2082-2123`, which purges every non-source row unconditionally; `dir_cache_fs.h:371,429,439` and `dir_cache_fs.cc:1984` say "WARNING" where `:517,610,1902,1906` log `LOG(ERROR)`; `escape.h:36-40` "They do not exist yet" (the mountinfo octal unescape exists, `mounts_below.h:37`); `test/qemu/guest/init:642` and `test/qemu/BUILD.bazel:885` name `test/qemu/scripts/mkrootfs-debian.sh`, which does not exist; `test/qemu/README.md:2238`, `guest/nfs.sh:361`, `third_party/linux/required_options.txt:70` name the deleted `build-kernel.sh`; `test/qemu/BUILD.bazel:262` "over a fake fhtest" (the test fakes `testutil`, `identity_oracle_test.sh:38`; `fhtest` still exists); `test/qemu/README.md:1778-1780,1788,1799-1800`, `create.sh:142,296`, `rename.sh:180` say `EXDEV`/`link-exdev`/`rename-exdev` where the checks are `link-into-boundary` (`create.sh:309`, "ot supported"), `rename-into-boundary` (`rename.sh:442`, `EOPNOTSUPP`) and `write-boundary-refused` (`write.sh:179`, ENOTSUP), and `mkdir` on the boundary gets `EEXIST`, not `EXDEV`. Wrong: `backing.cc:2093` mentions `CheckFilesystem` only to say it "is gone" (accurate history, though a "used to" comment); the README's `third_party/debian/scripts/mkrootfs.py` (`README.md:779-780`) is the real path (`third_party/debian/scripts/mkrootfs.py` exists). Not stale but unmet: `dcfs/version.h:7-8` "the wrapper phase will stamp it from git": decision 24 of phase 15 (`phases/15-mount-dcfs-wrapper.md:209-212`) specifies the stamp and it was never done (`kVersion` is the literal "0.1.0-dev"); Phase 15 is not done. | Fix each comment or doc line; decide whether to build the version stamp. Mechanical (version stamp: implementer). |

### 4. Dead code

| # | Gemini's claim | Verdict | Evidence | Fix and size |
|---|---|---|---|---|
| 4.1 | `names_random.sh` has an unreachable tail after `exit "$FAILED"` | CONFIRMED | `test/qemu/guest/names_random.sh:118` `exit "$FAILED"`; `:119` a lone `fi`; `:121-135` a copy of `:102-116`; `:137-140` the `dcfs-created-matches-reference` check. `sh -n` reports line 119. Present since cf6af89. See "Urgent". | Delete the tail and move the reference check above the `exit`, then run the guest test. Implementer. |
| 4.2 | `LogOpenFlags` has no production user | CONFIRMED | `git grep` for `LogOpenFlags\|log_open_flags` outside the library finds only `dcfs/BUILD.bazel:211-228` (its target and its own test) and plan notes. No target depends on `:log_open_flags`. | Delete the header, test and two BUILD targets. Mechanical. |
| 4.3 | `GetGeneration`, `UnlinkDentry`, `RenameDentry` have only test callers | PARTLY | `UnlinkDentry` (`metadata_cache.cc:1015`) and `RenameDentry` (`:1022`): only tests call them (`metadata_cache_test.cc`, and four setup calls in `dir_cache_fs_test.cc:1437,1748,1840,1860`), and the header says so (`metadata_cache.h:314-330`, "No production caller"). `GetGeneration` (`:371`) has a caller in the checking build: `dcfs/testonly/trace_recorder.cc:2066`. The prepared statements Gemini names (`sql_get_gen_` etc.) do not exist; these use `Query(ctx, "SELECT ...")`. | Delete `UnlinkDentry` and `RenameDentry` (replace the four setup calls with `SetNegative`/`PutDentry`-level calls); keep `GetGeneration` and say who uses it. Implementer. |
| 4.4 | `FsckArgs::backing_flags`, `CombineFsckStatus`, `kFsckReboot`, `kFsckCancelled` are dead | CONFIRMED (the last one is not) | `backing_flags` is filled at `dcfs/fsck.cc:266,269,271` and read nowhere (only `fsck_test.cc:124-158`). `RunFsck` starts `int status = kFsckOk;` (`:397`), never changes it, and calls `CombineFsckStatus(status, x)` at `:406,413`, so the function is the identity there. `kFsckReboot` is produced nowhere (`fsck.h:40`; only `fsck_test.cc:113`). Not dead: `kFsckCancelled` is returned by `RunProgram` (`fsck.cc:103`), though its one consumer (`FstabOptions`, `:117`) only tests for non-zero. | Drop `backing_flags`, `status`, the combining and `kFsckReboot`, with their tests. Implementer. |
| 4.5 | `HelperArgs::sloppy`, `no_mtab`, `verbose` are never read | CONFIRMED | `dcfs/mount_dcfs.h:103,105,106` are set at `mount_dcfs.cc:178,184,187` and read only in `mount_dcfs_test.cc:71-72`; `fake` (`mount_dcfs.h:104`) is read (`main.cc:673`). The parser must still accept `-s`, `-n`, `-v` (mount(8) passes them), so the cases stay and the fields go. | Make the three cases no-ops without fields; adjust the test. Implementer. |
| 4.6 | `CheckpointMode::kFull`/`kRestart` and the `db_ == nullptr` guard in `Checkpoint` are dead | PARTLY | Holds: `kFull` and `kRestart` (`dcfs/sqlite.h:273-274`) are referenced nowhere, tests included (callers use `kPassive`, `kTruncate`; `dir_cache_fs.cc:633`, `backing.cc:2348`, `sqlite_test.cc`). False: `Connection` is movable and default-constructible (`sqlite.h:216,297-299`), so a null `db_` is reachable in principle; but no production path or test reaches `if (db_ == nullptr)` at `sqlite.cc:334` (no test checkpoints a closed or moved-from connection), the same shape as the guard at `:315`. | Drop the two enumerators; delete or test the null guards. Mechanical. |
| 4.7 | The void-`parent` branch of `ForkSplit` is dead in production | CONFIRMED (citations wrong) | `dcfs/fork_split.h:50-55`: the `std::is_void_v` branch. The one production caller, `ForkDaemon` (`startup_channel.cc:130-135`), passes a parent returning `int`; the void parent is used only by `test/qemu/testdata/cov_fork_fixture.cc:52-57`. Gemini's paths are wrong: there is no `tools/coverage/`, and `umount_helper.cc` does not call `ForkSplit`. | Drop the void branch and give the fixture an `int` parent. Mechanical. |
| 4.8 | `only_daemon_of()` in `mount_dcfs_systemd.sh` has no callers | CONFIRMED | Defined at `test/qemu/guest/mount_dcfs_systemd.sh:858-866`; `git grep only_daemon_of` finds only the definition and its comment (`:856-857`). | Delete it. Mechanical. |

### 5. Duplicated logic

| # | Gemini's claim | Verdict | Evidence | Fix and size |
|---|---|---|---|---|
| 5.1 | Reading an fd to EOF is written five ways | CONFIRMED | `dcfs/mounts_below.cc:48-56` (`ReadAll`, 64 KiB, errors returned), `remount.cc:23-35` (`ReadMountinfo`, which also reopens `/proc/self/mountinfo`, as `mounts_below.cc` does), `fsck.cc:50-60` (`ReadAll`, 1 KiB, errors swallowed), `startup_channel.cc:38-46` (`ReadReport`, 4 KiB), `testonly/files.h:24-34` (`ReadFileToString`, 4 KiB). They differ in buffer size and in what a read error does. (The `DiscoverMountsBelow` loop Gemini cites does not exist; `mounts_below.cc:117,161` are two uses of the one `ReadAll`.) | One `ReadAll(fd)` and one read-file-to-string in the syscalls layer; decide fsck's error handling. Implementer. |
| 5.2 | `UmountAndWait` and `RunProgram` both fork, `execv` and decode `waitpid` | CONFIRMED | `dcfs/umount_helper.cc:173-199` and `dcfs/fsck.cc:66-103` each: `fork`, build `argv` from strings, `execv`, `_exit(127)`, then the same `waitpid` loop that retries on `EINTR` and wraps the error "while waiting for ...". (`ForkSplit` is not involved in either.) `startup_channel.cc:102` is a third `waitpid`. | One run-and-wait helper in `syscalls_process`. Implementer. |
| 5.3 | `DecodeAttr` and `GetStub` decode the same 16 `DCFS_ATTR_COLUMNS` | CONFIRMED | `dcfs/metadata_cache.cc:249-279` (`DecodeAttr`, `c + 0..15`) and `:309-326` (`GetStub`, columns 3-18 inline). They differ in `st_dev`/`st_ino`/`backing_ino` only. | Factor the stat/btime half into one function both call. Mechanical. |
| 5.4 | `ReadDirNames` and the test helper `ListDirectory` duplicate the `getdents64` loop | CONFIRMED | `dcfs/backing.cc:454-473` and `dcfs/testonly/files.h:38-58` are the same loop, skipping `.` and `..`. Low value: the testonly copy is a test oracle and may be better left independent of the code it checks. | Leave, unless russ wants one loop (then mechanical). |
| 5.5 | Mount-id extraction from `statx` is written twice | CONFIRMED | `dcfs/file_handle.cc:67-70` (`MountIdFromStatx`, exported in `file_handle.h:23`) and `dcfs/backing.cc:325-328` (`MountId`, with `kMountIdMask`, `:73`), same logic; `file_handle.cc:139` repeats the mask a third time. | `backing.cc` calls `MountIdFromStatx`. Mechanical. |
| 5.6 | `ErrnoOf(const absl::Status &)` is copied into five files | CONFIRMED (four, not five) | Four definitions: `dcfs/backing.cc:79`, `backing_test.cc:83`, `dir_cache_fs_test.cc:493`, `testonly/trace_recorder.cc:85`; `file_handle.cc` has none and `status_errno.h` does not exist (`StatusToErrno` is in `status.h:102`). One has already drifted: `trace_recorder.cc` returns 0 for OK and -1 when no errno is attached, the others return 0 for both. | One `ErrnoOf` (or `StatusToErrno`) in `status.h`; decide the no-errno value. Mechanical. |
| 5.7 | `normalize_stat()` is copied into five guest scripts | CONFIRMED | `readonly.sh:102`, `create.sh:106`, `rename.sh:150`, `write.sh:122`, `nfs.sh:206`; the bodies are identical except for two comments (md5 equal in two groups, `diff` shows only `:18-19`). | Move into `guest/lib.sh`; rerun the five guest tests. Mechanical. |

## Counts and precision

| Category | Findings | CONFIRMED | PARTLY | WRONG |
|---|---:|---:|---:|---:|
| 1. Docs contradicted by code | 10 | 4 | 5 | 1 |
| 2. README promises without tests | 6 | 1 | 3 | 2 |
| 3. Stale references | 4 | 3 | 1 | 0 |
| 4. Dead code | 8 | 6 | 2 | 0 |
| 5. Duplicated logic | 7 | 7 | 0 | 0 |
| **Total** | **35** | **21** | **11** | **3** |

Measured precision: strict (CONFIRMED only) 21 of 35, 60%; lenient (CONFIRMED
plus PARTLY, i.e. something real behind the finding) 32 of 35, 91%. The
categories were uneven: dead code and duplication (5, 4) were mostly right,
while the README-promise category (2) was 2 wrong and 3 partly right out of 6,
with README quotes that are not in the README. Findings of consequence
(3.1 and 4.1) are confirmed and were not ranked above the doc nits by Gemini,
which marked all 35 `[certain]`.

## Confirmed findings by fix size

**Mechanical (docs, comments, pattern deletions).** 1.1, 1.2, 1.3, 1.10, the
doc and comment parts of 1.4, 1.5, 1.6, 1.8, 3.2, 3.3 and 3.4; 4.2, 4.6 (the
enumerators), 4.7, 4.8; 5.3, 5.5, 5.6, 5.7; 5.4 only if russ wants it.

**Implementer (code and tests, no protocol).** 3.1 (urgent), 4.1 (urgent),
2.3, 2.6, 2.1 (or strike it from phase 10), 1.7, the rename and test cleanup
in 3.2, the version stamp in 3.4, 4.3, 4.4, 4.5, 5.1, 5.2.

**Protocol.** None.

## Incidental, outside the 35

`dcfs/backing.cc:2063-2075` (`GetGroups`): a `while (true)` that re-asks when
`getgroups` reports `EINVAL`, with the comment "(another thread cannot change
ours, but be exact anyway)": a retry loop and a 1.10a phrase, for russ to
rule on (style.md 1.12). The `StartupPurge` comment and body carry "used to"
history too (3.4).

## The Flash experiment: one broad job against five narrow ones (2026-10-17)

process.md rule 5's measurement. Six Gemini 3.8 Flash (medium) jobs, read-only,
on the same tree (c645222): one prompt with the five questions above, and
five prompts with one question each. A Flash (high) job then matched every
finding to this note's key; the orchestrator spot-checked the matches (they
name the same defect). The key has 34 CONFIRMED or PARTLY findings.

| Shape | Findings | Matched a confirmed key row | Distinct key rows found (recall) | Matched a WRONG row | Not in the key |
|---|---|---|---|---|---|
| One broad job | 20 | 5 | 4 of 34 (12%) | 0 | 15 |
| Five narrow jobs | 37 | 14 | 11 of 34 (32%) | 0 | 23 |

Narrow jobs found almost three times as many of the known defects, and
neither shape repeated a finding the key had shown wrong. Both are far
below Argon's single broad sweep (the key itself). The 38 findings not in
the key are unverified: some are real (the fsck rebuild promise, the 0400
database "tightened" to 0600, which led to 26.38), so precision is a lower
bound. Conclusion: for Flash, narrow jobs, as rule 5 already prefers;
Argon for a broad sweep when its quota allows.

