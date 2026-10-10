# dcfs execution log

Started 2026-09-27 in a session scratchpad; moved into the repository on
2026-10-05. Newest entries at the bottom.

## Needs russ
- (done by russ 14:40) deleteme/ + swap files removed; mmdebstrap installed; russ added to kvm (use `sg kvm -c` in this session).
- libfuse: DONE, 08e3eec1 on master (rebased on upstream e001ea32, bit 44, 7.47, minor 46); tag pre-rebase-2026-09-27.
  fuse-generation-qemu suite NOT re-run yet (would rebuild its kernel, ~30 min); russ to decide.
- Host kernel 6.8 cannot run the daemon at all (ProbeRoot needs FS_IOC_GETFSUUID). End-to-end acceptance moves to QEMU
  (readonly_test). Ask russ whether they want a dev-only identity override for the host, or a newer host kernel.

## Done
- dfs: committed 9b6fefb "ublk: drop GetControlDevice"; moved to ~/Sources/archive/dfs
- dircachefs: .git seeded (submodule clones dropped); branches: main (tip), old/main, old/using-libfuse
- ccf80f8 snapshot commit; 811644e cleanup commit (also dropped the /usr repo rule from MODULE.bazel)

- 74092ef 0.1 README merged (sonnet; 2 nits fixed by orchestrator: em-dashes, 'no templates')

- 24f87c7 0.2 Bazel refresh merged (sonnet; deps at plan versions; rules_cc load added for Bazel 9; ubsan linkopts trimmed by orchestrator)

- 11391c0+36e5fa9 0.3 libfuse 3.18.2 via BCR + patch (bit 44, protocol 7.47) merged; --dynamic_mode=off; -Werror off for external/

- e8aeb37 0.5 tree compiles; main mounts/ls/unmounts (sonnet). dir_cache_fs is a root-only skeleton; file_handle/fs_db/mount
  targets out of the build. Phase 0 complete.

- 93743e6 3.4a tools/fhtest merged (haiku)

- 029309d 1.1 Abseil status macros/StatusBuilder/RET_CHECK/StatusToErrno merged (sonnet; found+fixed missing EOPNOTSUPP)

- bdfd61c 1.2 syscalls/fd merged (haiku; one review round: xattr ERANGE race, readlinkat cap, Close idempotence)

- 15f2ac9 1.3 sqlite wrapper merged (sonnet; one review round: unwind on failed COMMIT)

- 9b95ee0 3.1 fuse_ops table + FuseRequest reply helpers merged (sonnet; readdirplus caps withheld until 3.2 implements it)

- 2.1 schema v1 + Migrate/meta helpers/MintFuseGeneration merged (sonnet; review: SQL splitter -> Connection::ExecScript)

- ff5b538 1.4 FileHandle value type merged (sonnet; review: device from parent dir for non-boundary entries; 47 errno names added incl. EIO/ELOOP)

- 2.2 MetadataCache merged (opus; clean first pass; orchestrator fixed zero-arg BindAll in sqlite.h). Known: invalidating a
  directory orphans its children's inode rows (no dentries); GC is a later concern.

- 5.1 QEMU infra merged (sonnet). Kernel built in 28 min at ~/.cache/dcfs/kernel-build (7.3.0-rc4 + generation patch,
  btrfs/xfs/nfsd/fuse-io-uring on). main_static links fully static. Orchestrator botched the BUILD.bazel rebase
  conflict once (masked by `| tail`), repaired in 2eb6309. Lesson: never chain bazel through tail with &&.
- 3bf24f0 2.3 backing layer/PopulateDirectory/LookupOrPopulate/StartupPurge merged (opus). Host tests skip the
  non-root parts (open_by_handle_at needs CAP_DAC_READ_SEARCH); real coverage in QEMU. OpenNode returns ESTALE
  (plan said EIO) on a replaced object. Known risk: unprivileged daemon sees gen 0->N as recycle after chmod.

- 3.2 read-only ops merged (sonnet; 5 QEMU iterations). //test/qemu:readonly_test PASSES under KVM: warm metadata =
  0 sectors read on vdb AND the vdc submount, listing/st_ino match backing, restart keeps the cache. Fixes on the way:
  busybox has no mountpoint/find -ls; open_by_handle_at rejects O_PATH mount fds (mount fds now O_RDONLY dirs, plan
  amendment 7); boundary dir reopened via /proc/self/fd.

- fuse-generation-qemu suite: ALL 11 TESTS PASS with linux 320a1ed162d7 + libfuse 08e3eec1 (KVM, 20 min incl. kernel
  rebuild). NOTE for russ: its Makefile does not rebuild kernel/libfuse when sources change (kernel rule has no deps;
  libfuse rule keys on build.ninja mtime); agent used `make clean` first. Not modified.

- 742a29b 3.3 FUSE passthrough reads merged (sonnet, clean first pass). //test/qemu:passthrough_test PASSES under KVM:
  content reads hit the disk, metadata stays at 0 sectors, daemon CPU ~0 over 64 MiB (kernel passthrough active),
  write opens EROFS (until Phase 4), no fd leak over 200 opens, restart OK.

- 3.5 CLI/lifecycle/systemd unit merged (sonnet): --fuse_opt, actionable foreign-db error, ordered shutdown with WAL
  checkpoint, exit-code fix (positive fuse_session_loop return = signal = clean), packaging/dcfs.service.
- 3.4b NFS handle tests as guest test handles_test (sonnet): merged pending its QEMU run. Two checks correctly SKIP
  until Phase 4 (fstat EIO on an open fd; seeing a recycled name) because exclusive access means out-of-band backing
  changes are invisible by design.

- 64e0fbb 5.1b merged: ALL tests run in QEMU guests (microvm+qboot+PVH, minimal 11 MB kernel, KVM), ~2-3 s per
  unit test, ~1 s kernel->init, full suite ~40 s; host device-id seam deleted; asan works (dynamic initramfs).
  17/19 green on merge; the 2 failures are the daemon bugs / test bug above, not infra. ubsan broken by an
  abseil+gcc constexpr issue (pre-existing, unfixed).

- b636371 3.6 merged (sonnet): (1) all syscall errors now dcfs::ErrnoToStatus (payload) -- absl::ErrnoToStatus in the
  ioctl template broke ReadGeneration on tmpfs; (2) --fuse_opt=max_read=N: libfuse never sets conn.max_read itself,
  Init() must copy it; (3) RequireAttr(): every op maps a missing row to ESTALE (VFS retries opens once after ESTALE).
  Full suite 19/19 green incl. handles_test and lifecycle_test. tmpfs DOES support FS_IOC_GETFSUUID on 7.3.

- 8be5799 4.1 Setattr merged (sonnet). setattr_test 37/37 in QEMU. Review round: busybox truncate opens O_WRONLY (EROFS
  until 4.4) so the test never sent SETATTR(size); added tools/testutil (static guest helper: truncate/utimens/lchmod).

- 75ec504 4.2 create family merged (sonnet): mkdir/mknod/symlink/link/create write-through; ProbeObject shared with
  population; fresh dirs complete by construction; parent completeness restored after single-dentry ops; writable
  Create refreshes attrs on Release. create_test 24/24; full suite 21/21. Also fixed the serial-verdict drain flake.

- 33102f0 4.3 unlink/rmdir/rename merged (opus, clean first pass): row lifetime (delete at nlink 0 with no open
  files, else on last Release), ReresolveAfterFailure so failed ops do not leave names unknown, both parents'
  completeness restored. rename_test 46/46; suite 22/22. Found: 2 concurrent opens of one file -> 2nd gets EIO (kernel
  allows one passthrough backing file per inode) -> fixed in 4.4.

- 6a473b1 4.4 merged (sonnet, clean): writes allowed (passthrough), one O_RDWR backing file per inode shared by all
  opens (fixes concurrent-open EIO), ATOMIC_O_TRUNC unset so O_TRUNC goes through SETATTR, Flush/Fsync refresh attrs,
  fallocate, set/removexattr write-through (ForgetXattr narrow phase 1), 4.2 failure paths re-resolve. write_test 61
  checks; suite 23/23. Coverage gap noted: no unit tests for the new backing helpers (e2e only).

## In flight
- 4.5 pjdfstest conformance in the guest (sonnet, wt step-4.5)
- 5.3 Debian rootfs + NFS test (sonnet, wt step-5.3): rootfs build WORKS unprivileged (~40 s; needed a fresh Debian
  archive keyring, world-readable keyring path for mmdebstrap's inner sandbox, unshare --map-user+--map-users with
  subuids, excluding /dev nodes); runner --rootfs + init chroot work; dcfs/nfsd/mountd start in the guest; BLOCKER:
  NFSv4 loopback mount hangs (kernel client is v4.0-only). Agent continuing (nfsd fs mount / rpc_pipefs / rpcbind leads).
- proto47: kernel/libfuse/dircachefs minor 46->47, v2 patch prepared (not sent), test kernel rebuild, fgq suite rerun (sonnet)
- QUEUED after 4.5 lands: out-of-band change detection + tests; then README/design.md rewrite
- 4.6 robustness (opus, wt step-4.6): writable-open marks attrs unknown (crash gap) + free out-of-band detection + tests
- 20:30 usage limit hit with 4 agents running (4.5, 4.6, proto47, 5.3); all four cut off. Session switched to Opus 5.5.
  Resumed 2 at a time: proto47 (steps 1-4 done: linux d530980dfbb7 minor 47, tag fuse-generation-v1; libfuse
  8b0f9246; v2 patch ready, checkpatch clean; dircachefs 7841afb) and 4.6 (restart from scratch).
  PARKED (uncommitted WIP in worktrees): 4.5 pjdfstest, 5.3 NFS (note: 5.3 edited build-kernel.sh for a "private
  kernel build"; review before merging).
- 4.6 (opus) 37cdcb1: writable opens keep attrs UNKNOWN until last writable release (crash gap fixed; crash_test proven
  to fail on the pre-fix daemon via tools/testutil writehold); free out-of-band detection in VerifyBackingIdentity and
  PopulateDirectory (widened statx mask, no new syscalls) with WARNING + adopt attrs + dir incomplete/negatives dropped
  + xattrs unknown on ctime change; stderr threshold now WARNING. 24/24 green. Kernel caches NOT invalidated on
  detection (no notify_inval). Fixing a Release refcount leak before merge.
- 2a4f672 4.6 merged (+ Release never leaks the backing file on refresh failure; unlinked-row deletion requires valid
  attrs). Resumed 4.5 (rebase onto main first). 5.3 still parked.
- proto47 DONE: linux d530980dfbb7 (minor 47, tag fuse-generation-v1 = v1), libfuse 8b0f9246 (tag pre-minor47),
  dircachefs carried patch merged. v2 patch ready in scratchpad/v2 (checkpatch clean). fuse-generation-qemu suite
  12/12 PASS on the v2 kernel; dcfs handles/readonly pass. Test kernel ~/.cache/dcfs/kernel-build now = v2.
- russ sent the v2 patch (2026-09-28). Overnight: finish 4.5, 5.3, then docs, then 5.2.
- russ: accept kernel-cache staleness on out-of-band changes (no notify thread); plan amendment 11; docs must say so.
- 01:30 usage limit hit again (5.3 and 4.5 cut off); both resumed after reset. Keep to 2 agents.
- Audits launched (read-only, reports in scratchpad): audit-races.md (opus), audit-crash.md (opus), audit-style.md (sonnet). Triage into fix steps when they land.
- 111502a 5.3 NFS merged: real NFSv4 export of dcfs in a Debian chroot guest; held-open file survives a dcfs restart;
  DB wipe -> ESTALE. Root cause of the hang: no /etc/mtab in the apt-variant rootfs (rpc.mountd segfaults). Shared
  kernel gains NFSD_LEGACY_CLIENT_TRACKING. FOR DOCS/RUSS: (1) after restarting dcfs under nfsd, `exportfs -f` is
  required (nfsd pins the old vfsmount) -> operational note; (2) GNU find refuses to descend across submounts over NFS
  because st_ino = backing ino collides (every ext4 root is ino 2) under one NFS device -> real user-visible limitation.
- 4.5 pjdfstest done (sonnet): 8826 checks; dcfs 178 fail vs ext4 43; fixed ENAMETOOLONG-as-ENOENT and double umask;
  154 dcfs-specific failures ALL from one cause: no caller-credential switching (files created by users become
  root-owned on the backing fs!). Needs step 4.7 (setfsuid/setfsgid + supplementary groups per request). Rebasing 4.5.
- Audits in: crash (F1 power-loss ordering, F2 generation reissue after power loss, F3 parent not marked unknown on
  create, F4 root identity not checked, F5/F6 gen-0 identity, F7 no DB lock, F8-10 low), style (lib.sh dedup, dead
  code, clang-format pointer alignment mismatch, stale comment). Race audit still running.
- russ decision: refuse submounts (amendment 12); st_ino question moot. Step 4.8 'no submounts' after 4.5 rebase; then 4.7 creds.
- Race audit in (10 findings; io_uring can go multi-threaded; mmap-after-close writes; ACL staleness; setxattr caches sent value; errors after successful syscall; readdir dup via rowid; +overlaps). Proposed steps 4.8 submounts, 4.7 creds, 4.9 mechanical fixes, 4.10 power loss. Awaiting russ: power-loss approach, mmap-after-close policy.
- 4.10 durability (dirty set + random gen) launched; 4.5 rebase resumed
- 03aa81b 4.5 pjdfstest merged (26/26 suite). 4.8 no-submounts launched.
- 4.10 done (opus): random gen + schema v2; durable dirty set (Transaction kSync = PRAGMA synchronous FULL around the
  commit; fast path when already dirty), SyncBacking on fsync/fsyncdir/shutdown/every 5 s at next request; StartRun
  recovery; FinishRun; identity by handle bytes + btime; F8. power_test (simulated lost backing mutations; control
  run fails without recovery). Cost ~3.3 ms/create with fast path vs 9.4 ms if every phase 1 flushed (virtual disk).
  Leftovers for 4.9: F3 parent-unknown on create, F4, F7, F9, F10 (.. resolution after recovery). Rebasing onto 03aa81b.
- 07:40 limit reset. Resumed 4.10 (+ typed cache_state table) and 4.8; launched tri-state audit (opus). Queued: xattr tri-state fix after 4.10 merges.
- 4.8 done (mountinfo startup check, runtime EXDEV, tests converted, 26/26 + pjdfstest) but in-memory refusal -> false negative after restart; sent back to persist refusals as dentries (tri-state rule). Merge order: 4.8 then 4.10 (rebase again).
- Tri-state audit in: F1 unguarded fills (coroutine), F2 setxattr caches sent value, F3 side-effect xattr changes, F4 restore-complete lost update, F5 create parent attrs, F6 ParentOf unknown->ENOENT, F7 coarse flags, F8 SET NULL trigger. Plan: 4.11 xattr tri-state+F2+F3; 4.12 epochs+CAS+F5; 4.13 dentry state+trigger+.. fallback; then 4.9, 4.7.
- f6c4f31 4.10 merged (27/27 incl pjdfstest; typed cache_state, test shown failing first). Killed 10 orphaned nfs_test wait loops from the 5.3 agent.
- 12:40 limit reset; resumed 4.8 (commit + rebase) and 4.11 (restart). Coverage audit parked until one finishes.
- Coverage audit: 26/31 covered, 4 missing (xattr ERANGE race, readlinkat cap, readdir buffer boundary, Release leak), 2 weak; BeginCreate parent-unknown unfixed (-> 4.12). 4.14 test backfill launched.
- a0c6922 4.8 merged (28/28 incl pjdfstest; refusals persisted as dentries, test shown failing first).
- 4.11 xattr tri-state done (F2, F3 test-first; cap-after-chown already handled by kernel REMOVEXATTR; F1 left for 4.12). Rebasing onto a0c6922.
- a20be4c 4.11 merged (28/28). 4.12 epochs/CAS/create-parent handed to the 4.11 agent (has the F1 context).
- 14:40 limit hit; 4.12 (just started) and 4.14 (mid-backfill, was on a scratch revert branch) cut off; resume both at 17:40.
- 7b8ce46 4.12 merged (F5 parent unknown, F4 CAS via directories.epoch, F1 in-memory fill guards; 28/28). 4.13 to same agent.
- 4.14 done (6 tests, revert proofs; readdir fix is perf-only -> timing test). Sent back: syscalls.h testonly hooks violate the test-code-outside-production rule -> link-time --wrap fakes.
- 1c18d1e 4.13 merged (dentry states, delete trigger, single-name reresolve, .. from backing; crash.sh dir-handle reconnect). 4.9 to same agent; 4.14 must rebase onto it.
- limit hit again (resets 06:00); 4.9 and 4.14 cut off; resume both then.
- bc0fdce 4.14 merged (6 backfill tests, link-time --wrap fakes, no production change; 32/32). 4.9 must rebase.
- f032c4e 4.9 merged (8 items, test-first, 32/32). 4.7 caller credentials launched.
- 6a60df6 4.7 merged: caller credentials (AsCaller), pjdfstest dcfs 28 = ext4 28, dcfs_specific 0, baseline empty. Found: 4.5's 'ext4' run was actually tmpfs; chown(-1,-1) ctime hidden by dcfs -> fixed. 33/33.
- 1678205 6.1 style cleanup merged (11 commits, no behaviour change, 33/33). 6.2 docs launched.
- 446aac1 6.2 docs merged. Doc review found: POSIX ACLs not enforced (no FUSE_CAP_POSIX_ACL; security), only ext4 tested, WAL result ignored, F10 ESTALE for removed in-use objects, stale metadata_cache.h comments, DotStat ino, stale CI yml, systemd unit lacks allow_other/umount. 6.3 follow-ups launched.
- 8b3aa82 6.3 merged (POSIX ACLs enforced via FUSE_CAP_POSIX_ACL+DONT_MASK, F10 lookup counting with in-memory removed records, WAL check, dot inodes, comments, systemd unit, CI build-only; 34/34). 5.2 launched.
- 51c37dc 5.2 merged: e2e matrix ext4/xfs/btrfs, 70/70, pjdfstest dcfs-specific 0 on all; btrfs FS_IOC_GETFSUUID absent -> BTRFS_IOC_FS_INFO (amendment 20). Flake noted: readdir_boundary_test dot-inodes 1/3.
- e5df2a7 7.1 merged: flake = test bug; ASan 70/70 incl e2e; UBSan blocked by GCC+Abseil constexpr; plain 70/70 (28 min). PLAN COMPLETE.
- Prepared Phase 8 (connected open) and Phase 9 (identity from backing fs) work plans in the plan file; awaiting russ's go-ahead and Phase 9 decisions.
- Plan updated: Phase 8 tests revised (property, not watchers), Phase 9 decisions, Phase 10 drop kernel patch, Phase 11 multi-instance submounts (discussion).

## 2026-10-04/05 planning (no code changes)
- Long planning discussion with russ: submounts and btrfs subvolumes as
  separate instances behind a `mount.dcfs` wrapper, fstab with and without
  systemd, dropping the kernel patch, identity from the backing
  filesystem, connected fds, pinned toolchain and tools, CI, test tiers,
  coverage, benchmarks, crash/stress/failure testing, filename tests.
- Found: the cache database is world-readable (mode 0644 with umask 0):
  now Phase 2.
- Plan moved from ~/.claude/plans into docs/plan/, split into one file
  per phase, renumbered in execution order; this log moved here.
- Added Phase 12 (TLA+ model with TLC, trace validation, a KLEE trial on
  one function for russ to evaluate); later phases renumbered 13-20.
  Audits moved to docs/plan/audits/; decisions.md, notes/ and standing
  instructions in process.md added so the work can be resumed by others.
- KLEE trial dropped from Phase 12 (russ): KLEE supports only LLVM 16 (partially 17-19).
- Added the idle test (Phase 10), Phase 21 real-hardware checks with a trial point after Phase 11, and rewrote process.md for the new phases (original waves moved to history.md).
- Plan consistency review (read-only subagent): fixed 10 findings (stale old-plan step numbers, ordering lines, Phase 2 wording, soak 'last', cache checker moved from Phase 11 into Phase 8 where the failure sweeps first need it); added FUSEX as future direction; server is x86_64 and gets kernel >= 6.9 before the hardware checks.
- Wrote execution.md: 13 waves of at most two lanes, models per lane, sync points S1-S13, russ checkpoints.
- LICENSE (MIT) added from the downloaded text; plan: Bazel fetches and builds all third-party dependencies (kernel, QEMU, busybox, Debian via rules_distroless, pandoc, TLC); CI is GitHub Actions.
- Phase 4: QEMU built as minimal as possible (no default features or devices; microvm, virtio-mmio/blk, serial, RTC only; raw images; qboot; dependency list justified in its README).
- russ reviewed AGENTS.md (LGTM): Phase 1 done. Plan, AGENTS.md and LICENSE committed at russ's request.
- 2026-10-05: russ said go. Wave 1 dispatched: lane 1 Phase 2 (Sonnet, worktree step-2), lane 2 Phase 3a Bazel-built stock kernel (Sonnet, worktree step-3a).
- Bazel: removed 48 orphaned output bases (15 GB, from removed worktrees; russ OK); disk cache capped at 50 GB (--experimental_disk_cache_gc_max_size); try-import user.bazelrc; from wave 2 steps run in two long-lived lane clones (~/Sources/dircachefs-lanes/lane-{1,2}) instead of a checkout per step.
- Agent types with model and effort (.claude/agents/dcfs-{mechanical,implementer,investigator,protocol,reviewer}) and CLAUDE.md (imports AGENTS.md; Claude-specific notes). They take effect after Claude Code restarts. Wave 1 agents run as general-purpose (default effort).
- russ moved to Claude Max: lane limit is now the machine (4 CPUs, 11 GB): 2 building lanes + 2 non-building lanes, per-lane caps in user.bazelrc; applies after the restart.
- Phase 2 merged (519b7dc, bf3d78d; Sonnet, two review rounds): database and -wal/-shm 0600,
  missing cache directory created 0700, warning for a group/world-accessible directory, existing
  wider-mode files tightened, symlinks (O_NOFOLLOW) and files not owned by root refused. Test first
  each round. Agent ran full suite 71/71 and ASan before the second round; unit + cache_permissions
  plain and ASan 18/18 after. Accepted residual risks (both need an unsafe cache directory, which
  dcfs warns about): SQLite opens -wal/-shm by path after dcfs's check (swap window), and a hard
  link to a root-owned file passes the owner check (relies on fs.protected_hardlinks=1).
- Wave 1 lane 1 freed: started Phase 4a (Bazel-built minimal QEMU + busybox, build and smoke tests only, no harness wiring until 3a merges) in lane-1 clone (Sonnet, general-purpose: agent types need a restart).
- S1 attempt 1 invalid: the step-3a worktree's Bazel server had been started by an orchestrator
  `bazel help` without the kvm group, so every test ran under TCG. Observed under TCG: the stock
  kernel's boot_test timed out at 900 s and release_leak_test at 1200 s (data for Phase 5.1, which
  must make TCG usable). Rerunning S1 with a KVM server. Rule added to CLAUDE.md.
- Phase 4a progress: busybox 1.38.0 committed on step-4a (busybox `find` never had `-ls`; the plan's
  premise was wrong). QEMU 11.1.2: deps glib + zlib only (no pixman); microvm needs FDT (internal
  dtc); `-device help` = virtio-blk-device, isa-serial, mc146818rtc, virtio-serial-device (upstream
  quirk). glib needed a pkg-config shim for rules_foreign_cc; build in progress.
- S1 passed (2026-10-05): stock-kernel boot_test passes; full suite 71/71 with KVM (default patched
  kernel). Phase 3a merged (d08136f, d497d24): linux 7.2.9 fetched and built by Bazel from tinyconfig +
  third_party/linux/kernel.config (fragment covers later phases too), 3.3 MiB bzImage, ~9 min cold
  build; GNU bc Bazel-built; flex/bison/host C toolchain still from the host (BCR flex/bison fail at
  runtime under Bazel: runfiles/m4); selectable with --//test/qemu:kernel=stock. Orchestrator fix:
  build scripts now delete their temp build directories.
- Wave 2 lane 1: Phase 3b (drop the kernel patch, stock kernel default) dispatched in lane-2 (Sonnet, general-purpose). Phase 4a continues in lane-1.
- Phase 4a merged (24450f1..8a65668; Sonnet, 3 review rounds). busybox 1.38.0 static, 55 applets.
  QEMU 11.1.2: --without-default-features/--without-default-devices, microvm + virtio-mmio/blk +
  isa-serial + RTC (plus upstream's unconditional virtio-serial-device), raw/file block drivers
  only, qboot from the tarball; glib/gmodule/pcre2/zlib linked statically through a pkg-config shim
  (PKG_CONFIG_LIBDIR), so NEEDED is only libc/libm (orchestrator caught host libz and an absolute
  pcre2 path into a Bazel output base in round 2); custom repository rule re-adds subprojects/dtc
  that Bazel's tar extraction drops. Full suite 73/73. Not yet wired into the harness. TCG boot
  with it showed no console output in 600 s under heavy load (inconclusive; Phase 5.1).
- Phase 4b merged (Sonnet, 2 rounds): Debian bookworm image built by rules_distroless from
  snapshot 20261004T203145Z (20 explicit packages incl. systemd, util-linux mount, xfs/btrfs/quota
  tools; 136 total; per-package sha256 in MODULE.bazel.lock), assembled with host mke2fs/tar;
  mkrootfs-debian.sh deleted; nfs_test passes from it; full suite 74/74. Known gaps: files owned by
  the build uid (host mke2fs 1.47.0 has no tarball input; needs Bazel-built e2fsprogs >= 1.47.1 for
  the systemd guest, noted in Phase 15.6); mke2fs and tar still host tools.
- Phase 4c dispatched in lane-1: Bazel-built static e2fsprogs (>= 1.47.1); Debian image from the root-owned @debian//:flat tar, no host mke2fs.
- Phase 3b merged (Sonnet): kernel patch and patched libfuse dropped; stock BCR libfuse 3.18.2;
  stock kernel 7.2.9 is the default for e2e and unit tests (qemu_cc_test.bzl now honours the
  flag); libfuse_version_test deleted (it only asserted the patch). Test-first note: the stock
  kernel alone did not fail on unchanged code (the patched libfuse falls back to fuse_reply_attr
  when the kernel lacks the capability); building against stock libfuse is the real check. Agent:
  full suite 70/70 and ASan 70/70 (twice each) before rebase; orchestrator after rebase onto
  4a/4b: build + 9 representative tests (incl. nfs_test, cache_permissions, smoke tests) pass.
- Needs attention (Phase 6.2, first item): rename_test_xfs fails under heavy host memory pressure
  (10/10 forced reruns, a different zero-backing-reads check each time: unlink, rmdir, rename
  replace), never on ext4/btrfs and never on a quiet host. Likely xfs's deferred inode
  inactivation (inodegc) reading AG metadata inside the measured window after an unlink/replace,
  i.e. a timing-sensitive test rather than a cache miss. Fix the test to quiesce xfs before
  measuring (e.g. syncfs + wait for inodegc) and confirm under load.
- Phase 4d dispatched in lane-2: wire Bazel-built QEMU/qboot/busybox into the harness; remove the patched-kernel flag value, build-kernel.sh and the kernel.bzl repository rule.
- Phase 4c merged (Sonnet): e2fsprogs 1.47.4 (static mke2fs/debugfs, with a minimal private static
  libarchive 3.8.1 built inside third_party/e2fsprogs; upstream bug: debugfs.static omits
  libarchive, patched), the Debian image built with `mke2fs -d <tarball>` from the root-owned
  @debian//:flat tar (missing ancestor directories synthesized), ownership_test (root:root,
  /bin/mount setuid) failed first then passes; nfs_test passes; full suite 70/70 (agent). Host mke2fs
  no longer used. Also: the repo's global -Werror broke autoconf probes inside rules_foreign_cc
  builds (fixed per build with CFLAGS=-Wno-error); revisit when the pinned toolchain lands (Phase 7).
- lane-1: rename_test_xfs flake investigation dispatched (Sonnet; evidence first; fix the test only if the reads are xfs's own deferred work, and prove the check still catches a backing read).
- rename_test_xfs flake fixed (ded4642; Sonnet): root cause is xfs's deferred inode inactivation
  (inodegc), drained only by freeze/unmount (xfs_inodegc_stop), not by sync/syncfs; its AG metadata
  reads landed in check_cold's measurement window under host load. Fix: check_cold freezes and
  thaws the backing fs (FIFREEZE/FITHAW) before taking its baseline. Before: 8/10 failures under
  load; after: 20/20 passes; an injected backing read inside the window is still caught.
  Follow-up (Phase 6.2): other guest scripts with zero-backing-reads checks after unlinks on xfs
  may share the latent flake; move quiesce_backing() into lib.sh and use it in every such check.
- Plan: Phase 5 develops the CI workflow locally with pinned nektos/act before publishing; a full suite under act doubles as a host-dependency detector (russ). Needs russ: Docker access (not in the docker group).
- russ added russ to the docker group (use sg docker). Docker on this machine is production: never touch containers/images/volumes/networks we did not create, no prune (CLAUDE.md, Phase 5).
- russ: sanitizer suites no longer block steps (run in the background after merge; required green at sync points; failures become the next step). New side track Phase 6.3: why ASan is so slow (hypothesis: --config=asan instruments the Bazel-built QEMU/glib/e2fsprogs/busybox via rules_foreign_cc), started in lane-1.
- Phase 4d merged (67ac1c0..deecc69; Sonnet, ~4.6 h incl. long suite runs): every test boots the
  Bazel-built QEMU 11.1.2 + qboot and the Bazel-built busybox (run-qemu.sh refuses host paths and logs
  the binaries); the patched-kernel path, build-kernel.sh, test/qemu/kernel.bzl and the //test/qemu:kernel
  flag are gone. Found and fixed 15 busybox config gaps the pinned busybox exposed; the worst: no
  md5sum, so content checks compared two empty strings and passed silently, and `ls` never sorted.
  Full suite 75/75; ASan on 3 representative targets. Remaining host tools: KVM, mkfs.ext4/btrfs/xfs
  for scratch disks (follow-up: Bazel-built xfsprogs/btrfs-progs; mke2fs exists), network once per pin.
  pjdfstest hit run-qemu.sh's 1200 s timeout under contention (passes alone in ~550-650 s): Phase 5.1.
- S2 plain suite: 75/75 on main 3186e2e (cache hits: identical inputs to lane-2's verified run). S2 ASan suite pending the Phase 6.3 finding.
- Phase 6.3 findings (Sonnet): confirmed --config=asan instrumented third-party builds (QEMU and
  mke2fs/debugfs linked libasan; glib recompiled with -fsanitize). Fix (pending merge): cancel the
  sanitizer for third-party builds only (QEMU/e2fsprogs/libarchive configure flags, pjdfstest copts,
  per_file_copt for glib|zlib|pcre2). A blanket external/.* cancel broke abseil (SwissTable layout
  depends on ASan; a SEGV in metadata_cache_test), so abseil, libfuse, sqlite etc. stay sanitized.
  Use-after-free still caught. Runtime: pjdfstest_test_ext4 406 s plain, 481 s before, 457 s after;
  the rest of ASan's cost is dcfs itself (intended). Residual: pjdfstest still NEEDs libasan (Bazel
  appends global linkopts after target linkopts; no per-file linkopt).
- Follow-up (Phase 6.2/6.3): the big remaining cost is rebuilding glib+QEMU (~450-470 s) whenever a
  configuration's disk cache is cold, because sanitizer configs change those actions' keys. Build
  third-party tools in a configuration independent of --config=asan/ubsan (e.g. an exec or
  flag-resetting transition on the tool targets), so plain and sanitized runs share one QEMU build.
- S2 PASSED (2026-10-06): main 318ad59; plain 75/75; full ASan 75/75 (73 executed, 42 min wall,
  13:05-13:47, machine otherwise idle). Phases 2, 3 and 4 done; Phase 6.3's first fix merged. Wave 2
  complete. Next: russ restarts Claude Code (agent types with effort), then wave 3 (Phase 5.1 TCG
  speed, Phase 6.1 tiers) plus the 6.2 follow-ups (shared xfs quiesce helper, config-independent tool
  builds).
- 2026-10-06: Claude Code restarted (agent types with effort available; russ now has kvm and docker groups directly). Wave 3 dispatched: lane-1 Phase 5.1 TCG speed (dcfs-investigator), lane-2 Phase 6.1 tiers (dcfs-implementer).
- russ: orchestrator (Fable 5.1 this session) is overseer only: decide, dispatch cheaper subagents, review; Fable subagents only for the most complicated tasks, if ever (CLAUDE.md).
- Review of waves 1-2 in (dcfs-reviewer; audits/review-2026-10-06-waves-1-2.md). No critical bugs.
  Triage into three fix steps, each test first, queued for the next free building lane:
  R1 cache hardening (dcfs-protocol): M1 trust the cache directory (open O_DIRECTORY, require
     root-owned and not group/other-writable, else refuse), st_nlink==1, SQLITE_OPEN_NOFOLLOW,
     check -journal; L7 test gaps; L6 docs.
  R2 build/test infra (dcfs-implementer): M2 QEMU -fno-sanitize=address,undefined as one word (+
     ubsan readelf check); M3 CONFIG_POSIX_TIMERS=y and audit of EXPERT-gated options tinyconfig drops
     (KCMP, AIO, SYSVIPC, ...), L8 XFS_QUOTA; L1 build_kernel.sh set -e suspended in the logged block;
     L2 busybox fragment survival check + smoke test that runs each feature; L3 `stat %U/%G` compare
     nothing in the guest (use %u/%g); L4 boot.sh must fail on a kernel version mismatch; L12 fail on
     a failed thaw; L10/L11 reproducibility and nits.
  R3 pinned mkfs (dcfs-investigator): L5 scratch filesystems still made by host mkfs.ext4/xfs/btrfs
     with host defaults: Bazel-built mke2fs with an explicit config, pinned xfsprogs and btrfs-progs;
     L9 Debian lock enforcement. Phase 4 reopened for this.
- russ: M1 (hostile cache directory) not pursued; replaced by one startup check (database no more accessible than the backing root directory). Remaining review findings planned in review-fixes.md (R1-R3). lane-3 added (capped) for R1+R2.
- russ: more parallelism, swapping accepted. lane-4: R3a (Bazel-built xfsprogs/btrfs-progs, mke2fs config, Debian lock; runner wiring waits for 5.1); lane-5: Phase 12.1 TLA+ model (dcfs-protocol), started early since it depends on nothing in flight.
- Phase 6.1 merged (9887599; dcfs-implementer): size is the tier, macros fail without size/timeout,
  real resource tags (cpu:N, resources:memory:N verified by observation), `exclusive` removed from
  e2e. fast 46 tests 1m05s, presubmit 70 tests 2m15s, full 75 in 35m44s (baseline 39m35s), all green.
  Notes for 6.2: pjdfstest ~570 s alone, ~1065 s when two run side by side (within eternal);
  write_test_btrfs's write-large-passthrough-cpu check (cpu ticks < 20) flaked once under load and
  may flake more with parallel guests: make that check robust test-first, never loosen it blindly.
  The agent hit a permission denial killing its own stray wait loop and used TaskStop instead.
- R3a merged (2f2eda5; dcfs-investigator): Bazel-built static mkfs.xfs/xfs_io (xfsprogs 7.2.0),
  mkfs.btrfs/btrfs (btrfs-progs 7.1, no lzo/zstd), with private static libuuid/libblkid (util-linux
  2.42.4), liburcu, inih; checked-in mke2fs.conf (1.47.4's built-in profile) used for the Debian
  image; --lockfile_mode=error verified to catch pin drift; debs.lock covers all 149 .debs. Smoke
  tests plain and ASan. Pending R3b (after 5.1 merges): wire the tools into run-qemu.sh (patch in
  the agent's report) and pin 4 KiB blocks for the small test images (the built-in profile would give
  64 MiB images 1 KiB blocks, unlike a real disk). Note: inih's hash is of a GitHub tag archive, not
  a release asset (not byte-stable by contract).
- Phase 10 (benchmarks, idle and memory tests) started in lane-4 (dcfs-implementer).
- Machine saturated (load 19, swap 9/9 GB): Phase 10 agent stopped and lane-4's Bazel server shut down; its uncommitted work stays in lane-4 for a later agent. Resume when load allows.
- Phase 12.1 merged (1683e6b; dcfs-protocol): formal/dcfs.tla + MC configs, TLC 1.7.4 on a remote
  JDK via third_party/tlaplus/tlc_test; 11 tests green (small 241k states/54 s, large 2.6M/9 min);
  the four historical bugs reintroduced each give the expected counterexample. Three latent gaps
  found in today's code (formal/findings/, unreachable while single-threaded under the kernel's
  per-directory lock, real under coroutines): (1) SyncBacking/ClearDirty drop inodes with a mutation
  in flight, so a sync point between phase 1 and the syscall loses the dirty row (CrashSafe);
  (2) Readdirplus/Readdir check completeness, run syscalls, then list without rechecking
  (ServedFromCacheIsCurrent); (3) rename phase 3 links a source id resolved before phase 1 and the
  hard-link case escapes LinkDentry's rollback (CacheNeverWrong). Queued as step R4 (dcfs-protocol,
  test first, each fix moves its findings/ config into the real model), before Phase 13.
- Needs russ: read formal/README.md (the newcomer guide) and say whether the model is legible.
- Phase 9 merged (a8d2688; dcfs-implementer): EscapeBytes/UnescapeBytes with an exhaustive round-trip
  test over all byte strings up to length 3; 14 log sites escaped; strlen walk over the xattr list
  replaced; 61-name corpus (hazard classes + generic/453-454 sets) through both population and
  mutation on ext4/xfs/btrfs, restart with zero backing reads, handles, PATH_MAX-deep chains; seeded
  random names (1,000 medium; 100,000 enormous on a 4G disk). The only dcfs bug found: a newline in
  a name forged a log line (fixed; failing-first quoted). Deferred: escaping of flag paths in
  main.cc's startup messages (lane conflict); octal escaping for Phase 15's text formats.
- Load flakes now recurring (3 building lanes + 4 slow guests): write_test's
  write-large-passthrough-cpu (dcfs cpu ticks < 20) fails under load on all three filesystems, and
  pjdfstest_test_{ext4,btrfs} hit run-qemu.sh's 1200 s guest limit (pass alone in ~890 s). Both
  are 6.2 work, test first, no loosening without understanding; dispatched now to lane-2.
- Phase 5.1 merged (ceb7925; dcfs-investigator): TCG hung in LAPIC timer calibration (no kvmclock,
  PIT and PIC off); fix is pit=on,pic=on under TCG only (KVM path unchanged, boot_test 2.1 s before/
  2.4 s after). DCFS_FORCE_TCG=1 forces TCG; kernel cmdline carries dcfs_accel; timeouts set after
  the accelerator is known (KVM unit 60/e2e 1800 s; TCG unit 300/e2e 7200 s). TCG vs KVM (loaded
  host): boot 8.6 s alone; unit tests 6-14 s; most e2e 2-9x; pjdfstest_ext4 3502 s (5.7x) which is
  close to its eternal 3600 s Bazel timeout; readdir_boundary 488 s (12x, fork-heavy shell loop).
  TCG suite 66/70; the 4 failures are write_test's CPU-tick check (ticks 46/40/20 under TCG vs the
  < 20 limit; already borderline on btrfs under KVM at 19): handed to the 6.2 lane working on it.
  Follow-ups: R3b in lane-1 now (mkfs wiring, 4 KiB blocks, README's TCG wording, run-qemu.sh
  nits); CI step decides pjdfstest under TCG (ext4 only, or a longer timeout).
- 6.2 CPU-tick flake fixed (25f7f10; dcfs-investigator): the old write-large-passthrough-cpu check
  (daemon CPU ticks < 20) rose with host load because guest stime accounting inflates when vCPUs
  are descheduled, not because dcfs did work; dcfs handled 70 requests during the 64 MiB write (64
  GETXATTR, 0 READ/WRITE). New check write-large-passthrough-requests counts daemon wakeups
  (voluntary context switches) in a window that ends after the writer's close: 9-10 with
  passthrough on (KVM under load 24/24, TCG 6/6), 73-76 with passthrough off (scratch build, 12/12
  fail). The first write.sh daemon runs with --sync_interval_sec=100000 so a periodic sync does not
  land in the window.
- dcfs finding (queue for dcfs-protocol, small): with passthrough on, the kernel still sends one
  GETXATTR(security.capability) per write(2) (file_remove_privs); a program writing 4 KiB at a time
  wakes dcfs once per write. Declaring FUSE_CAP_HANDLE_KILLPRIV_V2 (dcfs's backing filesystem kills
  privileges on the real inode during passthrough writes) should stop them; verify the kernel's
  behaviour for passthrough writes before enabling, test first (count GETXATTRs during a 4 KiB-write
  loop). Also check guest/passthrough.sh's own cpu_ticks check for the same flake.
- Phase 10 resumed in lane-4 (load back to ~8) from the earlier agent's uncommitted work.
- R3b merged (ae5c462; dcfs-implementer): run-qemu.sh requires --mke2fs/--mke2fs-conf/--mkfs-xfs/
  --mkfs-btrfs (refuses /usr, /bin, /sbin paths, logs them), the macros pass the Bazel-built tools;
  mke2fs.conf's small/floppy profiles set blocksize=4096 (boot.sh vdb-block-size check failed with
  1024 before, passes after); host-side run_qemu_mkfs_test with fake tools; README/test README drop
  "ten-plus times slower". Full suite 100/103 before rebase; the 3 were the since-replaced CPU-tick
  check. Phase 4 and R3 complete: no host mkfs, QEMU, qboot, busybox, kernel or mke2fs remain.
- Phase 10 merged (8288067; three dcfs-implementer agents): google/benchmark suite in the guest
  (bench/dcfs_bench, dm-delay and mktree helpers, clock shim for the timer-less guest kernel), idle
  test (60 s warm cache: zero backing reads/writes; the cold control moves the counter), memory test
  (second tree adds at most half of the first's growth; TCG uses 20k entries), smoke and full runs.
  Baseline, KVM, fastbuild (NOT optimized): stat 303 us dcfs vs 801 us backing; lookup 1.5 ms vs
  5.5 ms (slow 5 ms backing: 20 ms vs 92 ms); open+close 738 vs 516 us; small read 1.7 vs 0.95 ms;
  startup with 100k entries 20 ms; recovery of 10k dirty entries 166 ms; ~100 bytes per referenced
  inode. FINDING: cached readdir of a 10k-entry directory 3.5 s through dcfs vs 0.13 s backing
  (27x; readdir without plus 2.1 s). Investigation dispatched to lane-4 (dcfs-investigator):
  measure with `-c opt` too, profile, find the per-entry cost. Benchmarks should also get an
  optimized-build run (6.2 follow-up: a `bench_full_opt` config or `-c opt` in its rule).
- Phase 10 merged (fd47bbd; three dcfs-implementer agents): google/benchmark suite in the guest
  (bench/dcfs_bench, dm-delay and mktree helpers, clock shim for the timer-less guest kernel), idle
  test (60 s warm cache: zero backing reads/writes; the cold control moves the counter), memory test
  (second tree adds at most half of the first's growth; TCG uses 20k entries), smoke and full runs.
  Baseline, KVM, fastbuild (NOT optimized): stat 303 us dcfs vs 801 us backing; lookup 1.5 ms vs
  5.5 ms (slow 5 ms backing: 20 ms vs 92 ms); open+close 738 vs 516 us; small read 1.7 vs 0.95 ms;
  startup with 100k entries 20 ms; recovery of 10k dirty entries 166 ms; ~100 bytes per referenced
  inode. FINDING: cached readdir of a 10k-entry directory 3.5 s through dcfs vs 0.13 s backing (27x;
  plain readdir 2.1 s). Investigation in lane-4 (dcfs-investigator): measure with -c opt too,
  profile, find the per-entry cost; benchmarks also need an optimized-build run.
- Orchestrator note: a rebase conflict in test/qemu/BUILD.bazel (R3b and Phase 10 both appended
  targets) was resolved by keeping both blocks; the readdir agent found the lane mid-rebase and
  correctly stopped until it was clean.
- Phase 5.2 (CI workflow developed with pinned act; full job under act as the host-dependency detector, run only when load allows) started in lane-2 (dcfs-investigator). Nothing is published; russ pushes.
- R4 done on step-r4 (dcfs-protocol, 4 commits, +1339/-359), awaiting an independent dcfs-reviewer
  pass before merge: sync points snapshot the guard clock and dirty set before syncfs and ClearDirty
  keeps anything newer (the suggested "keep in-flight inodes" was insufficient per the model);
  readdir takes its listing right after the completeness check; rename re-verifies its resolution
  inside phase 1 (kAborted -> re-resolve, 3 tries then EAGAIN). New dcfs:dir_cache_fs_test drives
  DirCacheFS through forged FUSE requests with a --wrap hook. Model: SyncExclusive removed; the
  three findings became Bug* constants with known_bugs variants; MC_nolock now covers all requests
  (4.46M states). Unit+formal 29/29, ASan 18/18, presubmit 93/93. Follow-up found by the agent:
  RemoveChild/BeginRemove/SettleUnlinkedFile trust a child id resolved before phase 1 (same shape as
  the rename gap; outside the model's scope): queue as R4.4 after the review.
- russ: two additions. Phase 5.3: OSV-Scanner GitHub Action over an SBOM generated from all our pins (BCR, http_archives, Debian debs), test first with a known-vulnerable entry, weekly schedule, ignores with expiry. Phase 22: request cancellation (FUSE_INTERRUPT via libfuse): inventory worst-case durations on a slow backing, safe points, tri-state-preserving semantics (not cancellable between a mutation's syscall and phase 3), single-thread caveat (interrupt arrives as its own request), coroutine/io_uring token design, unit and guest tests first, docs; even if everything is fast, the policy, tests and docs land. Runs after R4, before Phase 13.
- R4 merged (c0b27a5..411686f) after a dcfs-reviewer pass (audits/review-2026-10-06-r4.md): all three
  fixes verified against their invariants; no regression reachable today. New findings (coroutine
  future, fix now): writable opens escape the sync snapshot (high), RemoveChild has the rename gap
  (confirmed), rename's retry loop cannot wait, two test gaps, a ClearDirty O(n*m) fast path. Queued
  as R4.2 in review-fixes.md, dispatched to lane-1. The reviewer detached lane-1's HEAD by mistake
  (reported it); restored.
- russ: 12.2 trace validation pulled forward to right after R4.2 (traces from existing tests and the forged-request harness; reviewer checks call sites).
- Readdir performance (lane-4, dcfs-investigator, pending a schema-version follow-up before merge):
  cause was a missing index, not a design problem: ListDir's page query (parent, rowid>cursor,
  state='present', ORDER BY rowid LIMIT 64) and IsDirComplete's unknown-name probe each scanned the
  whole directory per request, making a listing O(n^2) (2k entries 0.19 s, 10k 2.4 s). Fix: partial
  indexes dentries_present and dentries_unknown; per-page cost now constant (metadata_cache_test
  counts SQLite VM instructions). 10k entries: fastbuild 2409 -> 284 ms; optimized 704 -> ~160-300 ms
  vs backing ~30-170 ms (noisy host). `-c opt` is unusable for the bench (rebuilds kernel/QEMU;
  QEMU's build then tries pip offline): README documents a per_file_copt -O2 for dcfs/bench/sqlite/
  abseil/libfuse/benchmark instead. New bench_readdir_test (large).
- Watch (6.2): pjdfstest_test_ext4 took 1700 s under load in that lane, against run-qemu.sh's new
  1800 s KVM e2e limit; either the limit needs headroom for a loaded host or pjdfstest needs
  sharding by test directory.
- Readdir fix merged (b829d0e, 3366525): schema v3 migration adds the two indexes to existing caches (v2 database test failed first on the version). The 5000-entry test fill now uses one SQL statement after an OOM in the 256 MB ASan guest.
- 6.2 bundle dispatched to lane-4: passthrough.sh's cpu_ticks check (convert to wakeups if it flakes; prove passthrough-off still fails) and pjdfstest sharding by test directory (1700 s vs 1800 s KVM limit under load; 3502 vs 3600 s under TCG).
- Load 28 with four building lanes plus act: paused the 6.2 pjdfstest/passthrough lane (just started) until R2 merges.
- R4.2 merged (ef5dbf0..437f65f; dcfs-protocol): all five review follow-ups, each failing first
  (ReleaseDuringASyncPointKeepsTheDirtyRow, WritableCreateIsDirtyWhenReplied,
  UnlinkMarksWhatItRemovesUnknown, ...); harness gains --wrap=syncfs and --wrap=name_to_handle_at
  hooks. Unit+formal 29/29, ASan 18/18, presubmit 101/101. Coroutine note recorded in design.md.
  Bazel servers were OOM-killed twice this evening when swap filled; agents now pass --jobs=2.
  Next in lane-1: 12.2 trace validation.
- R1+R2 merged (a4bfc3c..11b3b25; dcfs-implementer, ~10 h incl. OOM-killed servers): startup check
  that the database grants no more access than the backing root (cases 6-12 failing first; a
  database owned by the backing root's owner is now accepted); M2 one-word sanitizer cancel (ubsan
  smoke test failed first on libubsan); kernel config block (POSIX_TIMERS, KCMP, AIO, SYSVIPC,
  ADVISE_SYSCALLS, MEMBARRIER, RSEQ, BUG, XFS_QUOTA, LOCALVERSION=-dcfs-stock; nfs_test
  timers-timeout-fires failed first); build_kernel.sh re-execs under sh -eu; boot.sh requires the
  -dcfs-stock stamp; busybox survival check + feature smoke test + FEATURE_MOUNT_FLAGS; %u/%g in six
  scripts (no mismatches exposed); drop_caches_quiesced before every zero-reads baseline, failed
  thaw fails the test; reproducible kernel/busybox/rootfs stamps; clock shim removed. Full suite
  116/117: idle_short_test_xfs FAILS (writes +4 over 60 s idle), pre-existing (also fails on the
  base commit): investigate now (lane-4) since idle writes defeat spin-down.
- 12.2 done on step-12.2 (dcfs-protocol; +4159/-137), under a dcfs-reviewer call-site pass before
  merge: ProtocolEvents interface (no-op in production), testonly recorder (link-time selected
  main_static_traced), formal/Trace.tla + tla_trace_test (TLC with CommunityModules, pinned, plus a
  trimmed TLCOverrides jar the stock jar needs), traces from the harness (29 valid) and guest
  crash/power/create/rename tests (6/5/5/10 valid); fault build (phase 1's mark-unknown skipped) is
  rejected at that event. Finding: RecoverDirty marks dentries unknown in directories that are not
  dirty themselves; Trace.tla's T_Recover allows it explicitly and the model should gain it. Never
  taken: UnlinkFailed, RenameFailed, RenameFailed2 (unreachable without out-of-band changes). Note:
  the traced rename run flaked on rename.sh's unlink-mnt zero-reads check when a sync point fell in
  the window under tracing load (passes 4/4 after trimming output): rename.sh's windows are fragile
  under load (6.2).
- 12.2 review (audits/review-2026-10-06-trace-validation.md): event placement sound, production
  code clean; but validation unsound as delivered: structural cuts (syscall before phase 1, End
  before syscall, silent getattr) stop the trace at a valid prefix and trace_validate.sh passes any
  trace without an invalid line; no syscall-start events (a sync snapshot taken after syncfs began is
  accepted); InodeForgotten cuts whole directories; fills over valid attributes unchecked;
  mid-run initial state trusted; T_Recover wider than RecoverDirty. Sent back as 12.2b with the
  reviewer's fixes and two more fault tests; re-review required before merge.
- 2026-10-07 ~03:10: a usage limit cut off the three running agents (12.2b, 6.2 bundle, 5.2 CI) mid-step; all three resumed with their context after the reset. Lanes' uncommitted work was intact.
- Phase 5.2 merged (f9380f1; dcfs-investigator): .github/workflows/ci.yml with fast/presubmit/full
  jobs and .github/ci/*.sh; act v0.2.89 pinned (http_archive), runner image catthehacker ubuntu
  act-latest pinned by digest, actions by SHA; wrapper passes /dev/kvm + kvm gid, label
  dcfs.owner=act, --rm, a 6g memory cap. Under act with KVM: fast 52/52 (warm 3-8 min; cold 1h04
  incl. a 40 min kernel build), presubmit 100/100 (8-16 min), full plain 116/117 (idle_short_test_xfs
  again), full ASan 108/117: memory_test x4 (ASan's allocator inflates bytes per entry: 7862 vs 256;
  the test must not assert allocator-dependent bytes under sanitizers while keeping the second-tree
  bound), names_random_slow_test x4 (dcfs DISCONNECTS, ENOTCONN, during the 100k-name run under ASan
  on the host too: likely a real memory error only the slow tier reaches: investigate first). Host
  dependencies the detector exposed, now installed in prepare.sh and listed in README: cpio
  (mkinitramfs.sh printed "cpio: not found", exited 0 and cached an EMPTY initramfs: fix with
  pipefail or busybox's cpio), ninja (QEMU configure), flex/bison (kernel), libelf-dev (objtool).
  ftp.gnu.org unreachable once: gnu_bc has a mirrors.kernel.org second URL. pjdfstest under TCG:
  ext4 only, 7200 s. Kernel matrix deferred (kernel.config asks for FUSE_IO_URING, 6.14+; the stated
  minimum of 6.9 is not buildable as pinned). Nothing published; russ pushes when ready.
- 12.2b done on step-12.2 (dcfs-protocol, 7 commits): cuts restricted to unmodelled steps with
  per-target allowlists (harness: none), forbidden steps become rejected lines or `unexplained`;
  MutationSyscallStarting/SyncfsStarting events; InodeForgetting before the DELETE; fills report
  the code's decision; begin lines carry an origin checked by TraceInit; RecoverForgetting added to
  dcfs.tla with T_Recover restricted to the recorded dirty keys (state counts ~1.7x: small 688k,
  large 6.3M, nolock 5.1M). Fault tests: syscall before phase 1, phase 3 before syscall, snapshot
  after syncfs, skip mark-unknown (reason-checked), plus recorder unit tests and hand-written trace
  tests; all reject. 44/44 formal+dcfs, presubmit 116/116. Limitation: guest root traces for
  crash/rename/create stop at the first link or cross-directory rename (explicit root_cuts); only
  power.sh's reaches the end; modelling links/cross-directory renames is future model scope. Second
  reviewer pass dispatched before merge.
- 6.2 bundle done on step-6.2-pjd (dcfs-investigator), merging after rebase: xfs idle writes are
  xfs_log_worker's log covering (two +2 bursts at 30 and 60 s after the last write, then nothing;
  identical with dcfs gone), test warm-up now quiesces, README note for spin-down users;
  passthrough.sh's tick check passed 2/6 with passthrough OFF (too weak, not flaky): now counts
  wakeups (3-5 on vs 68-150 off); pjdfstest PASSED VACUOUSLY since 4d (busybox tail lacked -1:
  8570/8827 checks failed on both sides; FEATURE_FANCY_TAIL added, smoke test checks tail -1, new
  pjdfstest-suite-sane guard fails if >5% fail on the raw fs); pjdfstest sharded into rename /
  chown+chmod / rest per filesystem (187-474 s per shard under load; rename/ alone is 90-150 s).
  Lesson (second time): a pinned minimal busybox can hollow out tests that compare two sides;
  every comparison-style test needs a sanity floor like pjdfstest's.
- 12.2 re-review (audits/review-2026-10-07-trace-validation-2.md): original holes closed; new: held lines dropped when a directory dies during a listing (a forbidden step inside a population validates), 'failed' cuts for a half-way listing and for unmodelled syscall errnos fire regardless of the request's outcome, dir-itself cut too broad, child-fill guard judged by the code's own CanFill, root check accepts a trace that merely stops, fault regexes lack reason text. Sent back as 12.2c; merge after.
- 6.2 lane-4 (dcfs-investigator): (1) idle_short_test_xfs: the +4 writes are xfs log covering
  (xfs_log_worker, fs.xfs.xfssyncd_centisecs=3000: +2 writes at ~34 s and +2 at ~65 s after the
  last write, nothing for the remaining ~175 s of a 240 s window; the same with dcfs unmounted), a
  bounded settling, not a recurring cost; fixed in the test by drop_caches_quiesced in the warm-up
  (freeze/thaw covers the log), 9/9 runs pass on ext4/xfs/btrfs, and a scratch dcfs that writes
  and syncs every 20 s fails all three (not committed). README note for spin-down users.
  (2) passthrough.sh cpu_ticks: did not flake (0-5 ticks on KVM under load and TCG) but was too
  weak: with passthrough off in a scratch build it read 7-86 ticks and passed 2 of 6 runs; now
  counts FUSE wakeups (< 32; on: 3-5, off: 68-150, 6 of 6 fail). Helpers moved to lib.sh.
  (3) pjdfstest sharded (rename / chown+chmod / rest, test_suites keep the old names), shards
  3-8 min each on a loaded host (single guest before: ext4 817-989 s, btrfs 755 s, xfs 685 s).
  FOUND: pjdfstest had been passing vacuously since busybox lost `tail -1`
  (8570 of 8827 checks failed identically on the raw filesystem and through dcfs):
  CONFIG_FEATURE_FANCY_TAIL added with a smoke check, plus a pjdfstest-suite-sane guard (<5% of
  checks failing on the raw filesystem). With tail fixed both sides fail the same 28 TODO checks
  (ext4/xfs/btrfs shards sum to 8827 checks).
- 6.2 bundle merged (tip bf6c3fc). lane-4 next: Phase 5.3 OSV-Scanner.
- 5.3 OSV-Scanner (step-5.3-osv). Scanner v2.6.0 (action commit a345acff...). SBOM: 132 entries,
  102 Debian source packages matchable, 30 pins OSV cannot match (github/generic purls give no
  findings even for known-bad versions; Linux ecosystem not returned by api.osv.dev for old
  kernels): README. Seeded zlib 1.2.11 under act: 12 findings, job's self-check passes. Real
  scan under act: red, 158 findings in 26 Debian bookworm source packages (python3.11 24, expat
  28, glibc 16, perl 16, openssl 12, util-linux 9, libevent 8, pcre2 7, sqlite3 6, others <=3);
  fixed versions exist in bookworm for 57 of them (expat, libevent, openssl, pcre2, perl:
  newer than the 20261004 snapshot's main suite) - a snapshot bump or ignores with reasons are
  russ's call; none added.

- Phase 5.3 merged (323ad67; dcfs-implementer): tools/sbom (CycloneDX from every pin: 132 entries,
  102 Debian source packages; sbom_test fails on a pin without an entry, hermetic Python via
  rules_python), osv job (action pinned by SHA for v2.6.0; weekly cron; seeded zlib 1.2.11 fixture
  must fail first, and does: 12 CVEs), empty osv-scanner.toml (ignores need id, ignoreUntil,
  reason). OSV matches ONLY the Debian packages: pkg:github and pkg:generic purls report nothing
  even for known-bad zlib/xz, and the kernel has no reachable ecosystem; README lists every such
  pin. Real scan: 158 findings in 26 bookworm source packages (notes/osv-scan-2026-10-07.txt); 57
  have fixes in bookworm, which means the image sources omit bookworm-security: fixing that now
  (lane-4). The rest (unfixed in bookworm) need russ's policy: ignores with a reason and expiry
  (the image is a test-only NFS chroot), or moving the image to trixie.
- Needs russ: policy for OSV findings that Debian has not fixed in the pinned release.
- 5.3b security suites: third_party/debian now resolves bookworm + bookworm-updates
  (archive/debian/20261006T082722Z) + bookworm-security (archive/debian-security/20261006T081244Z);
  the old 20261004 snapshot lacked perl 5.36.0-7+deb12u4. Moved: libevent, expat, xz-utils, pcre2,
  openssl, perl. nfs_test, version_check_test, ownership_test, sbom_test pass. osv under act: 158 ->
  101 findings in 22 packages, all 101 unfixed in bookworm (no ignores added).
- 5.3b merged: Debian image now sources bookworm + bookworm-updates + bookworm-security (snapshots 20261006T082722Z / debian-security 20261006T081244Z; six source packages moved; nfs_test passes). OSV: 101 findings in 22 packages, all unfixed in bookworm as of 2026-10-06; policy still needs russ.
- russ: OSV scans shipped dependencies only (abseil, SQLite, libfuse, liburing, toolchain); the 101
  Debian-image findings are test-only and do not matter. "Needs russ: OSV policy" resolved. 5.3c
  dispatched to lane-4: SBOM split into shipped (gating) and test-only (informational), commit-based
  matching for the shipped pins, verified with a known-vulnerable commit.
- 5.3c (lane-4, step-5.3c-scope): OSV scope is shipped dependencies. Shipped = external repos in deps(//dcfs:main,
  //dcfs:main_static): abseil-cpp 20260817.0, gloop 20260708.rc1, libfuse 3.18.2, liburing 2.14, numactl
  2.0.19.bcr.1, sqlite3 3.53.4 (build-only: rules_cc, platforms, bazel_tools). SBOM split into shipped (gates)
  and test-only (informational). OSV matches C/C++ by git commit only and only through osv-scanner git roots
  (purl with commit reports nothing); libfuse 3.2.0 -> CVE-2018-10906 proves it. act -j osv: shipped scan
  clean, test-only 101 findings printed but not gating. OSV holds records for libfuse, sqlite, abseil
  (main-branch history only; LTS tags unmatched); none for gloop, liburing, numactl.
- 5.3c merged (fbbf780): OSV gates only the shipped set, derived from the Bazel graph of
  //dcfs:main(_static) (abseil, gloop, libfuse, liburing, numactl, sqlite3), matched by git commit
  through synthetic git roots (purls match nothing); proof: libfuse 3.2.0's commit reports
  CVE-2018-10906, sqlite 3.30.0 gives 19. OSV holds nothing for gloop, liburing, numactl, and its
  one abseil record misses LTS tags (documented). Shipped scan: no issues. Test-only scan
  informational (101 findings), never gates. verify-commits needs network in CI.
- ASan lane merged (d47929c..b8622e0): names_random_slow's ENOTCONN was the GUEST OOM killer, not a
  dcfs bug (ASan dcfs VmHWM 716 MiB vs 55 MiB plain; reproduced at 640 MiB, OOM lines in dmesg):
  slow names tests get mem=2048 (new run-qemu.sh --mem, qemu_test mem=); memory_test runs the ASan
  daemon with the quarantine off and asserts bytes-per-inode only plain (223 measured under ASan
  without quarantine, so it would hold); mkinitramfs.sh uses busybox cpio and fails loudly (host
  test with cpio hidden). Note: .github/ci/prepare.sh still installs cpio (kernel build may need it).
- Session cut off by the usage limit while 12.2c (trace validation final round) was finishing its
  tests in lane-1; resume it first. Then S3 (full plain + ASan on main).
- 12.2c merged: trace validation complete (two reviewer rounds; audits/review-2026-10-0{6,7}-trace-
  validation*.md). Forbidden steps now reject or become `unexplained` (no hiding behind cuts: held
  lines flushed, deferred "failed" cuts decided at the reply, dir-itself tied to the request's own
  resolution, fills judged by the recorder's own mutation record, root traces must reach the run's
  final event); 54 formal+dcfs tests, 126 presubmit, four guest trace tests green; faults rejected:
  skip mark-unknown, syscall before phase 1 (unlink and create-inside-listing), phase 3 before
  syscall, snapshot after syncfs, swallowed syscall error. Phase 12 done. S3 next.
- russ decided (2026-10-07): FORGET reconciliation for mmap-after-close, mutations on removed-but-referenced objects, relatime semantics, and O_TMPFILE/copy_file_range/FICLONE/ioctls: Phase 23 (with boundary stubs pulled forward as 23.5: visible in readdir, ENOTSUP inside, not EXDEV). Dispatch after S3 (memory).
- Cleanup: ~90 orphaned agent polling loops (self-matching 'while pgrep -f' and 'until grep -q MARKER' loops, some a day old) killed; only the S3 run and russ's shells remain. Rule added to CLAUDE.md and the agent definitions.
- S3 plain: 157/158 on dde52b9; the one failure was sbom_test catching 12.2's new tla_community_modules pin with no SBOM entry (the test doing its job); fixed (8720d86). ASan suite in progress.
- S3 PASSED (2026-10-07): main dde52b9 + the sbom pin fix 8720d86. Plain 157/158 (119 executed,
  84 min), ASan 157/158 (129 executed, 97 min), the single failure in both being sbom_test's
  missing tla_community_modules pin, fixed on main before the ASan run ended. Every former flake
  held: idle_short_test_xfs, write/passthrough wakeup checks, all nine pjdfstest shards,
  names_random_slow under ASan on three filesystems, trace tests, bench_full, idle_long, memory.
  Capped server (1.5 GB heap, memory=3500, one test at a time) ran without OOM at load 1-5.
  Wave 3 complete. Next: Phase 23 (semantics gaps) in lane-1; pending build-speed measurements in
  lane-4 (two building lanes).
- Build-speed measurements complete (notes/build-speed-2026-10-07.md): cold total ~32 min of actions, kernel 690 s + QEMU 518 s = 62%; sanitizer configs rebuild the six foreign_cc tools (~890 s avoidable); exec-config actions (kernel, busybox, bc, Debian image, TLC) are config-independent already. Distro kernels: Alpine linux-virt and Debian 13 boot no faster than ours under KVM, both lack DM_DUST, Debian needs ACPI and modules, Alpine pins vanish from mirrors within weeks. Recommendation: make the tool builds config-independent now; remote cache for CI; do not replace our kernel.
- Phase 23 implemented on step-23 (dcfs-protocol; +4235/-356; presubmit 140/140, formal 22/22,
  large/enormous 14/14, ASan small 24/24, 38 traces valid), under dcfs-reviewer before merge.
  Findings: FICLONE/FICLONERANGE/FIDEDUPERANGE cannot be implemented in a FUSE server (the VFS
  handles them; FUSE has no remap_file_range op): kernel work; cp --reflink=auto still shares
  extents via copy_file_range, which now works. The "mapping writes longer than the kernel keeps
  the inode" case cannot happen (the backing file holds the FUSE path, so the last FORGET follows
  munmap). Bug found and fixed: after chattr +i a writable open could still write through a
  pre-flag shared backing fd. Deviations: stubs are rows (schema v4 `stubs` table), link/rename
  INTO a stub gives ENOTSUP (kernel looks the target up first), O_TMPFILE is a row without a
  dentry, removed-object changes need no tri-state (record caches nothing mutable; reviewer to
  confirm). Needs russ: FORGET reconciliation's statx may spin up a sleeping disk when the backing
  fs has dropped the inode; alternative is holding one fd per written file until its FORGET.
- russ (2026-10-07): held fd per written file instead of a statx at FORGET (23.6), documented in
  three linked places so the workaround is removed when the kernel keeps the FUSE file referenced
  by passthrough mappings; tool builds independent of sanitizer flags: do it (6.3b, lane-4 now);
  remote cache: dropped (hosting not worth it); prebuilt kernel: dropped for the same reason;
  Alpine linux-virt: no durable pin even per series (old apks vanish), equal KVM boot time, no
  DM_DUST: keep our kernel; chattr +i class -> a small second TLA+ module (12.3).
- Phase 23 reviewed (audits/review-2026-10-07-phase23.md): no high findings; M1 SETFLAGS lets
  chattr +F create a casefold dir under dcfs; M2 DESTROY reconciliation cost and the
  live-mapping-after-unmount case; lows on shared-fd writability, stub errnos, stale stub ESTALE,
  orphan tmpfile rows, atime docs. NOT yet merged: the lane's rebase onto main conflicts in two
  plan files the agent also edited; the agent resolves it, then I merge. 23.6 + 23.7 follow.
- Orchestrator error: a merge command chain continued past a failed rebase and ran its later steps
  inside lane-1 (a stray detached commit; aborted, nothing lost). Merges are now one command per
  step, never chained past a rebase.
- 23.5 (lane-1, step-23): boundary stubs. A refused dentry is a stub directory (schema v4 `stubs`:
  nodeid >= 2^63, random generation, boundary root's attributes, triggers tie it to the refused dentry),
  listed (ListDir merges present + refused in rowid order, cost test unchanged), ENOTSUP inside (logged
  once per stub), EXDEV for renaming it; backing inode numbers >= 2^63 refused (ENOTSUP). Link/rename
  into a stub give ENOTSUP, not 15.1's EXDEV: the kernel looks the target up in the stub first.
- 23.2 (lane-1, step-23): changes to removed-but-referenced objects (SETATTR, xattrs, FSYNCDIR, OPEN
  of /proc/<pid>/fd of an O_PATH fd) go through the removed_ record's fd; the record caches nothing, so no
  tri-state or model change is needed. removed_test (3 fs) and harness tests; pjdfstest ext4 shards green.
  The plan's "unlink-then-ftruncate on an open file fails today" was not so: that already worked.
- 23.1 (lane-1, step-23): FORGET reconciliation of files written this run (statx by handle; a change is
  recorded as a phase 1 + fill). The last FORGET cannot precede munmap (backing_file_open pins the FUSE
  path), so the stores are always seen then; remaining: staleness between munmap and FORGET, and a possible
  disk spin-up for the statx. drop_caches_quiesced waits for the daemon (reconcile I/O before baselines).
- 23.4 (lane-1, step-23): copy_file_range (backing CFR: extents shared on btrfs/xfs), the ioctl allowlist
  (chattr/lsattr/GETVERSION; else ENOTTY), O_TMPFILE (a row, linked by LINK = the model's new
  "linkcreate", validated). FICLONE/FICLONERANGE/FIDEDUPERANGE are impossible without a kernel FUSE
  remap op: the VFS answers EOPNOTSUPP before FUSE. Writable OPENs now re-check writability (chattr +i).
- 23.3 (lane-1, step-23): relatime: a read open records the atime the backing mount's rule gives
  (relatime incl. ctime, strictatime, noatime from statvfs at InitRoot), cache only. atime_test (3 fs),
  RelatimeTest.*, TouchAtime cache test.
- Phase 23 (23.1-23.5) merged (1554bee..e5c27cc). 23.6 + 23.7 starting in lane-1.
- russ (2026-10-07): FORGET hook stays (RELEASE precedes munmap; only FORGET follows it); kernel
  pinned to a series is acceptable in principle, but Alpine's exact pins die within weeks (old
  commits unbuildable, weekly bump bot) and Debian's durable 6.12 lacks FUSE io_uring: decision
  pending; dm-dust can be an out-of-tree module (not a blocker). Cold-run pie: tests 84 min vs
  compile ~40 min; of compile: kernel ~25%, QEMU ~20%, our C++ + abseil/sqlite ~30%, other tools
  ~20%. TLA+: 12.3 widened to a revalidation model; 12.4 inode lifetime and 12.5 identity models
  added, trace validation in each. Agents no longer edit docs/plan/ (conflicts came from that);
  the lanes already are separate clones merged by fetch + fast-forward.
- Prior-art survey saved (notes/formal-prior-art-2026-10-07.md): no published TLA+ for filesystems; adopted as 12.6-12.10 the recovery-idempotence invariant, the effect-point property, the identity model from the NFS/kernel rules, a coherence parameter, a tree-sequence backing crash model with Ferrite litmus tests, directory streams, and a test-generation spike; ACE workloads into Phase 11.
- russ (2026-10-07): Alpine series pins for kernel and tools (signed fetch, escape hatch, scheduled update job): spike first; systemd guest stays Debian; measure why e2e guests need 1 GB; CI trusts the test cache (weekly forced rerun), 'everything through Bazel' rule; no bigger machine.
- russ (2026-10-07): all CI inside GitHub Actions (schedules are fine); dm-dust dropped (error +
  table-switched flakey cover it); systemd guest from a released Debian/Ubuntu cloud image, not
  built; no weekly forced test rerun (Bazel's invalidation is sound; flake hunting is targeted).
  Needs russ: names_random_slow (100k names, 11 min x 3 fs, 2 GB guests): recommend removing, or
  keeping one ext4 run in the slow tier.
- russ (2026-10-07): dust dropped; distro kernels boot directly (the disk image is only the systemd guest's root fs); delete names_random_slow_test; Alpine spike positive (notes/alpine-spike-2026-10-07.md): Phase 24 written from its design.
- names_random_slow_test deleted (4882ff6, lane-2): the 100k-name run was the only 2 GB guest.
- 6.3b (lane-4, acad8bf): tool builds independent of the configuration (exec_file over
  configure_make, cfg = "exec"). Failing-first key counts: qemu 147 actions differing, mkfs.btrfs 18,
  mkfs.xfs 4, mke2fs/debugfs 2 each; after: a cold --config=asan tool build 1,090 s under load, then 0
  actions for ubsan (19 s) and plain (10 s). The exec toolchain is -c opt, so QEMU gets -UNDEBUG.
  Presubmit 129/129; asan boot_test and write_test_ext4 pass. tool_keys_test (544 s, second Bazel
  server) runs in a weekly/on-demand `tool-keys` CI job; the test finds bazel on PATH (prepare.sh
  installs Bazelisk under ~/.local/bin).
- 5.4a (lane-4, e9678d2): CI trusts Bazel's test-result cache (test.sh drops --cache_test_results=no;
  AGENTS.md "everything through Bazel" no longer promises a weekly forced rerun; the schedule is for
  OSV and tool-keys only). Not yet run on a real runner; russ pushes when ready.
- Phase 23 (23.6 held fd + 23.7 review fixes M1, M2, L1-L8) finished verification in lane-1
  (873caf1: presubmit 140/140, formal + large trace tests, asan small). One-in-eight
  copy_test_btrfs `immutable-ctime` off-by-one-second seen once; under review (dcfs-reviewer)
  before merge.
- russ (2026-10-07): `dcfs --help` useless under the installed name (Abseil's main-file heuristic;
  "No flags matched"): fix + --version dispatched to lane-4 (step-help). Man page from the README:
  Phase 15 item 26 pulled forward as 15.0 (pandoc pinned, genrule, render test).
- --help fix (lane-4, step-help): FlagsUsageConfig claims dcfs/ files; `dcfs --version` prints
  `dcfs 0.1.0-dev` (dcfs/version.h; Phase 15 stamps it). help_test (small, guest) failed first with
  "No flags matched". Note for Phase 15: Abseil exits 1 after --help (0 after --version); the
  wrapper's own parser should exit 0, and dcfs itself if it is cheap without bypassing ParseCommandLine.
- 15.0 (lane-4, 26a8857): dcfs(8) from the README: pandoc 3.12 pinned (@pandoc, http_archive,
  sha256), tools/man/extract_sections.py (named sections; a missing one fails the build; tables
  become definition lists; >3 columns fail), //man:dcfs.8, dcfs_8_test (pandoc round trip: sections,
  every flag and default, no table rules), flags_consistency_test (main.cc vs README flag table; no
  drift). groff -man -ww -z check deferred to the Phase 24 groff pin (TODO in man/BUILD.bazel).
  Failing first: '--attr_timeout_sec' not found / 'FLAGS' not found with the Flags section omitted.
- Phase 23 branch review (dcfs-reviewer, 873caf1): no high findings; merge after fixes. M-1 held fds
  unbounded (EMFILE takes out every open, not just the fallback): cap with a reserve. L-a write_fd
  replaced by a later O_APPEND open. L-b: with a shared fd outstanding, writable opens are re-checked
  only when flags changed through dcfs, so an out-of-band chattr +i/+a is bypassed (before the branch
  it was refused); orchestrator default: keep the pre-branch property (re-check every writable open
  without disk I/O), russ may relax. L-c idle test must assert the FORGET happened; L-d destroy_test
  measures zero reads after SIGTERM. L-e copy_test_btrfs immutable-ctime flake is a kernel gap: FUSE
  does not invalidate cached attrs after fileattr_set, so stat serves the pre-chattr ctime; test
  made deterministic + README limitation. Verified sound: held-fd lifetime (RELEASE precedes FORGET
  via igrab), EMFILE tri-state, L5 recovery sweep (no model change), L7 compare-and-set.
  Needs russ: (1) L-b: accept the always-re-check default or document the trade; (2) L-e kernel gap:
  a FUSE patch (fuse_invalidate_attr after a successful fileattr_set) is small and upstreamable, or
  dcfs sends notify_inval_inode (needs the notifier thread), or it stays a documented limitation.
- russ (2026-10-07): Phase 24 simplified: the pin is the Alpine stable branch only; Alpine advancing
  within the series must never fail a test or job; no alpine.lock, no exact-version escape hatch, no
  drift test (they only served each other). Resolution lives in the repository rule (not in the
  lockfile); CI fetches fresh, developers refetch with `bazel fetch --force`; the weekly schedule is
  the suite on a fresh fetch, not a version check.
- russ (2026-10-07): btrfs/FUSE ctime-after-chattr stays a documented limitation (no kernel patch
  now); the failing check is kept and disabled by name (`DISABLED_immutable-ctime`, a `disabled`
  helper in guest/lib.sh that reports its would-be verdict and never fails). Test-first check: yes
  for every behaviour change so far; exceptions on record: the EMFILE fallback test written after
  the code (verified to discriminate), destroy_test (measurement only), CI/config commits.
- russ (2026-10-07): a style guide, docs/style.md, extending the Google C++ Style Guide (code must
  conform); StatusBuilder helpers in dcfs/status.h (Abseil has no per-code builders at all); no
  nested namespaces except dcfs::syscalls, called as `syscalls::open`; syscalls.h holds one thin
  wrapper per manpage-documented call, compositions (`*_opath`, ReopenPathFd, ...) move to backing;
  enum class; clang-format always, enforced by a Bazel format test + opt-in pre-commit hook (7.6);
  IWYU via layering_check + misc-include-cleaner (7.7). Draft in lane-4 (step-style), revising.
- Style guide merged (ecacef4, docs/style.md, 455 lines; AGENTS.md "Follow docs/style.md"; review
  checklist points at it). Phase 25 (style convergence) written from its Appendix A: 25.1 errors,
  wrappers, enums after Phase 23 merges (same files); 25.2 flat namespaces (clashes: ParentOf,
  SetXattr, RemoveXattr in cache and backing); format/includes in 7.6/7.7.
- russ (2026-10-07): Google's style guides for every language (C++, Python, Shell, Markdown docs;
  Bazel per buildifier). Shell: bash for host scripts; guest scripts stay POSIX sh (busybox has no
  bash) as a documented deviation until Phase 24 can ship bash; shfmt + shellcheck join 7.6.
- russ (2026-10-07): no `backing` namespace exception; a wrapper class may hold the functions.
  syscalls wrappers keep libc names (`setxattr`), other functions CamelCase (`SetXattr`): no clash.
- russ (2026-10-07): Alpine moved earlier, ASAP. 24.1-24.3 dispatched to lane-2 (dcfs-investigator,
  from the spike branch); it rebases over the guest-memory step when that merges. CI: russ pushed
  (github.com/eatnumber1/dircachefs); first run on 95595f3 in progress (osv green, fast running);
  an earlier run on 8ed3851 failed in fast and osv (logs need auth; checking the new run instead).
- PAUSE for a reboot (russ, 2026-10-07; more memory afterwards). No new agents started. State:
  - lane-1 `step-23.6` @ 2941566 (clean): all review fixes done and verified (presubmit 144/144,
    formal + large/trace targets, asan small). A second read-only review of the fix commits
    (52890de..2941566) was in flight; if it is lost, rerun it with the same focus list (batched
    phase-1 tri-state and mid-batch failure; cap default = 0 at a 1,024 limit; GETFLAGS on a
    replaced/deleted shared fd and O_APPEND|O_TRUNC; yielded write_fd lifetime; which tests waited
    zero before the usleep fix; ASan 512 MB guests vs the 400 memory tag). Then merge.
  - lane-2 `step-24` @ ebc0ee5 (24.1 committed; 11 uncommitted files = 24.2 in progress): Alpine
    rule done, kernel step underway. After the reboot: `git stash` or commit WIP first, then resume.
  - lane-3 `measure-guest-mem` @ 3007586 (1 uncommitted file): per-class mem=/asan_mem= allowances
    committed; measurement/tabulation nearly done. Merge after lane-2 is told to rebase over it.
  - lane-4 idle at main.
  - GitHub: first real run on 95595f3 (fast in progress at 03:47 UTC, osv green); the earlier run on
    8ed3851 failed in fast + osv (logs need auth). Check the result after the reboot.
  - Next after the merges: 25.1 style convergence (after Phase 23), 24.2/24.3 continue, Phase 22
    cancellation, then Phase 7 with 7.6/7.7.
  - All Bazel servers die with the reboot: nothing to do; first commands restart them via sg kvm.
- Second review of the Phase 23 fix commits (52890de..2941566) landed before the reboot: nothing
  high or medium. Merge after: (a) a startup WARNING when max_held_fds computes to 0 (any soft
  limit <= 64Ki, e.g. a 65,536 container hard limit; russ may prefer a softer curve below 128Ki) and
  reworded cap warning; (b) nice-to-have: BeginAttrChanges tolerates NotFound per id; lib.sh checks
  the busybox applets it uses exist (the usleep gap; no vacuous passes on main, flakes only; the
  callers are listed in the review). Informational: FUSE inodes never carry S_APPEND, so F_SETFL
  can clear O_APPEND and async passthrough writes (io_uring/AIO) ignore append-only (future work /
  README). Batched phase 1 keeps tri-state per file, no model change; write_fd yield closes the old
  fd; ASan 512 MB guests do not oversubscribe at local_test_jobs=2. DISABLED_immutable-ctime is
  russ's decision (recorded above). FIRST AFTER THE REBOOT: send (a)+(b) to lane-1, then merge.
- 6.2 guest memory (lane-3, 0188e30): a sampler in guest/init prints a MEM line per test (no pass
  without it); plain peaks 22-256 MiB (bench_full 569), ASan 52-1509 (bench_full > 2001, OOM at
  2048: asan_mem=3072 is a guess, measure on the bigger machine); e2e default 1024 -> 256, unit
  256 -> 192, per-class mem=/asan_mem= (select on asan/ubsan), tla_trace_test too; resource tags
  1200 -> 356 (9 e2e guests fit where 2 did); DCFS_MEM=<MiB> override; OOM-killer lines and
  boot-time death are reported and FAIL the run (bug: ASan bench_smoke PASSED while dcfs was
  OOM-killed at 1024). Presubmit 140/140, large + ASan subsets green. Open: resource tags cannot
  follow the config, so ASan runs are under-reserved (README says fewer test jobs; a
  `test:asan --local_test_jobs=2` in .bazelrc would enforce it); Alpine (lane-2) must rebase over
  this (run-qemu.sh --mem defaults, macros' mem=).
- 24.1 + 24.2 committed in lane-2 `step-24` (ecb3b17 rule, d8037c5 kernel; rebased on 345827e), NOT
  merged yet: presubmit, asan boot_test and a review are still owed. Kernel: Alpine v3.24
  linux-virt 6.18.55 (43 MB apk, fetch 8-12 s, replaces the 690 s build; bc gone); per-test
  `modules=` with mkmodules.py (dep/softdep, built-in modules skipped); boot.sh accepts
  `6.18.*-virt`; `required_options.txt` + kernel_config_test (DM_DUST dropped); SBOM reads
  resolved.json (`linux-lts@6.18.55-r0`), osv.sh fetches the Alpine repos. Rule: shared
  `alpine_index` per branch (one snapshot for all packages), stdlib RSA, signature_test (tampered
  index/control/data refused; failing first 3/21). fast 86/86, write xfs/btrfs, bench_smoke_xfs,
  trace_power, nfs green. 24.3 WIP is `git stash@{0}` in lane-2 (untested). 24.3 findings: Alpine
  mke2fs lacks `-d tarball` (libarchive): the Debian rootfs needs a mkrootfs (extract + `mke2fs -d
  dir` + debugfs for the 4 non-root files; no host symlink following) — approve or redirect; no
  util-linux tools are actually used (drop it); Alpine qemu ships the same qboot.rom; removing
  glib/zlib/rules_foreign_cc needs a lockfile update.
- All agents stopped at 2026-10-07 ~04:30 UTC; safe to reboot. After: (1) cap-0 warning to
  lane-1 then merge Phase 23 (rebase over 6.2: the ASan unit guests' 512 MB fix meets asan_mem=384);
  (2) Alpine 24.3 in lane-2 after a reviewer pass on 24.1/24.2; (3) 25.1 style convergence;
  (4) the failed GitHub `fast` job (log from russ).
- russ (2026-10-07): mkrootfs approved (extract + `mke2fs -d dir` + debugfs for ownership; no host
  symlink following); `gh` will be installed after the reboot: use it to read the failed `fast` job.
- Back after the reboot (2026-10-07): 62 GB RAM, 4 cores; lane caps raised (memory=14000,
  local_test_jobs=3, Xmx3g; jobs=2 stays, CPU is the limit). GitHub run on 95595f3: `fast` failed to
  BUILD (61 tests) because xfsprogs and btrfs-progs need `uuid/uuid.h`, present on the dev host
  (libuuid-dev) but not on the runner: a hermeticity leak in the util-linux wiring; KVM was available
  (DCFS_CI_KVM=1); 22 tests passed. Decision: 24.3 (Alpine tools) removes those builds and is the
  fix; CI stays red until it merges. Dispatched: lane-1 Phase 23 final fixes (cap-0 warning,
  BeginAttrChanges NotFound, lib.sh applet check, rebase over 6.2 with measured asan_mem); reviewer
  on 24.1/24.2 (read via git, not the working tree); lane-2 24.3 (mkrootfs approved, util-linux
  dropped).
- Alpine 24.1/24.2 review (dcfs-reviewer): verification chain correct (index signature over the
  raw stream, per-apk signature, index C: checksum, datahash; nothing extracted before all pass;
  full PKCS#1 encoding compared; unknown key fails closed; lockfile untouched; one index snapshot
  for all packages). Merge after: M1 old-index 404 gets a clear message + refetch hint; L1/L2 tar
  extraction hardening (hard links, device nodes, escaping symlinks); L3 verifier tests (malformed
  padding, RSA256 path); L4 bind index to branch (regex + DESCRIPTION); L5 built-in-only options
  (VIRTIO_MMIO, SERIAL_8250); L6 AGENTS.md/style.md third_party rule gets the Alpine exception;
  L7 /proc/sys/kernel/modprobe hook naming the undeclared module; L8 no hard-coded external paths.
  Sent to lane-2 for after 24.3.
- russ (2026-10-07): more lanes now that memory is plentiful (CPU, 4 cores, is the limit). 7.1 pinned
  clang toolchain started in lane-4 (independent of the open branches). 12.3 revalidation model
  waits for Phase 23 to merge (it maps the changed OPEN re-check); 25.1, Phase 22 and the guest
  helper dedupe also wait for it (same files).
- 24.3 + review fixes done in lane-2 (step-24 @ 53ea1fb, 12 commits, +3860/-5730): QEMU 11.0.3,
  e2fsprogs 1.47.4(+extra), xfsprogs 7.0.1, btrfs-progs 6.17.1, busybox-static 1.37.0 from Alpine
  v3.24 via musl-loader wrappers; mkrootfs.py replaces `mke2fs -d tarball` (image tree identical:
  8715 paths, debugfs ls/rdump); removed rules_foreign_cc, glib, zlib, util-linux, urcu, inih,
  libarchive, exec_file.bzl, tool_identity/keys tests + tool-keys CI job, device-allowlist test;
  all Alpine fetches together 22 s / 76 MB vs ~30 min of compiles. Review items M1, L1-L8 and nits
  closed (apk downloads moved to Python urllib for a testable 404 message: reviewer to weigh the
  Bazel-downloader trade). fast 84/84, presubmit 144/145 (readdir_boundary warm-listing 3.2 s vs
  2.5 s budget under load; 3/3 alone), asan boot + dcfs units 33/33. AGENTS.md gains the Alpine
  exception sentence (russ to see). Final review (24.3 + fixes) in flight; then merge, then push.
- russ (2026-10-07): downloads stay in Bazel. ae28089 (apk fetches via Python urllib, for a testable
  404 message) is being replaced by `rctx.download(allow_fail = True)` + the message in Starlark;
  no test for the message. Rule for the future: a repository rule fetches only through Bazel's
  downloader (repository cache, distdir, proxy/netrc, retries); verification may run a tool.
- Final Alpine review (24.3 + fixes, tip bd92bef): merge after N1 wrapper fails hard when root/ is
  missing (today it would exec the HOST's ld-musl and binaries), ld-musl path file + unset
  LD_PRELOAD, TCG start in tools_test; N2 mkrootfs hard links must not follow symlinks; N3 download
  message hedged (offline != stale index). Should: N5 SOURCE_DATE_EPOCH=0 clamps every inode time
  to 0 (keep package mtimes with a fixed later epoch); N6 committed tar-vs-image invariant; N8/N9
  stale names in sbom_test, README, BUILD comments, style.md. Verified: exec keeps PID (signals and
  exit status propagate), musl error text matching not weaker, QEMU 11.0.3 has every flag/device
  we use, lockfile diff real, no removed target referenced. N4 (pre-existing, medium):
  readdir_boundary `warm-listing-is-not-quadratic` is an absolute 2.5 s wall-clock budget: will
  flake under load and fail under TCG; next step: count work (rows read / SQLite steps) in
  dir_cache_fs_test, or a 4N-vs-N CPU-tick ratio. N7: when Phase 23 and Alpine meet, copy_test's
  casefold check needs the unicode module declared if Alpine builds CONFIG_UNICODE=m.
- Phase 23 merged (a64e666; 26 commits replayed without docs/plan hunks, tree identical to the
  tested tip 1c73972): presubmit 144/144, large/trace 44/44, asan //dcfs/... 32/32. Final round:
  cap reserve = half the soft limit within 16Ki..64Ki (65,536 -> 32,768; 1M -> 983,040), cap 0
  logged at startup (LogSink fake); BeginAttrChanges skips a missing row; lib.sh require_commands
  (nfs.sh names its own: the Debian chroot lacks sed); dir_cache_fs_test asan_mem=448 from MEM
  lines; destroy_test mem=832 (the 256 default let the kernel reclaim the pinned inodes).
  Unexplained: ~1,000 FORGETs of 100,010 held inodes at 832 MiB, no memory pressure. Unblocked:
  12.3 (lane-3), 25.1 (lane-1), N4 readdir timing (after 25.1: same test file).
- Dispatched: 12.3 revalidation model (lane-3, dcfs-protocol: formal/reval.tla, known-bug variants
  for the pre-23 reuse and the flags_changed-only re-check, trace mapping of OPEN/RELEASE/IOCTL/
  SETATTR); 25.1 style convergence (lane-1). Four lanes busy: 1 style, 2 Alpine fixes, 3 model,
  4 clang. Load to watch; guest timeouts flake first.
- Phase 24 merged (d0b2cc2, 18 commits, +4173/-5791): fast 83/83, presubmit 145/145 on the
  rebased tip. Final fixes: wrapper exits 127 outside its tree, empty ld-musl path file (strace:
  no host /lib probing), unset LD_PRELOAD, TCG start tested; mkrootfs keeps package mtimes
  (SOURCE_DATE_EPOCH = snapshot time), refuses traversal/non-UTF-8/newer-than-epoch; committed
  rootfs_invariant_test (tar vs image, every member); hedged download messages; stale names
  fixed. Phase 23 interactions: CONFIG_UNICODE=y (no module needed), GUEST_COMMANDS all present.
  OBSERVATION (not fixed): copy_test_ext4's serial log shows a kernel oops (NULL deref in utf8byte
  via utf8_casefold <- ext4fs_dirhash <- ext4_readdir) during the online-casefold check on Alpine's
  6.18.55; the test still passes; unknown whether 7.2.9 oopsed too. Needs an investigator: is
  `testutil ext4-casefold` enabling casefold on a filesystem without an encoding (a kernel bug
  worth reporting, or a test that should mkfs -O casefold), and should the harness fail a run on
  any oops/BUG in the serial log (it should). Also pending: N4 readdir timing budget, the
  destroy_test FORGET count.
- Casefold oops (lane-2, f45ef20): run-qemu.sh now FAILS a run on BUG:/Oops/kernel BUG at/WARNING:
  CPU:/Call Trace:/Kernel panic in the serial log, and guest/init dumps matching dmesg lines as
  KERNEL-OOPS: (console loglevel 3 hides them); run_qemu_verdict_test with canned logs (failing
  first: a NULL-deref log did not fail the run); absl's userspace "WARNING: All log messages..."
  false positive fixed. No other test's serial log (228 checked) has an oops or warning. The test
  now creates its disk with `-O casefold -E encoding=utf8` (disk spec gained an ext4-only mke2fs
  options field); testutil's `ext4-casefold` superblock tuning removed.
  Needs russ: KERNEL BUG CANDIDATE for linux-ext4. EXT4_IOC_SET_TUNE_SB_PARAM with
  EDIT_FEATURES enabling casefold on a mounted ext4 writes the feature bit and s_encoding on
  disk but never loads sb->s_encoding (ext4_encoding_init runs only at mount); ext4_ioctl_setflags
  then accepts +F (checks only the feature bit) and the next readdir calls utf8_casefold(NULL):
  NULL deref in utf8nlookup <- utf8byte <- utf8_casefold <- ext4fs_dirhash <- ext4_readdir. Root +
  writable mount + CONFIG_UNICODE=y; reproducer is the removed cmd_ext4_casefold at f45ef20^.
  Fix options: load the encoding in the ioctl, or check s_encoding (not the feature bit) in
  setflags/dirhash, or refuse enabling casefold while mounted. Present in v6.18, v7.2, 7.3-rc.
- russ (2026-10-07): keep a DISABLED_ test that fails because of the ext4 casefold-tune kernel bug.
  Dispatched (lane-2, step-oops-2): `casefold_tune_oops_test`, its own guest, reproducer restored as
  `testutil ext4-tune-casefold`, `DISABLED_casefold-tune-online-oops` under the `disabled` helper,
  with an explicit per-test `kernel_failure = "expected"` opt-in so the harness's oops rule stays
  strict everywhere else.
- DISABLED reproducer merged (3d4c32f): casefold_tune_oops_test (small, own guest, no dcfs) reports
  `DISABLED_casefold-tune-online-oops would FAIL (kernel: BUG: kernel NULL pointer dereference,
  address: 0000000000000018)` on 6.18.55; `kernel_failure = "expected"` opt-in, refused for unit
  mode or any other script; an oops is tolerated only if the guest itself reported it as would
  FAIL; warnings/panics still fail. Verdict test +8 cases. fast 85/85.
- Dispatched (lane-2, dcfs-investigator): why destroy_test saw ~1,000 FORGETs of 100,010 pinned
  inodes at 832 MiB; hypothesis order: opath-hold-tree holds fewer than counted; the cap; a
  non-final FORGET (nlookup < count) wrongly dropping the held fd (would be a dcfs bug, test
  first); inode-cache shrinking; fd counting. Threshold (>= 90%) to be tightened either way.
- 12.3 done in lane-3 (step-12.3, 4 commits, +2077/-36): formal/reval.tla (OpenExact, HeldFlagsLegit/
  HeldModeLegit, WritesUseAWritableFd, WriteFdHeld, WriteFdKeepsOffsets, DirCacheNeverWrong,
  liveness CachedDecisionsConverge); known bugs: no-recheck (pre-23), recheck-if-changed with
  OutOfBand (bypass; passes with exclusive access), casefold, no write_fd, write_fd last-writer;
  formal/limitations/ for out-of-band mode/casefold/stale; MC_reval 69k states 11-23 s, _oob 443k
  44-72 s, _liveness 68k (medium); trace mapping: FileOpened/FileReleased events, ioctl_arg,
  setattr to_set, per-file DCFS-REVAL traces, `oob` lines via NoteOutOfBand (harness only; guest
  recorder gap documented). Failing first: harness trace rejected by the no-recheck variant
  ("explains 3 of 10 events"). Formal 44/44, fast 101/101. Recorder unit tests written after the
  code (deviation). Under review (dcfs-reviewer).
- 12.3 review: merge as is (model faithful; matches the code at the tip; variants reproduce the
  Phase 23 counterexamples; exclusive config passes for the right reason; production footprint
  negligible, no behaviour change). Follow-ups before merge (sent): F3 invariant that wfd exists
  only beside writers over a read-only shared fd + BugWriteFdNeverDropped variant; F1 the liveness
  config is trivial under exclusive access; F5 harness test for a refused SETFLAGS and FSSETXATTR.
  F4 (plan: coherence parameter not implemented; Done note) is the orchestrator's at merge.
- 25.1 done in lane-1 (step-25.1, 7 commits + docs): 8 *ErrorBuilder() helpers, 45 sites converted,
  4 `; ;` texts; C5/C6/C8/C14/C16; syscalls.h thin wrappers only (opath helpers, ReopenFd, FsUid/
  FsGid, GetInodeGeneration -> backing.cc; new getxattr/listxattr/setxattr/removexattr/fchmodat/
  utimensat wrappers; setgroups; LogOpenFlags in log_open_flags.h as a hidden friend, with a test;
  backing_fault_test with --wrap on getxattr/listxattr); enum class x4 (RET_CHECK streams the
  underlying value); ASSERT_OK_AND_ASSIGN once; pjdfstest README. presubmit 146/146, asan small
  26/26. Finishing: getrlimit/setrlimit/flock wrappers (C10 to zero), readlinkat/getgroups loops,
  setattr.sh comments; then review (fresh dcfs-reviewer), then merge and 25.2.
- GitHub run 37582181373 (b41cd17): osv, fast (Test 153 s), presubmit (Test 305 s) green; full's
  "Test (all tiers)" step FAILED after 2,110 s (ASan step continues on error, still running).
  Orchestrator error: reported the step as passed when it had merely finished. Failing tests to be
  read from the job log / test-log artifact once the job completes; candidates: a large-tier test
  or a timing budget under runner load (readdir_boundary's 2.5 s wall clock, N4).
- russ (2026-10-07): warnings "all over" during compilation, and ld.gold's "wildcard match appears in
  both version 'libnuma_1.1' and 'libnuma_1.2'": -Werror was meant to mean every warning is
  addressed. Dispatched a warnings audit (lane-5, dcfs-investigator): catalogue every compile and
  link warning on a clean build (plain + asan, no disk cache), zero for our code incl.
  `-Wl,--fatal-warnings`, our flags never reach externals, why dcfs links libnuma at all (drop or
  patch numactl's version script), a test that our targets carry -Werror/--fatal-warnings;
  coordinates with 7.1 (clang/lld in lane-4). Five lanes busy.
- russ (2026-10-07): the "only backing.cc calls syscalls::" rule was about spin-ups; relaxed to
  "syscalls that can reach the backing filesystem (a backing fd, handle or name) only in
  backing.cc; process-local syscalls (rlimits, credentials, /proc, the cache db file, mount tables)
  may call syscalls:: anywhere". style.md 1.7 updated by the 25.1 agent; C9 dropped.
- GitHub run 37582181373 complete: osv, fast, presubmit green; full: plain tiers 171/172 (only
  destroy_test: held 69,577 of 100,000 after a 489 s write phase on a contended 4-vCPU runner,
  54,356 after drop_caches, no memory pressure; shutdown still read nothing), ASan tiers 172/172
  (destroy_test passed there in 224 s). First green ASan suite on GitHub. The held-fd loss scales
  with write-phase duration, not memory: hypothesis = something periodic (the sync point
  reconciling written_ / releasing held fds, or a FUSE dentry invalidation) drops held fds over
  time; sent to the lane-2 investigation with a slowed-writer reproduction plan.
- russ (2026-10-07): firm rule: every syscall goes through syscalls.h (tests included; errors
  become Status there). 25.1b queued behind 25.1 in lane-1: raw_syscalls_test + conversion.
  Needs russ: port tools/fhtest.c and tools/testutil.c (guest C programs) to C++ so the rule has
  no exception? (recommended; until then they are the documented exception).
- russ (2026-10-07): tools/fhtest.c (copied from fuse-generation-qemu) and tools/testutil.c stay C,
  excluded from the syscalls rule; third-party code is never rewritten.
- russ (2026-10-07): style rules as AST checks (yes: 7.5b clang-query matchers, replaces the regex
  test); a small hermetic ML model as a style gate (no: nondeterministic, slow, weak where it
  matters; judgement rules stay with the merge review).
- russ (2026-10-07): the tiny-model style judge reconsidered at the right size (one rule, one
  chunk, sub-1B, deterministic): a measured spike, 7.5c, after the matchers.
- russ (2026-10-07): Phase 26 "Bumpers": 26.1 gate self-checks (yes), 26.2 runtime invariant checks
  in test builds (yes), 26.3 golden per-op syscall traces with strace rather than in-process
  recording (yes), 26.4 ratchets on deterministic counts only (yes-ish, brittleness), 26.5 limited
  mutation testing (yes), 26.6 fast in-process fault enumeration (proposed; needs yes), reference-
  model differential testing dropped (pjdfstest/xfstests/fsstress + TLA+ test generation cover it).
- 12.3 merged (2c3d79c, 5 commits). Uncached: formal 43/45 (large_test and nolock_test hit 900 s
  under load 20; their inputs are unchanged from main where they pass in 460/322 s: rerun at the
  next quiet moment / sync point), harness trace test 49/49, guest trace tests 4/4, fast 104/104.
- russ (2026-10-07): second bumper set approved: 26.7 visibility split, 26.8 banned symbols, 26.9
  deps golden, 26.10 injected clock (two call sites: sync interval, relatime; the latter a raw
  clock_gettime), 26.11 strong types (after 25.2), 26.12 reproducible build (after 7.1), 26.13
  repository-shape tests; schema golden/migrations dropped (no users yet). 26.1 dispatched (lane-3,
  dcfs-mechanical): inventory of gates, missing self-checks, README table.
- russ (2026-10-07): 26.10 uses absl::Clock + absl::SimulatedClock (both in the pinned Abseil),
  not a clock type of our own; orchestrator had claimed Abseil lacked one.
- 25.1 + 25.1b done in lane-1 (step-25.1 @ 2a7fec6, rebased): fast 87/87, presubmit 147/148
  (readdir_boundary warm-listing 2.98 s vs 2.5 s under load; passes on rerun: N4 again), asan
  small 25/25. 25.1b: raw_syscalls_test (manual) finds 300 sites in 20 files (dir_cache_fs_test
  115, backing_test 52, bench 54, ...; false positives: local functions named open/read in
  dir_cache_fs.cc:1335,1346 and migrate.cc:57; clock_gettime at dir_cache_fs.cc:1496 is real and
  26.10's). Conversion = 25.1c (dcfs-mechanical, after merge). Fresh dcfs-reviewer on the branch.
- russ (2026-10-07): 26.6 approved as a try; the report must state the runtime it adds (dropped if
  not seconds).
- 26.1 escalated (mechanical -> implementer after two rounds): round 1 ran the self-checks by hand
  (no Bazel targets, no README); round 2 wired targets but every self-check tested a COPY of the
  gate's logic ("mirrors the logic of ..."), not the gate. Implementer rewrites them to execute the
  real gate code on known-bad input (lib.sh, pjdfstest.sh refactored to a function, the man tests,
  busybox_test.sh with a fake busybox, rootfs invariant with a tampered synthetic tar), bash +
  pipefail + 2-space, README table complete.
- destroy_test diagnosed (lane-2, ca8d196): the guest kernel RECLAIMED inodes: ~9 KiB per written
  file, the 832 MiB guest hit its low watermark at ~73k files and evicted; held fds rose
  monotonically until then (no sync-point or FORGET loss; hypotheses 1-3 out). MemAvailable counts
  reclaimable cache, so the MEM-line sizing rule hid it (peak_used 349 of 781 MiB). Fix: mem=1536
  (needs ~1030), thresholds exact (100% held, before == after the drop), a reclaim-scans check.
  Generalizing before merge: reclaim counters in every MEM line + run-qemu WARNING +
  `require_no_reclaim` in idle/release_leak/destroy; guest timeout from Bazel's TEST_TIMEOUT.
  Lesson for 6.2's rule: a sizing run must show reclaim_scans=0.
- 26.1 merged (d5b6607): six self-checks that execute the real gates (lib.sh require_commands;
  pjdfstest's sanity check refactored into guest/pjdfstest_lib.sh and fed empty/garbled/sabotaged
  prove output; the two man tests as subprocesses; busybox_test.sh over a fake busybox; the
  rootfs invariant's header_problems over a tampered synthetic tar), each shown failing with its
  gate disabled; README "Gates and their self-checks" table; the old copies moved out of guest/
  (the initramfs glob would have shipped them). fast 110/110, pjdfstest ext4 shards green.
- 25.1 review (fresh dcfs-reviewer): behaviour unchanged (all 46 error sites same code, no lost
  errno payload, moved helpers identical, wrappers' branches identical); merge after: style.md 1.8
  must name the lower layers (file_handle.cc, device_id.cc, startup in main.cc) and design.md's
  layering bullet updated; the fifth `; ;` site (main.cc:341); the cache-dir stat error lost its
  path; C17 wording; py_test timeouts; the manual raw_syscalls_test becomes a per-file ratchet now.
  For 25.1c: scanner gaps (names list; digit separators; std:: calls), message-text changes to
  list in the merge note, ioctl request type unsigned long, source-location assertion, doc claims.
- Dispatched 26.8 banned symbols (nm over main_static), 26.9 dependency golden (genquery, shared
  with the SBOM), 26.13 repository-shape tests + CI commit-subject check (lane-3, implementer).
- 26.8/26.9/26.13 merged (lane-3): banned_symbols_test (nm -u over the linked objects via an
  aspect; deny list with per-symbol rules; library-origin allows with reasons; first-party
  strerror at main.cc:246-274, dir_cache_fs.cc:113, fuse_request.cc:196 allowed as known
  violations -> 25.1c; mounts_below.cc:63 realpath allowed as startup-only); shipped_deps_test
  (golden dcfs/shipped_deps.txt = abseil, gloop, libfuse, liburing, numactl, sqlite3; reuses the
  //dcfs:linked_deps genquery the SBOM uses); repo_shape_test (third_party READMEs, guest scripts
  referenced, DISABLED_ names in README Limitations; tagged external: Bazel cannot declare "every
  directory" as an input) + .github/ci/commit_subjects.sh in the fast job. Tree fixes: pjdfstest
  README, two DISABLED_ names added to README Limitations. Each with a self-check. fast 117/117.
- Dispatched 26.3 strace golden traces (lane-3, implementer): Alpine strace into the guest,
  strace_lib.sh (`strace_op`, reducer to backing/cache/proc/fuse kinds), goldens per operation
  and cache state explained by design.md (a discrepancy stops that check and is reported),
  self-check over a canned trace, README section.
- destroy_test fix merged (a5f3227, lane-2): mem=1536 (needs ~1030; GitHub runners have 16 GB),
  exact thresholds (100,010 held after write, with the holder, after the drop), reclaim counters
  on every MEM line + run-qemu WARNING + `require_no_reclaim` in destroy/idle/release_leak, guest
  timeout = TEST_TIMEOUT - 60 s (1800 only when unset; TIMEOUT= wins); verdict test covers both.
  destroy_test 1140 s under load, reclaim_scans=0. CI's only red test should be green next push.
- Dispatched 12.4 inode lifetime model (lane-2, dcfs-protocol): formal/lifetime.tla (nlookup,
  rows/removed records/held fds/open fds, unlink-while-open, rename-over, FORGET batching,
  DESTROY, crash + sweep), six invariants, four known-bug variants (incl. the non-final-FORGET
  hypothesis), trace mapping of existing events. Five lanes busy: 1 style 25.1, 2 lifetime
  model, 3 strace goldens, 4 clang, 5 warnings.
- 26.3 merged (44da625, lane-3): see the phase file's Done note; 26.4 ratchets dispatched to the
  same agent (budgets file, SQLite statement/transaction counts if a cheap deterministic counter
  exists, ASan must give the same counts).
- 7.1 done on its branch (lane-4): toolchains_llvm 1.11.1 + LLVM 22.1.8 (23.1.2's lld needs ICU 70:
  the release binaries depend on host libc/libstdc++/zlib/libxml2/ICU; no sysroot: glibc headers
  and static libs are the host's; wrapper needs host mktemp/realpath/rm) -> 7.1b follow-up: a
  Debian sysroot, or Alpine's clang/lld through the musl loader (the Phase 24 pattern). Static
  main_static with lld + libc++; --dynamic_mode=off gone; clang found a missing <thread>, an
  unused private field, libstdc++ mangled names in the --wrap fakes; patches: liburing probes
  (host ld leak), toolchains_llvm cc_wrapper response-file bug (QEMU link; may be moot after the
  rebase), numactl version script dropped (its `local: *` hid clang's static ASan malloc: 10 ASan
  tests failed "bad-free"). presubmit 145/145, asan small 24/24, ubsan main_static builds.
  Rebasing over Alpine (its base predates it), then review.
- 25.1 review fixes done (lane-1, 44b35e2): fast 113/113, presubmit+formal 179/179, asan small
  25/25; raw_syscalls_test now an untagged per-file ratchet (baseline 301 sites in 21 files; a
  rising count fails). Merge pending: rebase conflicts with 26.13's pjdfstest README; handed to
  the lane agent with a rerun against the new 26.x gates.
- 26.4 merged (lane-3): budgets per op (backing/procfd/sync/sql_stmts/sql_txns) in
  guest/syscall_budgets.txt, `strace_budget` after each op, SQLite counts from the daemon's VLOG(2)
  step lines (no production change), identical under ASan (reducer skips fd-less mmap). Budgets:
  cold-lookup 11/2/0/51/1, warm-lookup 0/0/0/4/0, warm-readdir 0/0/0/31/0, create 21/5/2/136/10,
  unlink 12/1/1/52/4, mkdir 10/2/0/63/3, rename 14/1/1/82/4, write 8/4/1/84/6, fsync 11/3/2/86/8,
  chmod 19/5/1/47/3, forget-written 2/0/0/2/0. FINDINGS for an investigator (Phase 10's readdir
  27x and the create path): 31 statements per warm listing; 136 statements / 10 transactions /
  2 syncs per create.
- Dispatched (lane-3, dcfs-investigator, report only): why a warm readdir steps 31 statements and
  a create 136/10 transactions/2 fsyncs; per-entry vs per-call grouping, slope with a 1,000-entry
  directory, SQLite settings, a ranked reduction plan with expected counts and the protocol
  transitions each change touches; what the 27x readdir slowdown is made of. Steps follow after
  25.1 merges (same files).
- SQLite-count investigation (lane-3, report only; stopped on two auto-mode denials: temporary
  edits to strace_lib.sh/syscall_traces.sh, and `ls bazel-testlogs` -> Needs russ). Findings:
  readdir is 8x (916 vs 114 ms, 10k entries), not 27x (stale, pre-v3-indexes); counts are steps
  (a hit = 2); warm readdir = per-call completeness (2) + unknown probe (1) + ./.. handling, per
  entry 1 row step + GetAttr (2); READDIRPLUS pages fetch 64 rows for ~25 entries per reply;
  create's 2 fsyncs are its two durable phase-1 WAL commits; each transaction costs 2 steps, a
  kSync one 6 (synchronous FULL/NORMAL flips). Plan, ranked: (1) join attrs into ListDir (-2N, no
  protocol change); (2) LIMIT = reply budget; (3) ReadOne stops after the first row (every
  sql_stmts budget drops); (4) merge the completeness statements; (5) merge create's phase-3
  fills (touches the fill guards: model + trace validation; needs the real breakdown); (6)
  profile per-row C++ (DeviceId::Parse per GetAttr, 21 columns) and the daemon's own CPU (bench
  does not record it). Steps after 25.1 merges.
- 25.1 merged (2f5515e, 13 commits, 55 files +2122/-1293): fast 122/122, presubmit+formal 188/188
  on the rebased tip; two stale strerror allow lines dropped from banned_symbols.txt. Dispatched:
  25.1c (lane-1: scanner gaps, 301 sites to syscalls::, false positives renamed, fuse_request.cc
  strerror, L2-L6 doc/typing items), 26.2 (lane-3, dcfs-protocol: --//dcfs:check_invariants
  build; the no-transaction-across-backing-syscall guard first; request-end tri-state/dirty-set/
  lookup-count/held-fd checks; fault tests first; fast+presubmit run the checking build).
- Warnings audit done (lane-5, step-warnings, holds for 7.1): 6 kinds / ~82 sites, none from our
  code; sources: numactl version script (13 links x 7 gold lines), static-glibc getaddrinfo
  (googletest, numactl) and dlopen (SQLite, libfuse) notes, liburing -Wunused-parameter, a Bazel
  rule/file name clash in third_party/linux, 2 GCC -Warray-bounds in abseil under ASan. Fixed:
  -Werror/-Wimplicit-fallthrough/-Wno-sign-compare scoped to our sources via per_file_copt (0 of
  580 external compiles carry them), global -Wl,--fatal-warnings, skylib analysis tests asserting
  both on our targets (fail first), patches/flags for the rest (most moot under lld: pruned at the
  rebase over 7.1). Needs russ: libnuma + liburing are shipped only for libfuse's fuse_uring.c
  (dcfs never passes -o io_uring): turn HAVE_URING off in the libfuse overlay (drops both
  dependencies from the shipped binary and the SBOM) or keep FUSE-over-io_uring for later?
- 12.4 done in lane-2 (step-12.4, 4 commits, +2321/-31): formal/lifetime.tla with NodeidStable/
  ReferencedServed, NotRetiredWhileReferenced, HeldOnlyWhileWritten/WrittenUntilLastForget,
  ForgetKnown, LookupsExact/NothingLeaks, KernelForgotAfterCrash/UnnamedRowsSwept; four known-bug
  variants reproduce (non-final FORGET drops the held fd; removed record retired while referenced;
  pre-23.7 tmpfile row; FORGET_MULTI counted once); events LifetimeChanged/Destroyed; harness
  nodeid traces (32 valid); MC_lifetime 114k states 13 s. THREE FINDINGS in the code
  (formal/findings/, tests expect the violation): (a) a stub's synthetic nodeid is reused while
  the kernel still holds it (SetRefused takes MAX+1 of live stubs; an out-of-band relisting) ->
  a live nodeid names another object instead of ESTALE; (b) a crash between unlink's syscall
  and phase 3 leaves a row of a freed object the sweep keeps (nlink column not 0); (c) DESTROY
  with a tmpfile open, then a clean start: no sweep, the row stays. Under review (reviewer
  verifies each finding against the code); fixes follow as 12.4b, test first.
- 7.1 rebased over Alpine (lane-4, 58049eb): presubmit 186/186 (incl. toolchain_test), asan
  //dcfs/... all sizes 32/32, ubsan //... builds, hermeticity build with host compilers blocked
  passes; cold //... 19.5 min on 2 shared cores (was ~40), main_static 10 min incl. refetch; the
  llvm cc_wrapper response-file patch dropped (README keeps the upstream note); numactl patch
  stays (dependency under review); banned_symbols resolves llvm-nm via runfiles, allow lines
  adjusted for clang's codegen (fprintf: abseil str_format, libnuma). Under review (fresh
  dcfs-reviewer): hermeticity completeness, static-link flags, the --wrap mangled names, patches.
- 12.4 review: merge after M1/M2. Corrections: finding (c)'s path is a READ-ONLY open of an
  unlinked file across a clean shutdown (a tmpfile keeps the flag unclean: writable open = durably
  dirty through FinishRun); finding (a) is broader: ForgetNegativeDentries on ANY detected
  out-of-band change in the parent deletes all its stubs, the relisting re-mints MAX+1 in listing
  order, two boundaries can swap nodeids; the kernel marks the old inode bad (EIO, not ESTALE);
  NFS handles safe (generations); the unit test StubsLiveWithTheirRefusals asserts the reuse on
  purpose. Finding (b) confirmed (BeginRemove leaves nlink; sweep matches nlink=0 only; rmdir too).
  12.4b (dcfs-protocol, after merge): (a) persisted monotonic stub high-water mark (negative
  int64s: no AUTOINCREMENT) + keep stub rows when the dentry is merely forgotten (unknown), so a
  re-refusal keeps id and generation (else d_invalidate detaches a mount on the stub); (b) at an
  unclean start probe dirty-set inodes without a present dentry by handle, delete on ESTALE/
  nlink 0, else refresh (directories included); (c) sweep at every start with a partial index on
  nlink = 0 (or Destroy retires rows of open objects with no link). Each: finding config moves
  into the real model; failing-first tests.
- 25.1c done (lane-1, step-25.1c @ caa9cf0, 7 commits): scanner gaps closed (digit separators,
  std::/(name)(/>name( forms; `->name(` still ignored), 320 sites converted (production 5: two
  lambdas renamed open_node/read_row, clock_gettime and realpath wrappers; fuse_request.cc
  strerror gone; tests 247; bench 66), new wrappers (mount, umount2, mkdtemp, mkstemp, fcntl,
  realpath, clock_gettime, nanosleep, getpid, sync) and a testonly syscalls_process library
  (fork, execv, waitpid, kill, dup2, _exit) so execv stays out of main_static; bench's readdir
  walks use getdents64; baseline empty: raw_syscalls_test enforces. fast 123/123, presubmit+
  formal 189/189, asan //dcfs/... 34/34. Targeted review (production sites, testonly wiring,
  assertion fidelity) before merge.
- 12.4 merged (def2352): model fixes from review (clean flag depends on writable opens; finding
  (c) now the read-only-open path; destroy-with-opens config; T_LifeRestart taken; a recorder gap
  fixed test-first: run lines did not update the directory's last state). formal 63/63 (lifetime
  318k states 39 s). 12.4b dispatched (lane-2): stub id high-water mark + keep stub rows on a mere
  forget; unclean-start probe of dirty inodes without a present dentry; sweep at every start.
- 25.1c review: faithful, no weakened assertions; fix before merge: the realpath allow line now
  covers the wrapper (ban the mangled wrapper symbol, allow only from mounts_below); std::
  filesystem/ifstream back doors in tests and bench converted and added to the scanner; doc
  statements; `pause` wrapped; `remove` dropped from the list (std::remove algorithm).
- 7.1 review: switch sound (641 compiles/152 links on the pinned LLVM; main_static static, no
  libstdc++; --wrap names verified; 22.1.8 right: 23.1.2's lld NEEDs libicu*.so.70). Fix before
  merge: ASan lost its C++ runtime (the C driver links asan.a without asan_cxx.a: new/delete
  mismatches unreported, a regression vs GCC's libasan) -> `-fsanitize-link-c++-runtime` for asan
  and ubsan + a guest self-check; banned_symbols tests incompatible under sanitizer configs
  (interceptors, -O1 printf folding, dynamic binary); `--dynamic_mode=off` restored with the real
  reason (lld rejects libfuse's .symver in a -shared link; 16 useless .so links otherwise); host
  dependencies documented fully (liburing probes still use host GCC's crt/libgcc -> -rtlib=
  compiler-rt; -L/usr/lib/gcc on every link; UAPI headers from the host; ld.lld needs libxml2 ->
  ICU 74/liblzma; /bin/bash); one numactl override (7.1's; lane-5 drops its two patches); SBOM
  gains llvm-project under shipped; toolchain_test self-check; allow-line reasons; READMEs with
  Pin sections; BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1; CI repository-cache keying (1.94 GB tarball
  vs the 10 GB quota). Lane-5 told.
- 26.2 done (lane-3, step-26.2, 3 commits, +1978/-24): Context::checks hook (no-op in production,
  one empty virtual per backing call and request); checks: no transaction / busy statement at any
  backing call; at request end (changed rows + named inodes): tri-state, identity (only root has
  generation 0), dirty set (durable => dirty row; any==false => empty), writable-open, lookup
  counts, held fds (= held written_ entries, <= cap), removed records; whole database at StartRun
  and after DESTROY; LOG(FATAL) + DCFS-INVARIANT-VIOLATION on the console, run-qemu fails the run;
  16 death tests failed first; small/medium tiers boot :initramfs_checked (no Starlark flag: it
  would reconfigure everything), large and the measuring tests (syscall_traces, memory) the plain
  binary; status-returning variants for 26.6. NO VIOLATION in any existing test. Two checklist
  sentences were wrong as written (dirty set != unknown rows; a held fd can coexist with a later
  writable open) and are checked one-way per design.md. Under review.
- 25.1c merged (752c573, 8 commits): raw_syscalls_test enforces outright (baseline deleted);
  realpath gate fixed (mangled wrapper symbol banned, allowed only from mounts_below); std::
  filesystem/ifstream back doors converted (testonly/files.h: ListDirectory, ListTree, RemoveAll,
  FileSize, ReadFileToString) and added to the scanner; `pause` wrapped; `remove` dropped from
  the names (std::remove algorithm). fast 138/138, presubmit+formal 207/207. Dispatched 26.7
  (syscalls split by visibility: backing-reaching vs process-local, analysis test) and 26.10
  (absl::Clock in Context, SimulatedClock harness tests for the sync point and relatime) to
  lane-1.
- 26.2 review: sound; merge after: the request-end check misses sync-point deletions
  (`DELETE FROM dirty` without WHERE takes SQLite's truncate path, invisible to the update hook):
  check open_for_write and durable inodes at every request end + a no-WHERE death test; a
  harness `--wrap` fake over backing's libc calls asserting no open transaction, so unhooked call
  sites fail; death tests for the untested branches; the tiers' added wall time measured (26.6's
  baseline); checker into main_static_traced; nits. Three deviations recorded in the phase file.
- 7.1 merged (f218d45, 13 commits): presubmit 208 pass + 1 skip (ASan-only test), asan //dcfs
  //tools 44 pass, ubsan //... builds, hermeticity build with host gcc/ld and /usr/lib/gcc blocked
  passes. Warnings lane released to rebase/prune; 7.2 coverage then 26.12 reproducible build
  dispatched (lane-4). 12.4b done in lane-2 (1efbe54): schema v5 (last_stub_id, inodes_unlinked
  partial index), refused dentries go unknown instead of deleted (same stub nodeid/generation
  across a forget), ProbeRecoveredRows at unclean start, sweep at every start; formal/findings
  empty; presubmit+formal 206/206, asan 33/33; under review.
- 26.7 + 26.10 done (lane-1, step-26.7 @ d35d6ea): `//dcfs:syscalls_backing` split out (openat..
  open_by_handle_at, ioctl, mount/umount2; process-local stay in `syscalls`); enforcement is a
  genquery golden of direct dependents (visibility cannot name one target inside a package):
  dir_cache_fs/metadata_cache are not dependents; failing-first with a deliberate dep. Clock:
  `Context::clock` (absl::Clock*, GetRealClock default); SimulatedClock tests: no sync before
  the interval, exactly one after AdvanceTime, held fd unchanged across a sync point, relatime
  records the simulated time + 24 h rule (failed first with wall times); scanner and ban list
  cover clock_gettime/gettimeofday/time/absl::Now (0 sites). presubmit+formal 208/208, asan small
  26/26. Pending: design.md paragraph, rebase over 7.1, retest; then merge.
- 12.4b review: (a) and (c) correct; (b) SENT BACK: ProbeRecoveredRows runs inside StartRun, which
  main.cc calls before InitRoot registers the mount fds, so in production every probe fails
  (NotFound, no errno), logs a WARNING per dirty inode and deletes nothing; the harness fixture's
  Start() order hid it. Fix: one backing::Startup() that main.cc and the harness share (ListDirty
  before RecoverDirty; the probe after InitRoot/StartupPurge), test in production order with a
  trace. Also: query-plan test for inodes_unlinked; design.md: a forgotten boundary's stub can
  outlive it until the parent is relisted (deliberate); probe cost note. Lesson for the
  checklist: a start-up change must be tested in main.cc's order, not the fixture's.
- 26.7 + 26.10 merged (a6b86df, 4 commits): syscalls_backing split with a dependents golden;
  Context::clock (absl::Clock) with SimulatedClock tests; design.md "The clock" paragraph; the
  absl::Now ban's mangled name holds under clang/libc++. presubmit+formal 209 pass + 1 skip (+
  readdir_boundary timing flake, 3.04 s, third time: N4 now). Dispatched to lane-1 (step-readdir):
  N4 as work counting (harness step-count bound for N=100/1000, readdir_boundary as a 4N-vs-N
  CPU ratio) and SQLite reductions 1-4 (attrs joined into ListDir, LIMIT = reply budget, ReadOne
  stops after the first row, merged completeness statements), budgets lowered per change,
  bench_readdir before/after; (5) create's phase-3 merge and (6) profiling deferred.
- Warnings audit ready (lane-5, step-warnings @ ed88abe, 3 commits on 7.1; NOT yet merged: its rebase onto the current main conflicts after 26.7/26.10, handed to the agent; the orchestrator's chained command logged a merge that had not happened, corrected here): -Werror/-Wimplicit-fallthrough/
  -Wno-sign-compare scoped to our sources (externals get none of our flags; 7.1's external
  -Wno-implicit-fallthrough and the -Wno-error lines removed), global -Wl,--fatal-warnings,
  analysis tests (warning_flags_*, external_compile_*), kernel_config rename, Initramfs echo
  removed; the libfuse/googletest/numactl patches and SQLite define dropped (moot under lld);
  two narrow per-repo -Wno flags remain (liburing unused-parameter, google_benchmark
  thread-safety-analysis). Cache-less plain and asan builds: zero warnings from our code. fast
  147 pass + 1 skip, presubmit 216 pass + 1 skip. Nit left: Bazel warns `platforms` 1.1.0 vs the
  root's 1.0.0 (7.1's bazel_dep: bump to 1.1.0 in the next MODULE.bazel touch).
- 7.2 + 26.12 done (lane-4, step-7.2 / step-26.12 @ 2a5fb96): native `bazel coverage` with the
  toolchain's coverage feature, instrumentation filter ^//(dcfs|bench|tools), combined lcov;
  guest profiles via LLVM_PROFILE_FILE=/cov/%m.profraw tarred onto an extra virtio disk (serial
  at 8 KB/s timed out), cov-lcov.sh -> COVERAGE_DIR; coverage_pipeline_test self-check; CI
  `coverage` job uploads the lcov (no gate until Phase 8). BASELINE (small+medium tiers): dcfs/*.cc
  92.0% lines / 72.5% branches; all ours 87.7%; least covered: status.cc 60.7, syscalls_process
  77.4, sqlite.cc 82.5, device_id.cc 82.8, main.cc 84.7, file_handle.cc 85.4, mounts_below 88.5,
  syscalls.cc 88.5, dir_cache_fs.cc 90.0, backing.cc 91.5; fuse_ops.cc 30/98 branches. Crash/
  power/SIGKILL paths produce no profile (documented). 26.12: two builds from different paths and
  output bases are byte-identical today (toolchain redacts __DATE__/__TIME__, relative paths);
  gate `bazel run //tools:reproducible_build` (~12 min cold) + repro_compare self-check; CI job.
  Under review. Needs russ: the baseline above is the one to pick tests from (Phase 8).
- Warnings audit merged (55caee3, 3 commits on 26.7): presubmit 217 pass + 1 skip. Our code builds
  with zero compile or link warnings under the pinned clang; externals get none of our flags;
  analysis tests pin both. 7.4 UBSan dispatched to lane-5 (dcfs-investigator).
- 7.2/26.12 review: pipeline correct (profiles merge across daemon restarts via %m and across unit
  and e2e tests: hits summed, verified on the combined report; dcfs/ 92.4% lines 72.5% branches
  small+medium tiers; trace tests contribute nothing). Fix before merge: llvm-profdata
  --failure-mode (a truncated profraw silently dropped a test's lcov with only a WARNING; CI must
  check an e2e-only file like main.cc); stale docs (serial console, --config=coverage); 26.12
  compares //dcfs:main too and states same-host-only until 7.1b, own repo contents cache for one
  build; banned_symbols incompatible under the coverage config; the filter's junk records;
  Phase 8 scope = dcfs/ only, branches reported; CI cache sizes (does the saved repo cache
  include the 12 GB extracted LLVM?). 26.2 interaction: E2E_COVERAGE_OBJECTS follows initramfs_for.
- 26.2 review fixes done (lane-3): the truncate blind spot closed by a no-op TEMP trigger on
  `dirty` held by the checker's connection (SQLite then deletes row by row and the update hook
  sees every row; the per-request sweep first tried made a 6,000-file test quadratic); harness
  `--wrap` backstop over 26 libc calls aborts on an open transaction/cursor (an unhooked
  StatFd failed to die before); 15 more branch death tests (33 total, harness 98 tests);
  checker linked into main_static_traced; RSS level over 5 find/drop cycles (no retention).
  RUNTIME of the checks (two runs each, load 8-15, nocache): fast 295/290 s vs plain 280/234 s
  (+5-15%); presubmit 1309/1388 s vs 1055/978 s (+25-35%); harness within noise. readdir_boundary
  runs the plain build (its timing guards the algorithm); kRecountLimit 16384 -> 1024. Rebasing
  onto main (many branches landed since), then merge; 26.6 follows.
- readdir step done (lane-1, step-readdir @ 09e8f3d): N4 = ReaddirWorkTest (SQLite step bound for
  N=100/1000, plain and plus; a per-entry completeness check makes it fail) + readdir_boundary as
  a 6000-vs-1500 daemon CPU-tick ratio (quadratic variant: 246 vs 23 ticks FAIL); reductions:
  attrs joined into ListDir (1000 entries: 3298 -> 1298 steps), LIMIT = reply budget (plus 2800
  -> 1262), ReadOne stops after the first row (every sql_stmts budget down: warm-lookup 4 -> 2,
  create 136 -> 119), merged completeness/ParentOf statements (warm-readdir 31 -> 13). bench_
  readdir 10k entries: 652 -> 252 ms (backing 83): 7x -> 3x. No golden or backing count moved;
  no modelled transition. Remaining: create 119 statements / 10 transactions / 2 syncs (plan
  item 5), per-row C++ (item 6). New flake class seen: atime_test second-boundary race (want
  ...067 got ...068; 4/4 on rerun) -> make it deterministic with the injected clock (26.10)
  when touched. dir_cache_fs_test under ASan: timeout moderate, asan_mem 960. Under review.
- readdir review: no correctness bug (all 21 ReadOne callers unique/aggregate/LIMIT 1; the join
  cannot drop a present dentry; stubs and attrs_valid=0 take the old path; LIMIT never too
  small; merged statements equivalent; budgets match; bench consistent). Fixes before merge:
  design.md (join, LIMIT, ratio check; a Phase 22 note: attributes now read at listing time),
  stale step_counter comment, re-measure the ratio check's bite on the faster code, one macro
  for the attribute columns, the step counter counts without formatting SQL (the ASan peak),
  ReadOne's unique-only contract guarded in the checking build.
- russ (2026-10-08): auto-mode denials resolved; libfuse's io_uring stays (needed later; libnuma +
  liburing remain shipped with their banned-symbol allows); coverage baseline to be looked at
  when convenient; a push from f9103b3 is reasonable (first GitHub run on the clang toolchain).
- 12.4b merged (7136ac2, 5 commits): schema v5 (cache_state.last_stub_id, inodes_unlinked
  partial index); refused dentries go unknown on a mere forget (same stub nodeid + generation
  across it); backing::Startup() = StartRun (ListDirty before RecoverDirty) -> InitRoot ->
  StartupPurge -> ProbeRecoveredRows, shared by main.cc and the harness (the first version ran
  the probe before the mount fds existed: caught in review); sweep at every start (query-plan
  test); formal/findings empty, three new known-bug variants; `probe` lifetime trace line.
  presubmit+formal 209 pass + 1 skip, asan 35/35. Phase 22 (cancellation) dispatched to lane-2.
- 26.2 rebased over everything up to 0b184a7 and green (fast 149 + 1 skip, presubmit+formal 218 +
  1 skip, asan small 30/30; the checker caught 26.10's ClockTest forging a FORGET of an uncounted
  lookup: fixed); needs one more rebase over 12.4b (conflicts in dir_cache_fs.cc/backing.cc:
  Startup(), the probe, v5 stub semantics) -> agent; then merge, then 26.6.
- Phase 22.1/22.2 design round done (lane-2; notes/cancellation-inventory-2026-10-08.md). Key
  fact: once dcfs has read a request the caller cannot be killed, even by SIGKILL, until dcfs
  replies; a 100k-entry first listing on a slow disk (~70-100 s) makes `ls` unkillable today.
  Recommendation (b): single-threaded safe points (own session loop, non-blocking poll of
  /dev/fuse at safe points; EINTR at the next probe batch / before a phase-2 syscall), written
  so the coroutine rewrite inherits them; kernel-blocked syscalls (spin-up, syncfs, network)
  stay uninterruptible until (c)/(d), documented. Needs russ: approve (b) + the documented limit.
- russ (2026-10-08): Phase 22 = option (b): synchronous interrupt checkpoints now (own session
  loop, non-blocking drain of /dev/fuse at checkpoints, EINTR there), single-threaded; async
  interruption of kernel-blocked syscalls (spin-up, syncfs, network) waits for coroutines/
  io_uring. Implementation (22.2 model, 22.3 tests + checkpoints, 22.4 docs) dispatched (lane-2).
- readdir step merged (9fd9605, 8 commits): N4 (ReaddirWorkTest step bound; readdir_boundary as a
  CPU-tick ratio: quadratic variant 39 vs 435 ticks, 11x, fails) + reductions 1-4; every sql_stmts
  budget lowered; warm 10k listing 652 -> 252 ms; DCFS_ATTR_COLUMNS/_NULLS macros; the step
  counter raises sqlite.cc's verbosity only; ReadOne documented unique-only in style.md (a debug
  second step would restore the counted step: the checking build is the place). presubmit+formal
  217 pass + 1 skip, asan small 30/30. dir_cache_fs_test asan_mem 960 (ASan quarantine, measured).
- GitHub run 37651534035 (7f326a0, first on the clang toolchain): osv green; fast: 62 guest tests
  FAILED with their tests green inside: the kernel-failure detector's dmesg pattern matched
  "WARNING: " in the AMD runners' hardware-vulnerability advisories ("Speculative Return Stack
  Overflow: WARNING: See https://kernel.org/..."); the Intel dev box never prints them. Fix in
  lane-6 (narrow to WARNING: CPU: / WARNING: at / cut here; advisories as must-pass fixtures in
  run_qemu_verdict_test). Lesson: the verdict self-check needs fixtures from a real runner's log,
  not only ours. The toolchain itself built and ran fine on the runner. 12.5 identity model
  dispatched (lane-1, dcfs-protocol).
- Detector fix merged (f81d3f6): guest/init's dmesg grep matches real kernel warnings only
  (WARNING: CPU:, WARNING: at, the cut-here marker, BUG:, Oops, kernel BUG at, Call Trace:,
  Kernel panic, general protection fault); the SRSO/Spectre V2 eBPF/RETBleed/MDS advisories are
  must-not-match fixtures in run_qemu_verdict_test (failing first). Not booted on an AMD host:
  the next GitHub run is the proof. Push from f81d3f6.
- 7.1b dispatched (lane-6, dcfs-investigator): sysroot (Debian via rules_distroless) vs Alpine
  clang/lld through the musl loader (vs the hybrid B'): decide with the hermeticity build
  (/usr/include, /usr/lib/gcc, host libs blocked), prototype the winner.
- 26.2 green on main-as-of-12.4b (harness 110/110, fast 149 + 1 skip, presubmit 217 + 1 skip +
  one serial-console MEM-line garble flake on removed_test_btrfs, 3/3 on rerun; asan small 30/30
  with dir_cache_fs_test's ASan guest at 832 MiB); the start-up full check runs at the end of
  Startup after the probe; the v5 stub rule in the tri-state check; a dead Context's DESTROY
  tripping "dirty-set" in a crash test is handled harness-side (a crashed daemon runs no DESTROY).
  One more rebase (readdir step + detector fix) handed to the agent; then merge, then 26.6.
- Coverage baseline reviewed (notes/coverage-baseline-2026-10-08.md): dcfs 92.4% lines / 73.3%
  branches; the non-passthrough data path has zero hits (Q1 for russ), four dead functions under
  our configuration (Access handler, FuseRequest move-assign, openat2 wrapper, FileHandle::
  ToString: Q2), cheap gaps (futimens/removexattr on O_PATH, status/device_id/mounts_below/
  file_handle edges, main.cc misuse), error branches left to 26.6 + Phase 11; gate scope Q3.
  7.2/26.12: review fixes done (continuous-mode profiles found atime_test's cleanup truncating the
  daemon's profile 2-4/10 runs; CI repo contents cache no longer saved: 37 GB locally; coverage
  job restores presubmit's cache read-only); final rebase over the readdir step handed back.
- russ (2026-10-08): coverage gate at today's dcfs numbers (92.4 lines / 73.3 branches), ratchets
  up; the non-passthrough data path is reachable (per-file passthrough refusal past the backing
  stack depth: source on a FUSE mount) -> e2e variant dcfs-over-dcfs, not deletion; delete the
  dead Access handler, FuseRequest move-assign, openat2 wrapper; test FileHandle::ToString with a
  fields-not-literal rule for debug strings (style.md); default_permissions stays.
- 7.4 merged (1a65836): `--config=ubsan` = undefined + an explicit vptr (clang 22's `undefined`
  omits it: the self-check's vptr case failed to die until named), -fno-sanitize-recover=all,
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 in the guest; implicit-conversion, unsigned-
  integer-overflow, float-divide-by-zero, local-bounds, nullability left out with reasons. Whole
  suite under UBSan: fast 65 (22 min), presubmit 216 (+12 min), large/enormous 243 total (+52
  min), 3 skipped (sanitizer-incompatible gates); NO findings anywhere (no ubsan.supp). CI:
  UBSan step after ASan in `full`. Follow-ups: memory_test_xfs reclaims in the PLAIN build at
  mem=448 (22k scans) and under UBSan at 576 (0 at 768): resize; the `full` job's 350-min
  timeout is tight with ASan + UBSan: split the sanitizer runs into their own jobs.
- Dispatched (lane-5): 7.4b memory_test sizing (plain xfs reclaims at 448) + sanitizer suites as
  their own CI jobs; then Phase 8 cheap gaps (status/device_id/mounts_below/file_handle unit
  tests, FileHandle::ToString with the debug-string rule written into style.md, utimensat/
  removexattr-on-closed-file guest checks). Deletions and the dcfs-over-dcfs variant wait for 22.
- 7.2 + 26.12 merged (96c53e1, 7 commits): native bazel coverage with the toolchain's feature,
  guest profiles in continuous mode (%m%c + runtime-counter-relocation: a SIGKILLed or
  mid-write daemon keeps its profile; atime_test's cleanup had been truncating profiles 2-4/10),
  llvm-profdata --failure-mode=all, run-qemu fails on a bad profile, CI checks main.cc (e2e-only)
  coverage, coverage job restores presubmit's cache read-only; CI repo contents cache no longer
  saved (37 GB locally). 26.12: two builds (own output bases, own repo contents cache for one)
  byte-identical for main_static, main and dcfs.8; same host only until 7.1b. Next: the Phase 8
  gate (baseline file at 92.4/73.3, ratchet) and 26.5 (lane-4).
- dir_cache_fs_test (plain) timed out at its 60 s `short` under load 18 on main itself (the
  harness grew: readdir work test, lifetime tests): 26.2's rebase sets it `moderate`.
- Dispatched (lane-4): 8.1 the coverage gate (dcfs/coverage_baseline.txt at 92.4/73.3, fails on a
  drop and on an unrecorded rise >= 0.1: the baseline only moves up in the raising commit;
  self-checks) then 26.5 limited mutation testing (mull on clang 22 or an AST-JSON mutator;
  first run's survivors as findings; cost per mutant).
- russ asked whether default_permissions should be required: it already is by construction
  (dcfs appends it; FUSE has no "no" form). Proposed: Init() verifies it and refuses otherwise;
  reject --fuse_opt naming it; keep the Access handler as a fail-closed EACCES (ENOSYS would
  make the kernel ALLOW) covered by a forged-request test, instead of deleting it. Awaiting yes.
- russ (2026-10-08): 26.4b: FUSE requests per user-level operation (yes), slope tests for every
  operation class (yes), allocations per op (maybe later), recovery/shutdown ad hoc, workload
  budgets (no); the VLOG-based step counting is replaced by a hook counter first. Testing stays
  process (presubmit before merge by the lanes; CI post-push), no git hook.
- russ (2026-10-08): 26.4b reuses the protocol-event recorder for request-level accounting (FUSE
  requests, phases, transactions per request = reductions of the trace); SQLite steps as a
  counter on the same hook; the protocol-events and Context::checks hooks merge into one
  testonly observer interface.
- russ (2026-10-08): default_permissions required: Init() verifies and refuses; --fuse_opt naming
  it rejected; Access handler kept fail-closed (EACCES + error log; ENOSYS would mean ALLOW) with
  a forged-request test. Joins the Phase 8 cleanup step (after Phase 22 merges: same files).
- Test-speed review from the GitHub runs (phase 6 file, 6.4): host-only tests rerun under every
  sanitizer/coverage config (TLC 19 min each time) -> incompatible under those configs; destroy
  _test 100k -> 20k (5 ms per create on the runner = 2 durable fsyncs per CREATE: a product
  finding for 26.4b's create slope test); TLC workers = runner CPUs. For the next free lane.
- 26.2 merged (4511503, 10 commits): runtime invariant checks in the checking build the small and
  medium tiers boot; hooks before every backing call (harness --wrap backstop), request-end
  checks, the TEMP-trigger fix for truncating deletes, 33 death tests, checker in the traced build
  too; dir_cache_fs_test ASan allowance 1088; run-qemu.sh now honours TEST_TIMEOUT for unit
  guests too (they were capped at 60 s regardless: the real cause of the harness timeouts under
  load; verdict test failed first). presubmit+formal 220 pass + 2 skips. 26.6 (fault enumeration
  by call site, runtime to be reported) and then 26.4b dispatched to the same agent.
- 6.4a-c (host-only tests incompatible under sanitizer/coverage configs; destroy_test 20k; TLC
  workers follow the CPUs) added to lane-5's CI round, ahead of its Phase 8 cheap-gaps round.
- russ (2026-10-08): shard the sanitizer and large-tier suites across runners (6.4d, 3 shards per
  suite, deterministic target partition in test.sh); added to lane-5's CI round.
- GitHub run 37667603332 (de64dbb): `fast` failed at the new commit-subject gate (26.13): the eight
  readdir commits carry `N4:` (the orchestrator's brief named the review finding, not the step
  26.4); the gate was right; pushed history stays; the next push range starts after them.
  `osv` was cancelled at 20 min inside prepare (1 min before 7.1): the fetch now pulls the LLVM
  tarball on a runner with no repository cache -> lane-5 (restore the repo cache read-only /
  avoid the toolchain for the fetch / raise the timeout). Nothing else ran. process.md: the
  dispatch prompt names the subject prefix.
- 8.1 + 26.5 merged (6788fc6): tools/coverage_gate.sh in the CI coverage job against dcfs/
  coverage_baseline.txt (92.40 / 73.30; fails below, fails on an unrecorded rise >= 0.1 naming
  the new baseline; self-check over canned lcovs); mutation testing: tools/mutation/mutate.py
  (clang AST JSON, pinned clang; negate conditions, delete Begin*/End*/Mark* calls, swap
  present/absent; scope.txt), `bazel run //tools/mutation:mutate`, killers = fast //dcfs/...
  then the trace test; 277 mutants; 25-sample: 21 killed (19 by the small tier, 2 only by trace
  validation), 4 survived (8.2), 91 s per mutant, ~13 h full on this machine -> schedule job;
  mull rejected (host-side runner, LLVM 22.1.2 deb, no sha256 fetch). 12.5 done (lane-1), in
  review: findings for Phase 14 (generation check under recycling) and a README correction
  (NFS handles lost after a power loss for rows since the last durable commit).
- Dispatched (lane-4): 8.2 tests killing survivors #1, #3, #4 (each shown killing its mutant),
  then 26.5b: the weekly/dispatch mutation job (sharded by mutant ids if over the 6 h limit;
  survivors as an artifact, never a failure) and a per-push `--changed <range>` mode bounded to
  ~30 mutants that fails on a survivor in changed code.
- 12.5 review: merge after a README wording fix (a file CREATED through dcfs since the last
  durable commit also loses its NFS handle at a power loss: its row is phase 3); the 26.3 order
  question resolved against the doc (all reads through one O_PATH fd that pins the inode; ext4
  frees an ino only at eviction); Phase 14 finding recorded in its phase file (generation check
  for held nodeids under recycling; FUSE_ATTR_GENERATION would close it kernel-side).
- Phase 22 done in lane-2 (step-22 @ 059a7e2): checkpoints (LookupOrPopulate, every 16 probes in
  PopulateDirectory, before every phase-2 syscall, cold OpenInode, Fsync/Fsyncdir), SessionLoop
  (non-blocking drain of /dev/fuse at checkpoints; INTERRUPT to libfuse at once, the rest queued),
  dcfs/interrupts.h via Context, `Interrupted` event; model: Interrupt(p) behind `Interrupts`,
  GuardsBalanced, 3 configs (medium 42 s; two large ~2 min), 3 variants; guest: timeout -s INT 1
  ls on 20k entries ends 0.7-0.8 s after the signal (was 14-20 s), kill -9 of find 0.7 s;
  presubmit+formal 223 pass + 2 skips, asan small 30. DEVIATION to fix: a refused INIT's EPROTO
  now returns 0 (SessionLoop cannot see libfuse's se->error). AGENTS.md phrase changed: "at safe
  points" -> "at checkpoints before its backing syscalls (dcfs/checkpoint.h)" (russ to see).
  Under review.
- Phase 22 review: sound; merge after: INIT refusal exits non-zero (required), rename's double
  re-resolve, Drain reads at most one request ahead, SPLICE_READ unset, parameterized checkpoint
  tests, large TLC runs to completion, cancel_test bound by accelerator, docs/AGENTS wording.
  Deviations recorded in the phase file.
- 12.5 merged (e3fe0aa, 4 commits, +2671/-21): formal/ident.tla (OneHandleOneObject,
  HeldResolvesToItsObject, HandlesResolveToTheirObject, ServedWhileLive, GoneIsStale,
  ReuseDetected, NoBadInode; Target = today | Phase 14; Evidence), 6 known bugs, 5 limitations,
  IdentityResolved event + DCFS-IDENT traces (36 valid), design.md's population-policy text
  corrected (order of the reads is irrelevant: one O_PATH fd), README: NFS handles of rows
  recorded since the last durable commit (fills, and the objects a change creates) are lost at a
  power loss. formal 83/83, fast 167 + 2 skips. 12.6 (recovery idempotence) + 12.7 (effect-point
  property) dispatched to the same agent.
- 7.4b + 6.4a-d + 8.3 done in lane-5 (step-7.4b, 5 commits; rebase + full tier runs pending before
  merge): memory_test mem=704/asan_mem=832 sized by reclaim_scans=0 (peak_used grows with the
  allowance: size by reclaim, not peak); CI: full (large+enormous), asan, ubsan each a 3-shard
  matrix (test.sh --shard=I/N --sizes, cquery drops config-incompatible tests, shard.sh +
  shard_test; 42 ubsan tests per shard, 8 large per full shard; est. ~35 min of tests + build per
  sanitizer shard); osv restores the repo cache read-only, 45 min (the 20-min hang did not
  reproduce: 63 s cold); subject gate names the hash; 6.4a: 34 host-only tests + the tlc/trace
  macros incompatible under sanitizers/coverage (100 of 226 skipped per sanitizer config, was 3);
  6.4b: destroy_test 20k files, mem=384/asan 1344, plain 207 s / asan 138 s (6-17 ms per create
  here, 5 on the runner: 26.4b); 6.4c: TLC already uses -workers auto. 8.3 cheap gaps: status.cc
  59/61, device_id 127/127, mounts_below 55/55, file_handle 99/103, setattr.sh covers the *OPath
  helpers (reached only for non-regular files: fifo/symlink); FileHandle::ToString per field;
  style.md's debug-string rule written.
- 8.2 + 26.5b merged (4af0981): three survivors killed test-first; per-push mutation mode
  (`mutate.py changed --range`, <= 30 mutants, fails on a survivor in changed code; wired into
  `full`, 120-min step) and the weekly 6-shard mutation job (survivors as an artifact, fails
  only on tooling errors; ~2.7 h per shard estimated). First per-push run: 2 of 91 mutants
  sampled, both survived (8.2b: Release `writable` negation; Fallocate End deleted). Model gap
  12.11: no event for the end of an attribute change. Note for lane-5's merge: the mutation
  step sits in `full`, which lane-5 shards: it should become its own job (lane-4 after lane-5
  lands).
- USAGE LIMIT (2026-10-08): orchestrator cut off; agents keep running and report when resumed.
  State: main 048ebb9. In flight: lane-1 12.6/12.7 (then 12.11); lane-2 Phase 22 review fixes
  (INIT exit code, re-resolve, drain, parameterized checkpoint tests, large TLC, rebase) ->
  review-light merge -> Phase 8 cleanup (default_permissions required, fail-closed Access,
  deletions, dcfs-over-dcfs variant); lane-3 26.6 (runtime to report) -> 26.4b; lane-4 8.2b ->
  move the per-push mutation step out of the sharded full once lane-5 lands; lane-5 step-7.4b
  (CI shards, osv cache fix, 6.4a-c, 8.3) rebasing + full tiers -> MERGE FIRST, then russ pushes;
  lane-6 7.1b (interim status asked after 3 h of silence). Needs russ: push after lane-5 merges;
  linux-ext4 report; AGENTS.md cancellation wording at the Phase 22 merge.
- 7.1b done in lane-6 (step-7.1b): option A+ = @dcfs_llvm: the LLVM release plus a Debian sysroot
  (libc6/libc6-dev 2.36 from bookworm, linux-libc-dev 6.12 from trixie: bookworm's 6.1 UAPI lacks
  STATX_MNT_ID_UNIQUE; same snapshot) and the LLVM binaries' runtime libs (libstdc++, libgcc_s,
  zlib, libxml2, ICU, liblzma; patchelf from Alpine for RUNPATHs); `bin/clang.cfg` makes the
  sysroot the default (liburing's probes see it too); registered via toolchains_llvm's
  toolchain_root + sysroot. Remaining host dependence: glibc >= 2.36 to RUN bash/LLVM (Ubuntu
  24.04 ok, 22.04 not), bash/mktemp/realpath/rm, rules_distroless's tar/grep. Hermeticity build
  with /usr/include, /usr/lib/gcc, host compilers, libc6-dev files and the runtime libs blocked
  passes; toolchain_hermetic_test and mkinitramfs_test fail first; main_static now against glibc
  2.36 and identical over two differently-fetched builds; sanitizer guests take libc from the
  sysroot. B (Alpine clang/lld) out: musl binary + custom cc_toolchain; B' out: Alpine's libc++/
  sanitizer runtimes are musl. Pending: `7.1b:` subject prefixes, rebase, full tiers, repro gate;
  then review. Follow-ups: glibc into the shipped SBOM (git-commit purls only today); extract
  only the needed parts of the 12 GB tarball.
- Lane-5 round merged (56980ad, 5 commits): 7.4b memory tests mem=704/asan 832 by reclaim; CI:
  full/asan/ubsan as 3-shard matrices (test.sh --shard, shard.sh + shard_test), osv restores the
  repository cache read-only (45 min), subject-gate failure names the hash; 6.4a: 100 of 226 tests
  skipped under each sanitizer config; 6.4b: destroy_test 20k (mem 384 / asan 1344); 8.3: cheap
  coverage gaps + the debug-string rule in style.md + FileHandle::ToString tested per field.
  fast 169/169 nocache, asan small 31 + 1 skip on the rebased tree; presubmit's only failure was
  dir_cache_fs_trace_test at its 900 s limit under load 28 (823 s alone): timeout to raise.
  PUSH POINT: main 56980ad+ (the osv fix is in). Mutation step sits in full's shard 0 until
  lane-4 moves it to its own job.
- Dispatched: lane-4 moves the per-push mutation step to its own job and splits/raises
  dir_cache_fs_trace_test (823 s vs 900 s); lane-5 starts Phase 11.1 (dm-flakey/dm-error helper
  over the cache and backing disks via Alpine's dmsetup, first I/O-error and power-cut tests per
  phase, each stating its invariant; findings stop the step).
- russ (2026-10-08): CI timeouts must cover the cold-cache path (evictions, lock-file changes):
  each job's limit = ~2x its measured cold time, with the estimate recorded beside it; a
  `workflow_dispatch` input `cold: true` skips the caches so the cold path can be exercised on
  purpose; osv's one-minute past was the small module graph, not a special job: every job that
  evaluates the module graph restores the repository cache. Added to lane-4's CI follow-up.
- GitHub run 37690838032 (38303ec): osv green (the cache fix held: 1 min); fast failed at the
  subject gate on e3fe0aa ("12.5 review: ..."), everything else skipped. Fixes: the gate becomes
  its own non-gating job (lane-4); the orchestrator runs commit_subjects.sh over every branch
  before merging (process.md). A push of main past 38303ec (plan: commits only) runs the full
  pipeline.
- 12.6 + 12.7 done in lane-1 (step-12.6; merge pending the large TLC runs and 12.6b): dcfs.tla
  `RecoveryIdempotent` (crash during recovery: Next split into CrashServing/Recovering/Stopping),
  lifetime.tla's `RecoveryIdempotent` (probe steps separate, crash between); 12.7 action
  properties EffectAtSyscall, BackingAtSyscall, CacheLearnsAtCommit (the first two also on every
  recorded trace); variants recover_clears_dirty_first, effect_before_syscall, effect_after_
  syscall. FINDING: recovery is not idempotent: RecoverDirty clears the dirty set durably before
  ProbeRecoveredRows runs from memory, so a crash during the probe loses the unprobed rows (a
  freed object's row stays; identity safe). 12.6b: keep the listed rows dirty until probed (fix
  test-first, finding -> real invariant). formal 87 pass + the two large configs timing out at
  900 s under load (to be run alone with a long timeout); trace test 131 valid; fast 171 + 2 skips.
- 12.6/12.7 review: finding real (window opens at RecoverDirty's commit; only leaked rows of
  freed objects are lost; nothing live deleted); fix shape confirmed (rows stay dirty through the
  probe, one short delete transaction after the loop; lifetime.tla keeps the queued rows). Model
  critique: dcfs.tla's RecoveryIdempotent is CrashSafe-during-recovery plus a tautological
  fixpoint conjunct; MC_small (MaxCrashes = 1) never reaches a crash during a dirty recovery ->
  a MaxCrashes = 2 medium config; EffectAtSyscall follows from CacheNeverWrong + BackingAtSyscall
  (keep as the observable restatement; tie its permission to the syscall actions for Phase 22's
  Interrupt); the plan's 12.7 reply check (errnos, mutation results vs the backing between call
  and reply) is not implemented: implement or record as 12.7b; CacheLearnsAtCommit onto traces.
  Sent with 12.6b to lane-1.
- 11.1 merged (ba7687d): fault_lib.sh over dm-flakey/dm-error (healthy/error-writes/drop-writes/
  error-reads/error-io/dead, switched live), Alpine dmsetup (420 KB) in the e2e initramfs,
  fault_selftest (6 checks fail with injection stubbed), fault_backing/fault_cache/fault_power
  tests (cuts at each create phase and a sync point via fsfreeze, both disks dropping writes),
  all under the checking build, plain + ASan + UBSan: NO violation. Finding: a cache-disk I/O
  error reaches the caller as EAGAIN (SQLITE_IOERR -> UNAVAILABLE): decided EIO (11.1b, lane-5),
  then 11.2 (ACE workloads, real-kill variants, the fs matrix).
- 26.6 + 26.4b done in lane-3 (pending rebase/merge): 26.6 = 12 workloads, 49 call sites, 111
  iterations (EIO + the branched errnos), CheckAll + unclean Startup + CheckAll after each, no
  finding, 16-27 s in the guest, fast tier +4 s (kept: "seconds"); 26.4b = ProtocolEvents the
  single testonly observer (events, checks, counters; Context::checks gone), counters replace the
  VLOG step counting (budgets identical), request budgets (ls -l 100 = 1 LOOKUP + 1 OPENDIR + 6
  READDIRPLUS; cat = 1 LOOKUP + 1 OPEN; stat 10 deep = 12 LOOKUPs), slope tests exactly linear:
  create 82N+3 steps / 7N transactions / N+1 WAL fsyncs / 14N+64 backing calls; mkdir 48N+3 / 3N /
  1 fsync; unlink 38N / 4N / N; rename 59N / 4N / N; setattr 35N / 3N / N; cold lookup 20N+16.
  FINDING for the design (Needs russ): create, unlink, rename and setattr fsync ONCE PER
  OPERATION (each names an inode not yet durably dirty, so its phase 1 commits durably); only
  mkdir gets one per sync point; design.md's "a burst of creates costs one WAL fsync" is true for
  mkdir only; destroy_test's 5 ms per create is this fsync.
- Phase 22 merged (87b35a0): see the phase file. Unblocked: the Phase 8 cleanup (default_permissions
  required + fail-closed Access + deletions + the dcfs-over-dcfs variant) dispatched to lane-2.
  Note: dir_cache_fs_trace_test at 867 s of its 900 s (lane-4 is splitting it).
- 26.6 + 26.4b green on main-as-of-11.1 (step-26.4b @ 5a34528: harness 121 incl. death tests,
  sweep 49 sites / 111 iterations / 14.7 s, budgets unchanged, fast 176 + 2 skips, presubmit+
  formal 252 + 2, asan small 32 + 1; subjects ok); one more rebase over Phase 22 (checkpoints and
  the Interrupted event in the same files) handed to the agent; then merge.
- 26.5c merged (1be6511): `mutation-changed` and `subjects` are their own jobs (a bad subject no
  longer skips the tests); `cold: true` dispatch input skips the caches; timeouts carry cold
  estimates (cold fetch of every repo 23.5 min under load; C++ ~20 min); accelerator.sh no-op
  without testlogs; dir_cache_fs_trace_test split into a/b/c (132 valid traces, ~5 min each under
  load). 8.2b merged (55d0128). 8.2c (seven survivors) dispatched to lane-4. Job list now:
  subjects, fast, presubmit, coverage, reproducible, mutation-changed, full x3, asan x3, ubsan x3,
  osv.
- 2026-10-08, reports and reviews: lane-5 11.1b (EIO) + 11.2 done; lane-1 12.6b done; lane-2 8.4
  done; lane-3 26.4b green but based on 9563327 (rebasing again). Three Opus reviews, all "fix
  first": 11.2 (boot 1's verdict discarded in kill mode, no kill-mode self-check, weakened
  fault_backing listing check, ACE compares too few fields and claims more than it tests, 596 s
  ACE test unsharded); 8.4 (`mount_options` defaulted to default_permissions so the check could
  pass by default and the harness never built options; LOG(FATAL) branch; per-request ERROR log);
  12.6b (HIGH: recovery cleared probed rows from the dirty set without syncfs, pre-existing, model
  blind to it because a daemon crash made backing writes durable; fault test could pass
  vacuously). Fix lists sent to the lanes; plan corrected: Phase 11 status as built + 11.2b
  (fsstress/fsx), Phase 8 decision (dcfs-over-dcfs impossible: FUSE sources are refused for lack
  of a filesystem UUID), 12.6b/12.7b. Needs russ: support sources without a filesystem UUID
  (FUSE)? (identity question, Phase 14). TLC large/nolock: timeouts unchanged (26/18 min only at
  host load 15; state counts equal main's).
- 26.4b + 26.6 merged (b866c15) after a second rebase (first rebase had been onto 9563327; the
  conflict was 26.5c's trace a/b/c split meeting the ASan trace line; group c's filter now excludes
  FaultSitesTest/SlopeTest, which have their own targets). Fast 176 + 2 skips, traces a/b/c
  167/215/195 s, cancel_test, subjects ok. Lane-3 freed: 11.2b fsstress/fsx dispatched (new
  implementer).
- 8.4 merged (1b2fb6d) after one review round (mount_options defaults to empty so a missing value
  fails closed; the harness builds its options with BuildMountOptions and passes `-o`; the
  POSIX_ACL/DONT_MASK refusal returns FailedPrecondition so fuse_ops.cc is the only INIT refusal;
  ACCESS error log rate-limited; FuseRequest default constructor gone, the two reply-twice tests
  live in the harness; lifecycle.sh refusal checks under `timeout 10` wanting exit 1). Fast 177 + 2
  skips, traces a/b/c, lifecycle, subjects ok. Lane-2 idle: held until a lane frees (host at load
  13-15 doubles every test time); next for it: 11.4 out of space + 11.5 instant backing crash.
- 8.2c merged (6757e55): nine of the sweep's survivors killed by new harness tests (Setattr End,
  FORGET reconciliation warnings, passthrough id ownership on failed opens, Release close failure,
  Setxattr phase-3 record); `mutation.End(); return` in Setattr's checkpoint path is equivalent
  (the destructor ends it) and listed. New harness piece: `--wrap` of fuse_passthrough_open/close
  gives the unit tier a fake kernel passthrough (backing ids visible in open replies). Fast 177 +
  2, subjects ok. Lane-4 freed and held (load); lane-2 takes 11.5 + 11.4 (new protocol agent).
- 11.1b merged (461edcf): SQLITE_IOERR/READONLY carry errno EIO (status stays UNAVAILABLE; BUSY/LOCKED
  EAGAIN, FULL ENOSPC); fault_cache's phase1-error-is-eio check. 11.2 fix round reported done
  (boot 1 verdict, seven verdict-test cases, synced/unsynced witness, listing comparison, snapshot
  with nlink+md5 in lib.sh, direct/dsplit persistence via `testutil syncfs`, negative fixtures kind,
  ACE split a/b/fs on the checking build); sent for a second review pass; CI shard estimates still
  open with the agent. Lane-2 dispatched: 11.5 instant backing crash then 11.4 out of space.
- 11.2 merged (f3a2d2b) after the second pass (merge after A: boot 1's own dmesg scan; B: immediate
  kill, status 137, the 141 was SIGPIPE from closing the fifo early; C: no stray sleeps; D: ACE xfs
  under ASan 227 s vs 199-247 plain, `long` kept). Fast 180 + 2, ACE a 601 s, verdict test, ext4
  fault + kill tests, subjects ok; xfs/btrfs variants last run on the previous commit. CI: asan
  limit 180 -> 240 min (estimate 116-159 with the cold part), full ~80, ubsan ~105; shard table in
  README. Open: measure a cold run before a fourth asan shard / time-weighted partition. 26.4b
  follow-up noted by the 12.6b reviewer: testonly::Observers forwards all 62 ProtocolEvents
  methods but nothing catches a missing one at compile time (RecoveryDone was caught only by
  trace_c) -> 26.4c idea: pure-virtual events with a null default, or a reflection test.
  Lane-5 (same agent) takes 11.6 fsfreeze.
- 12.6 + 12.7 merged (5776861): ten commits (RecoveryIdempotent, crash during recovery, effect
  points EffectAtSyscall/BackingAtSyscall/CacheLearnsAtCommit naming actions, 12.6b keeping the
  dirty set through recovery, then the review's syncfs hole: recovery clears nothing, PowerLoss vs
  DaemonCrash, ProbesDone, two known_bugs variants, Observers forwards RecoveryDone, large_test
  eternal). Fast 181 + 2, formal 99/99 without large/nolock (run alone with 3600 s: pass), trace c
  43 valid, subjects ok. Lane-1: new protocol agent on 12.7b (reply ghost).
- 7.1b second review: all fixes verified; one blocker turned into a decision for russ (Needs russ):
  the shipped SBOM now lists the sysroot's glibc 2.36-9+deb12u14 and an OSV scan of it fails:
  16 unfixed Debian records (notes/osv-scan-2026-10-07.txt:102-117, DEBIAN-CVE-2026-5450 at
  CVSS 9.8). Policy options: ignore entries with reason + expiry; keep the deb scan informational;
  or a glibc build carrying the fixes (static main_static links libc.a, so a newer glibc means a
  newer sysroot pin, Debian 13's glibc 2.41). Until decided the step is `continue-on-error`.
  Also found: clang.cfg is not an input of sandboxed actions (fix in the same round). 11.2b review:
  fix first (random mode's seed derivation aborts; fsstress op successes not counted; failures not
  visible; "warm" comparison is the kernel's cache; digest without mtime/ctime), list sent.
- 11.2b merged (0beb46e) after one review round (random seed derivation aborted; fsstress
  successes now counted per op with floors and EIO fails; op/errno table and fsx disabled set
  printed and pinned; a dcfs-cache comparison after drop_caches; digest with mtime/ctime/type/rdev;
  README integrity string fixed; cc_library for headers; self-check under busybox; ops and seeds via
  cmdline; test.sh --list excludes manual: it had listed stress_random). Short 24-35 s, long ext4
  277 s, fast 185 + 2, presubmit test/qemu 111, names tests pass with the new names-dump. Initramfs
  +0.99 MB. Lane-3 and lane-4 held until 7.1b merges (7.3/7.6/7.7 follow it) and the load drops.
- 11.6 merged (5840bf0) after one review round (5 s answered bound, held-at-the-thaw assertion,
  fresh names after quiet(), per-scenario names in the frozen cuts, read-only `testutil sql` with a
  refused-DELETE check, exact attribute matches, ext4 medium). FINDING (design): a held mutation
  blocks the whole daemon, cached reads included; documented as a limitation; the argument for
  serving threads/coroutines (Phase 22's checkpoints cannot interrupt a syscall). Lane-5 held.
- 7.1b merged (508ad34, 15 commits) after two Opus reviews: @dcfs_llvm = LLVM release + Debian
  bookworm sysroot (libc6 2.36, linux-libc-dev 6.12) + runtime libs; extract.py (hermetic Python,
  skip list, `filter="data"`, relative symlinks for the debs, dangling-link and hard-link checks,
  extract_test); rctx.watch on the script/python/patchelf; patchelf realpath fix; clang.cfg/clang++.cfg
  with the link flags, delivered to sandboxed actions via extra_compiler_files/extra_linker_files;
  toolchain_hermetic_test + self-check with four bad probes; mkinitramfs ELF-magic/readelf checks;
  glibc 2.36-9+deb12u14 in the shipped SBOM with a separate osv step, `continue-on-error` until russ
  picks the policy (Needs russ, see above); reproducibility wording names the host glibc. Fast 188 +
  2, presubmit 262 + 2, ubsan build, asan small 34 + 1, blocked-host-paths build 605 s.
- CI on fee154d (russ's push): subjects/fast/osv/reproducible green; coverage FAIL = the ratchet
  asking for the baseline to rise to 95.82/78.51; presubmit FAIL = lifetime_test and
  lifetime_recovery_test racing on a shared /tmp/TLC.tla (the plain TLC runner sets no
  java.io.tmpdir; trace_validate.sh does); mutation-changed FAIL = mutate.py's clang invocation
  lacks libfuse's generated libfuse_config.h include path. All three to lane-4 (step-ci-green).
  The sharded full/asan/ubsan jobs were skipped, so the shard measurement is still pending.
- Lane-6 freed: 11.3 gap check (new implementer). Logging plan proposed to russ (absl semantics,
  no threshold override, ERROR visible by default, INFO lifecycle narrative, v=1 backing-reaching
  requests, v=2 every request, v=3 SQL); step to be recorded on his word.
- 25.3 logging (russ, 2026-10-08): plan approved with changes: WARNING visible by default too; no
  blanket rate-limiting rule (absl's LOG_EVERY_N/LOG_EVERY_N_SEC at the agent's judgement); no
  --log_dir; new style rule "return a failed Status or log, never both"; include russ's Status
  error-message guidelines (~/Style Guidelines for Accumulating absl__Status Error Messages.md),
  rewritten for our helpers. Dispatched to lane-5 (new implementer).
- 11.5 + 11.4 reported (lane-2) and reviewed (Opus): fix first for both. FINDING (protocol, HIGH,
  from kernel source): after a btrfs abort (or ext4 errors=remount-ro) the superblock is forced
  read-only, `sync_filesystem` returns 0 for a read-only sb and `syncfs` reports the errseq error
  once per open file, and SyncBacking reuses the fds opened at start: the first sync point after
  the abort fails, the next succeeds and clears the dirty set; after the operator's remount (which
  loses the unsynced mutations) and a restart the run ended clean and the lost objects are served
  as present. The test hid it with --sync_interval_sec=3600. Fix in 11.5 (test first, btrfs with a
  1 s sync interval + remount + restart): sync points fail on ST_RDONLY for a backing writable at
  start; start refuses a read-only superblock under a read-write mount; formal/README limitation.
  11.4's EEXIST reply narrowed to "the probe found the object" (ENOENT probe stays ENOENT); the
  recorder cuts record-failure runs as failed; the model keeps "a succeeded create replies ok" (no
  12.7b exception). Also from lane-2: 11.5 independently found and fixed the 12.6b syncfs hole on
  its base (dropped in the rebase since 12.6b landed first).
- 12.7b reported (lane-1): ReplyObservable (witness instants over the backing's states, errno
  classes ok/ENOENT/EEXIST/EAGAIN/EINTR, EINTR/EAGAIN only without a successful syscall: Phase 22's
  rule, accepted), two known_bugs (reply_after_failed_syscall, reply_unknown_as_negative), trace
  replies carry errno; no gap in the code; large 10.24M / nolock 8.22M states (nolock to eternal).
  Under Opus review.
- 25.3 docs merged first (7d8fab7, russ: "the logging style guide looks good"): style.md 1.6 (Status
  message rules) and 1.7 (levels, verbose levels, absl macros only, log-or-return), design.md
  "Logging", README flags. The code (reclassification, INFO/VLOG lines, tests) follows on the
  same branch; until it lands the docs describe behaviour slightly ahead of main.
- 11.3 reported (lane-6, 68626cc, under review): gap table 12 operation classes × 5 failure modes
  = 60 cells, 57 were gaps, all checked by `fault_recover_test` (ext4 medium ~70 s, xfs/btrfs
  large); window mode = dm-flakey 6 s up / 8 s down. Findings, no production change yet: on
  xfs/btrfs `open_by_handle_at` returns ESTALE for an inode the device cannot read, dcfs forgets
  the row and callers then get ESTALE/ENOENT for a file that exists instead of EIO (cache stays
  consistent; the reply is the question: reviewer asked for the minimal correct behaviour);
  after a failed write-back dcfs's recorded size can differ from what the fs reports after
  eviction (restart recovers it); ext4 EUCLEAN after a bitmap read error until remount; a failing
  btrfs inode read WARNs in btrfs_destroy_inode, so the btrfs variant pins inodes. Also:
  readdir_boundary_test timed out at 240 s on this host on origin/main (CI passed it on fee154d):
  host load suspected, log requested.
- 11.3 reviewed (Opus): fix first on the test side (cyclic flakey window never switched back and
  timing-sensitive; 27 of 60 cells vacuous under a no-op injection: fills answered from the fs's
  cache, setattr/setxattr "may succeed"; size-discrepancy exclusion; btrfs pinning hides the
  cold-inode path; fault_window without a self-check). HIGH → new step 11.3b: ESTALE from
  open_by_handle_at on an unreadable inode is answered as ENOENT for an existing file. Needs russ:
  btrfs_destroy_inode WARNING on a failed inode read (kernel bug to report). readdir_boundary_test
  hangs after `TEST mount PASS` on origin/main run alone (CI passed it on fee154d): bisect running
  in lane-3; push on hold until it is understood.
- 11.3 merged (18c077a) after one review round: window mode up 6 s / down 20 s then healthy,
  bounded by an uptime check; fills with no device reads and mutations that met no failure are
  SKIPs, each read-failing mode needs one erroring fill, README table with a cell-outcome column;
  the stale-size case asserts the inode stays dirty and is a README limitation; unpinned btrfs
  reproducer (`fault_recover_btrfs_unpinned_test`, large, `kernel_failure = "expected"`; run-qemu.sh
  tolerates exactly that WARNING only when the guest reports it, two verdict-test cases); the 23
  ESTALE/ENOENT cells on xfs print SKIP pointing at 11.3b. ext4 97 s, xfs 110 s, btrfs 107 s.
  Side effect of merging 25.3's docs first: `//man:flags_consistency_test` fails on main (README
  lists --v/--stderrthreshold/--minloglevel/--vmodule before main.cc links absl's log flags), so the
  25.3 code is part of the next push point. Lane-6 held for 11.3b (needs 11.5's backing.cc first).
- readdir_boundary_test "hang" closed: not a regression. On this host (load 12-15, ~50% steal in
  the guest) the test needs 215-230 s of guest time against the 240 s guest timeout; the serial log
  shows the creation loop (a shell fork + FUSE create per file, ~35 files/s) taking ~200 s, dcfs
  never in D state, dcfs CPU identical on fee154d and main (2960 vs 2973 ticks at the same point),
  MemAvailable 136-148 MB of 222, no reclaim; the initramfs growth costs 1.6 MB. CI's quiet runner
  passes it. names_test/names_random_test unaffected. Follow-up as 6.2 (lane-3): create the files
  with one testutil call, same N and assertions. Push hold for this lifted.
- CI fixes merged (340f559): coverage baseline 95.82 / 78.51 (8.1, the ratchet's own request from
  run 37725626313); TLC runner sets TMPDIR and -Djava.io.tmpdir under $TEST_TMPDIR (12.1; the race
  did not reproduce locally in 3 runs at 4 jobs, strace shows no TLC file under /tmp after the fix);
  mutate.py builds the target's compile prerequisites before the AST dump (26.5; reproduced by
  deleting libfuse_config.h; PrerequisitesTest). Survivors are findings in that job (exit 0 without
  --fail-on-survivor). 8.2d recorded: five survivors from the fee154d range. Push point = after
  25.3's code lands (flags_consistency_test). Lanes 4 and 6 held: 8.2d after 11.4 + 25.3, 11.3b
  after 11.5.
- 6.2 merged (be59465): readdir_boundary.sh creates its 7,500 files with one `testutil mkfiles`
  call (same names, N and assertions; two error-path checks); guest time on this loaded host 221-232
  s -> 135 s, dcfs's own create (~4.5 ms of daemon CPU each) now dominates. Lane-3 free (held:
  mutation-expansion step pending russ's decision).
- 26.5d (russ, 2026-10-08): expand mutation testing with our own tooling (operators from the
  literature, arid-node suppression, sampling, an equivalent-mutant data file, per-operator
  reporting); no Mull/Dextool. Dispatched to lane-3 (new implementer).
- 23.8 (russ, 2026-10-08): access times. Files: no prediction; `fstat` on the held backing fd at
  FLUSH/RELEASE/GETATTR-while-held (an open file pins its inode: no disk I/O), row dirty until the
  sync point. Directories and symlinks: stamped in the cache, never written to the backing (the
  only way to move a directory atime like a read is a read; utimensat bumps ctime). Alternatives
  in notes/atime-alternatives-2026-10-08.md. Dispatched to lane-4 (new protocol agent).
- 23.9 (russ, 2026-10-08): LINK of a removed object must answer exactly what the backing answers
  (ENOENT for an unlinked file, EPERM for a directory, success for O_TMPFILE), not ESTALE; README
  paragraph rewritten. Queued after 23.8 in lane-4.
- 25.3 code merged (76ab735, five commits): reclassification (12 WARNING->ERROR, 1 ERROR->return,
  1 WARNING->return with context, 1 INFO->WARNING, VLOG(2) SQL -> VLOG(3), 2 log-and-return pairs
  removed, 45 first errors capitalised); new `ProducedByDcfs`/`DcfsErrnoToStatus` (dcfs-origin errno
  payload) so the reply handler logs ERROR only for errors dcfs produced and a forwarded backing
  errno goes to --v=1; INFO: start line, recovery and probe summaries, sync point rows/duration,
  clean shutdown, first backing access after idle (idle = 12 x sync interval between
  backing-reaching requests); --v=1 per backing-reaching request, --v=2 every request + reply;
  tests in channel/dir_cache_fs/status/backing tests and lifecycle.sh (threshold); failing-first
  via a tests-only branch on the old main; budgets and trace shards unchanged; flags_consistency
  green (the four absl flags are a list under the README table). Presubmit 293 + 2. PUSH POINT.
  Post-merge Opus review requested (the agent's own design of ProducedByDcfs). Lane-5 held until
  the dcfs/ editors (11.5/11.4, 23.8) land; then 7.3/7.6/7.7.
- 23.8 (lane-4) found two gaps in the decided mechanism and got decisions (notes file updated):
  mark every attribute record of a held inode dirty; close the behind window with a dirty mark at
  each cold read-only open (no fsync) and sync points keeping held rows; atime-only dirty rows do
  not drive sync points (lazytime). The agent stopped once on the classifier (a tracked-file
  `git checkout`), resolved by hand-editing the merge markers; rule reinforced: commit WIP, never
  stash across a rebase.
- 25.3 post-merge review (Opus): no invariant or crash-safety problem; three MEDIUM silences
  (a failed success reply logged only at --v=1 and the real status lost behind "already replied";
  process errnos EMFILE/ENFILE/ENOMEM/EBADF/EFAULT counted as forwarded; CreateChild's lost line for
  a backing-errno record failure) plus idle-line placement/threshold, hot-path sync INFO on fsync,
  level inconsistencies, LOG_FIRST_N for failed recovered rows, IgnoreError refreshes, wording, a
  vacuous test, doc mismatches. All sent as 25.3b to lane-5 (same agent).
- CI on f1a426d (run 37799862872, russ's push): subjects, osv (with the informational glibc scan),
  fast 12 min, mutation-changed 19 min (first green run of the job), reproducible 19 min, presubmit
  21 min green. Sharded jobs with restored caches: full 17-24 min, asan 23-26 min, ubsan 19-25 min
  per shard (the 2-2.7 h asan estimate assumed cold caches; keep the 240 min limit for cold runs).
  Four failures, none a protocol bug: coverage (dcfs lines 95.65 < baseline 95.82: 25.3's new code
  partly uncovered; branches rose to 78.68) -> 25.3b; cancel_test on full/asan/ubsan
  (`checkpoint-logged FAIL`: the guest script greps a log line 25.3 moved) -> 25.3b; ubsan(0)
  syscall_traces_test cold-lookup with six extra pipe2/fcntl/write/close syscalls (a UBSan report
  or runtime init under the 7.1b compiler-rt) -> lane-6 investigator; asan(0) cancel_test guest OOM
  at 384 MiB and cancel_inventory_test's dcfs dying (no `asan_mem` on the two 20k-entry cancel
  tests) -> lane-6. Next push point after 25.3b and the sanitizer fixes land.
- Decision (russ, 2026-10-08): no full pre-push gate for now (plain + asan + ubsan + coverage +
  mutation locally, 3-4 h at current load, is not worth it while development is this fast). Push
  points stay as they are: every merged branch green on its tiers, CI catches the sanitizer-only,
  coverage and large-tier failures. The existing rule stands: a step touching C++ runs its own
  targets under --config=asan. **Once dcfs has launched and development slows, russ wants green
  pushes:** then a push point becomes a sync point and the candidate gets the CI checks locally
  first (a `tools/prepush.sh` calling the same `.github/ci/*.sh` scripts, result and SHA logged).
- 11.5 + 11.4 merged (9374050, seven commits) after two Opus rounds: read-only guard (sync point
  fails EROFS after syncfs on ST_RDONLY for a source writable at start; start refuses a forced-ro
  superblock via mountinfo, ext4 emergency_ro included; fsync golden/budget 11->12), EEXIST only when
  the probe found the object, recorder cuts record-failure runs as failed, orphans documented.
  Merge hazard carried to lane-1 (12.7b rewrites the same recorder closure and accepts a create's
  EEXIST unconditionally: 11.4's syscall_ok cut must stay ahead). Follow-ups 11.5b/11.4b (findings
  2-6) and then 11.3b to lane-2. Presubmit test/qemu+dcfs 169 + 2, fast 193 + 2.
- 26.5d reported (lane-3): eight operator classes with 29 tests on a compiled fixture, arid rules,
  one-per-line + per-function sampling with a seeded stable-id hash, `changed --max-mutants 30`,
  equivalent.txt (11 End() deletions after a refused checkpoint), per-operator report + survivors by
  function, weekly job seeded by run number with a job summary, a baseline run first; backing.cc
  added to scope. Sample sweep: 14 survivors (8.2e), several protocol-level (ProbeChild and
  RecordNewChild present/absent swaps, FsyncDirFd error->ok, StartRun || -> &&). Changed mode over a
  real range: ~16 min per survivor at this load, so a `--time-budget` (120 min) is being added.
  Under Opus review.
- 25.4 (russ, 2026-10-08): test-writing rules into style.md: when to extend an existing test vs
  write a new one (cost = boots and setup; debuggability = name + expectation message), EXPECT
  not ASSERT unless required, matchers for readable failures. Docs commit to lane-3; the 8.2e
  tests follow them; existing tests convert in 7.5/7.5b.
- 25.4 merged (d5eb185): style.md tests section: extend vs new (cost model + debuggability limit,
  examples ReleaseReportsWhetherTheOpenCouldWrite and write.sh's check_cold), EXPECT over ASSERT
  with the two allowed ASSERT cases, matchers over booleans (status matchers already in use: IsOk
  ~900, IsOkAndHolds ~460, StatusIs ~130; three EXPECT_TRUE(x.ok()) and the ASSERT_THAT balance
  are 7.5's job).
- Sanitizer CI fixes merged (2b18ec7): the ubsan-only cold-lookup diff was UBSan's vptr check
  probing a vtable prefix through a pipe (pipe2, fcntl x2, a 16-byte write, close x2; compiler-rt's
  IsAccessibleMemoryRange; no report), now its own strace kind `sanitizer` under
  `DCFS_SANITIZER=ubsan` only (strace_lib_test covers plain/asan/ubsan and a daemon's own pipe);
  cancel_test and cancel_inventory_test get `asan_mem = 1216` (measured peak 733-766 MiB under
  ASan at 1536; reclaim 0 at 1216) and plain cancel_test mem 256 -> 384 (kswapd scanned 18k pages
  at 256). Remaining before the push point: 25.3b (coverage lines, cancel_test's grep). Lanes 3 and
  6 held until the dcfs/ editors land.
- 26.5d reviewed (Opus): fix first. HIGH: per-function sampling always took the alphabetically
  first operators (status-return never sampled in 69 of 84 functions: the 8.2e list is provisional);
  header-macro nodes become mutants at header offsets (false survivors); the error-builder arid rule
  never matched absl's builder (MaterializeTemporaryExpr); 16 leaked Bazel output bases (~16 GB)
  under ~/.cache/bazel from scratch trees, SIGTERM skips cleanup; flaky kills and unrelated build
  failures misclassified; sampling before suppression; nth re-targeting; arid scan in comments;
  exit codes. All sent back with a cleanup of the leaked bases and a re-sweep.
- 12.7b fix round reported (lane-1, tip aacb290 on 703e0fc): traces carry the lookup answer
  (`LookupAnswered`, "ans") and the errno actually sent (`Replied`, `FuseRequest::errno_sent`),
  `T_Reply` strict and shape-checked (`QueryKinds`), ReplyObservable in MC_large (10.24M states,
  1 h 04 here, ~2000 s on the runner) and MC_interrupt_muts2, nolock eternal, the vanished-name
  create cut as out-of-band behind 11.4's failed cut, two new rejected trace fixtures. 111 formal/
  trace tests, fast 199 + 2. Short second review pass requested before merge. Note: small_test and
  known_bug_rename_stale_source_test exceed 300 s on this host at load 13-15 (935k states).
- 12.7b merged (5094b31, seven commits) after the second pass ("merge"; one nit -> 12.7c: ECOMM
  from ~FuseRequest recorded as errno 0). Lane-1 next: 12.7c + 12.11 (same agent).
- 12.7c + 12.11 reported (lane-1, 161b85d): ECOMM sentinel (`kNotReplied` -> unexplained line);
  `attrchange` request of D in dcfs.tla (phase 1, syscall, End as AttrChangePhase3, refresh fill,
  reply; checkpoint before the syscall), known_bugs/attr_change_end_skipped (GuardsBalanced),
  Trace.cfg checks GuardsBalanced, the recorder maps directory setattr/setxattr/removexattr/ioctl
  to it (the `dir-attrs` cut is gone), four production call sites emit the mutation syscall events.
  Limit: dcfs.tla models only D, so a skipped End after copy_file_range/fallocate/a file's setattr
  is still caught only by the harness tests (8.2); a per-file attribute trace would be 12.11b.
  Counts: small 936k -> 1.48M (timeouts to `long` requested), recovery 40k, liveness 147k,
  interrupt 281k; large/nolock unchanged. Short Opus review requested.
- 11.5b merged (ff5e35b): refusal and EROFS messages cover a remount,ro of another mount of the
  superblock (README/design.md), the "ro" check requires the guard's line and forced=yes, the
  read-only-at-start exemption ends at the first writable fstatvfs, `probed` is set right after the
  statx that saw the object (CreateWhoseObjectWasSeenRepliesEexist -5 -> -17 first), emergency_ro
  dated 6.15. 11.3b reported (lane-2, 2263c86, under review): ESTALE from open_by_handle_at is
  checked against one cached name through the parent's fd, comparing HANDLES (25.5's replacement
  test caught an inode-number version); same handle or an unreadable parent -> EIO with the row
  kept unknown; a readable parent's ENOENT or a different handle -> ESTALE as before; 18 xfs SKIP
  cells became FAIL-then-PASS; the model cannot express a handle open, so a formal/README limitation
  names 12.7b's reply_unknown_as_negative as the class.
- 12.7c + 12.11 merged (e05b345, three commits) after a "merge" review (nits: cite the VFS lock
  for leaving attrchange out of nolock or add it to MC_interrupt_nolock; GuardsBalanced overlaps
  DbMatches). small_test and known_bug_rename_stale_source_test to `long`. Lane-1 free: 12.8 next.
- 12.8 dispatched to lane-1 (new protocol agent): ordered backing-state sequence with a reordering
  regime constant (sequential / metadata-prefix / ext4-weak), directory-fsync switch, Ferrite litmus
  configurations; stop-and-report on any real gap.
- russ, 2026-10-08: Phase 15 (mount.dcfs wrapper) may start out of phase order: 15.1 + 15.2 to
  lane-6 (new implementer). Cleanup of the 16 stale Bazel output bases approved (lane-3 after its
  re-sweep). Answers given: sources without a filesystem UUID stay with Phase 14 (need another
  identity plus handle-free access from Phase 13; FUSE backings lack name_to_handle_at);
  fsync-per-create explained (durable phase 1 per operation vs riding on the parent's dirty row:
  a protocol change; ~1 ms per create on an SSD) and left to russ; glibc: proposed bumping the
  sysroot to Debian 13 (glibc 2.41) with the OSV scan gating on fixable records and ignore-with-
  expiry for the rest, awaiting russ's yes.
- 23.8 + 23.9 reported (lane-4, bfec268 / 80ed413, under Opus review): held fill (one statx of the
  held fd at FLUSH/RELEASE/attribute replies while held) recording attributes and marking the row
  dirty with the new atime-only reason (schema v6); any attribute record of an open file marks it
  dirty; cold read-only opens mark dirty without fsync or guard touch; sync points keep open files'
  rows; `dirty.atime` separate from `dirty.any`; directories/symlinks stamped cache-only with the
  later stamp kept; model file F outside AllKinds (MC_atime 111k, MC_atime_concurrent 67k; three
  known_bugs, two limitations); checker rule `open-file`; snapshot with atime. BUG found and fixed
  by the guest tests: ClearDirty set the in-memory dirty flags inside a transaction that could roll
  back. 23.9: LINK of a removed object forwards linkat(AT_EMPTY_PATH)'s answer (ENOENT/EPERM/ok).
- russ, 2026-10-08 (identity): keep handles only; drop the filesystem-UUID requirement when Phase 14
  runs and use mount point + statfs f_fsid as a sanity check instead of a guarantee; no new
  identity marker; backings whose root gives no handle are refused with a clear message. Recorded in
  phases/14. Cleanup of the stale output bases confirmed.
- russ, 2026-10-08: 1 ms per create is irrelevant, but a directory-level dirty set is wanted as a
  simplification: recorded as 23.10 (after 12.8 and 23.8 merge; protocol agent). glibc: bump the
  sysroot to Debian 13 (7.1c, next free lane; osv shipped-deb scan becomes gating with
  ignore-with-expiry for records no release fixes).
- russ, 2026-10-08: identity uses the filesystem UUID when available; mount point + f_fsid only as the fallback.
- russ, 2026-10-08: two style rules (25.5): one abstraction over variants (the identity's UUID /
  mount+fsid behind one object, never if-branched at use sites); avoid branching, inject
  dependencies and use fakes instead of test-only conditionals. Docs commit queued on lane-3.
- 25.3b merged (b3355fc, four commits): the 13 review items (DcfsErrnoToStatus for failed success
  replies and process errnos, MarkProducedByDcfs for CreateChild's probe failure, idle line at the
  request's first backing call with kIdleThreshold 60 s, fsync/fsyncdir sync points at VLOG(1),
  the duplicate VLOG deleted, level fixes, LOG_FIRST_N(ERROR, 10) for failed recovery probes,
  NoteBestEffort for the best-effort refreshes, wording, the vacuous test replaced, six coverage
  tests, docs) plus the LogPhase3Failure NotFound exclusion (confirmed: a row gone mid-Rename logged
  ERROR); CI items: cancel.sh asserts the interrupted request through --v=2 (the 25.3 capitalisation
  had also broken the grep), coverage baseline 95.97 / 79.22 from the gate on this tree. Fast 201 +
  2; presubmit green before the last rebases, not rerun on the final tree. PUSH POINT. Lane-5 free:
  7.1c (sysroot to Debian 13) dispatched.
- 23.8 + 23.9 reviewed (Opus): fix first for both. 23.9: LinkRemoved's success path is reachable
  (a closed never-linked O_TMPFILE held by nodeid) and records the new row undirty with a new
  nodeid and an interruptible re-resolve after the syscall. 23.8: a mutation row on a file held
  read-only keeps sync points running until release; recovery probes every atime-only row (a
  read-mostly workload pays for weeks of reads after a crash; the model's RecoverF only invalidates
  attributes): atime-only rows older than the kernel's dirtytime expiry will drive a sync point;
  concurrency configs missing; Destroy makes a shutdown with read-only opens clean against the
  model's text; checker self-checks missing; HeldFdOf includes written_'s O_PATH fd (statx per
  GETATTR of written files); DropAtimeStamp before the syscall. Sent back to lane-4.
- russ, 2026-10-08: the no-branching rule is "discouraged, sometimes necessary"; add the numeric-property technique (zero as absent, INT_MAX to turn a limit off) with its caveat.
- CI on 4bf7182 (run 37841442160, in progress): subjects/osv/fast/presubmit/reproducible green;
  coverage FAIL twice over: the ratchet fired on a RISE again (95.99/79.43 vs 95.97/79.22), and
  names_random_test(_ext4) under the coverage config end with ALL-TESTS-PASSED but no MEM line
  (init's coverage path reboots before the memory sampler). Decision: the gate fails on drops only
  and prints the suggested bump on a rise (8.1b); both to lane-3's queue after the 25.5 docs.
- russ, 2026-10-08: failing on a coverage rise is not worth it; drops fail, rises are noted, the orchestrator bumps the baseline at push points (process.md).
- russ, 2026-10-08: 26.14 quiet kernel in the guests (writeback/reclaim/timer spontaneity removed
  by sysctls, one vCPU for non-stress tests, every kernel event a test needs forced explicitly) plus
  a two-run coverage diff on one commit as the determinism measurement; QEMU -icount record/replay
  kept in the back pocket (TCG only).
- CI on 4bf7182 (run 37841442160) finished: subjects, osv, fast, presubmit, reproducible green;
  sharded jobs 17-27 min each with caches, eight of nine green. Failures: coverage (the rise ratchet
  + names_random under coverage: both queued to lane-3 as 8.1b/6.2); mutation-changed exit 1 on 4
  survivors of 30 (the job passes --fail-on-survivor: to be dropped in 26.5d, survivors to the job
  summary; the four recorded as 8.2f); asan (0): enospc_backing_test_btrfs guest OOM at 384 MiB
  (asan_mem to be measured and set by lane-2 after 11.3b).
- 11.3b merged (2301c18): an ESTALE from open_by_handle_at is checked through the parent's fd by
  one present name, else by up to 16 unknown names (a failed mutation leaves names unknown); same
  handle, an uncheckable name, or an unreadable parent -> EIO with the row kept and attributes
  unknown; a readable parent's ENOENT, a different handle, or no name at all -> ESTALE as before.
  fault_recover xfs: 0 FAIL cells (was 18 ENOENT/ESTALE replies for existing files). OpenNode runs
  inside Setattr's phase 3, which is the reason no checkpoint precedes the extra calls. 11.4c merged
  (f2faccc): asan_mem 832 for enospc_backing (btrfs peaks 486 MiB), 576 for enospc_cache and
  fault_shutdown. Lane-2 free: 26.14 quiet kernel to a new investigator.
- russ, 2026-10-08: 6.5 test performance scrub (dedicated investigator, measure first, no weakening, before/after per change); first free lane.
- Lane-3's queue merged: 25.5 (2771f62, style.md 1.10), 8.1b (803ecab: the coverage gate fails on
  drops only, a rise is a NOTE in the job summary; guest/init reads the MEM line before killing the
  sampler, the inferred race behind names_random's missing MEM line under coverage: not reproduced
  locally), 26.5d (0f73230, 11 commits: all ten review items, seeded operator order, header-macro
  rejection, absl builder arid rule, scratch output base under the scratch dir with cleanup in
  finally and SIGTERM handled, flaky reruns, exit 2 for tooling, time budget, equivalent.txt with
  nth, the per-push job reports survivors in the step summary and never fails on them). Cleanup: 16
  stale output bases expunged plus two /tmp dirs, ~15 GB freed (385G -> 370G used). Re-sweep: 30 of
  343 sampled, 18 killed / 8 survived / 3 invalid / 1 flaky -> 8.2e replaced. Lane-3 free: 6.5 test
  performance scrub dispatched (new investigator).
- 15.1 + 15.2 reported (lane-6, 0f92881, six commits, +2958/-277, under Opus review): argv[0]
  dispatch, `dcfs.` option split (fstype none/bind/native/autodetect, ro, foreground, cache_db,
  fuse_opt, remount, the absl flags), capture of native/bind mounts in a private mount namespace via
  /bin/mount + open_tree(OPEN_TREE_CLONE) handed back over a socketpair, own daemonisation with a
  readiness report after the first FUSE_INIT, syslog sink for the daemon (stderr in foreground),
  remount of the dcfs mount only, non-root and file-mountpoint refusals, `--source` removed from
  every script and the bench; new syscalls:: wrappers; syscalls_process non-testonly with execv
  allowed there only. `mount -t dcfs` through mount(8) is a SKIP in the busybox guest (15.6's
  systemd/util-linux guest covers it). Deferred: 15.3 cache path/identity, 15.4 stubs, 15.5, 15.6,
  15.7, allow_other default, kernel feature checks, -V revision.
- 15.1/15.2 reviewed (Opus): fix first (HIGH: ForcedReadOnly looks in dcfs's own mountinfo, which
  never lists an OPEN_TREE_CLONE mount, so captured mounts skip the 11.5 refusal and then exempt
  every sync point; relative paths after chdir; /tmp staging; untestable readiness; exit statuses;
  INFO to syslog by default; plus coverage, guest-check and doc items). Sent to lane-6. Confirmed
  for russ: all logging, syslog included, goes through Abseil (SyslogSink is an absl::LogSink).
- CI on 9a67552 (run 37853130640): everything green (coverage, mutation-changed with survivors in
  the summary, presubmit, reproducible, full x3, ubsan x3, asan 0 and 2) except asan (1):
  bench_full_test's guest out of memory at -m 3072 MiB after 181 s: to the 6.5 scrub (measure dcfs's
  VmHWM under ASan vs plain; size or finding).
- 23.8 + 23.9 fix round reported (lane-4, 9220139 / d18df06): all ten items done (RecordRelinked
  reinstates the row and nodeid for a linked tmpfile; held-only mutation rows become atime-only
  after the syncfs; recovery only unknowns atime-only rows' attributes and skips their probe;
  atime-only rows older than dirtytime_expire_seconds drive a sync point; MC_atime_crash passes on
  the real model, atime_fill_no_touch is a large target; death tests for the checker rules;
  OpenFdOf only; DropAtimeStamp after the syscall; failed held fill -> unknown; read budget 20 -> 21
  for the cold open's read-before-write). Rebase onto 40d779d requested, then merge. The spend
  limit stopped every agent at ~23:15 and was reset by russ; all six resumed 2026-10-09.
- russ, 2026-10-09: `[[nodiscard]]` is permitted (style rule added to 25.5).
- 12.8 reported (lane-1, 15f5ad1, under review): bSeq (ordered backing states since the last
  durable point) replaces bOpts; Reorder ∈ {seq, metaprefix, ext4 (Ferrite Def. 7)};
  DirFsyncPersistsFiles switch; write/fsync kinds in the new configs; CrashRefines; Ferrite litmus
  programs (holds: atomic replace/create-via-rename under seq, directory fsync through dcfs under
  ext4; expected violations as limitations; known_bugs/sync_by_file_fsync). No gap: a non-dirty
  database survives a power loss only with a backing synced since D's last change (durable phase 1
  + clear-after-syncfs): the property 23.10 must keep. Existing counts unchanged under seq.
- 12.8 reviewed (Opus): merge after wording (ext4 regime is an over-approximation of Definition 7;
  CrashRefines follows from CrashSafe; litmus wording). The review states the exact condition for
  23.10 (file rows never dirty: every written file needs a dirty parent recovery re-lists, incl.
  hard links elsewhere, NFS-handle opens without a parent dentry, unlinked tmpfiles; the model needs
  a file inode and a second directory): written into 23.10.
- 15.1/15.2 second pass: fix first (three util-linux breakers introduced by the fixes: `rw` refused
  for none, native `ro` refused on remount, non-path native specs rewritten; plus exit-status
  classing, bind remount flags, LastErrorSink window, /proc/sys missing, nodiscard on new headers).
  Sent back; deviations recorded in phases/15.
- 23.8 + 23.9 merged (8ea5743, five commits, rebased with commit messages recreated via
  commit-tree to drop the stale claims; one extra commit: the held fill that leaves a stale atime
  logs ERROR, keeping 25.3b's AFailedAtimeRecordIsLoggedAtError rule). Fast 204 + 2, trace shards,
  atime/removed guests ok. Lane-4 free: 8.2e + 8.2f survivor tests dispatched (new implementer).
- 12.8 wording round done (12ee27e: ext4 regime stated as an over-approximation, CrashRefines
  follows from CrashSafe, litmus wording, design.md cites Ferrite's specification; MC_crash_ext4 /
  metaprefix gain getattr + attrchange: 357k / 292k states). The rebase onto b845249 conflicts in
  dcfs.tla with 23.8's model F (two model changes touching the backing state and crashes): handed to
  the lane agent to merge F's crash behaviour onto bSeq; orchestrator's attempt aborted cleanly.
- 15.1 + 15.2 merged (106e16e, eleven commits, three Opus rounds). Third round: `none` accepts the
  libmount options (rw, defaults, nofail, _netdev, noauto, user...), a remount ignores native options
  with a WARNING, native specs are rewritten only when they exist relative to the cwd and the fsname
  keeps the spec as written, exit 1 only for usage/non-root (MarkUsageError) and 32 otherwise, the
  bind ro remount keeps nosuid/nodev/noexec, LastErrorSink only until INIT, a clear error without
  /proc/sys, [[nodiscard]] on the new headers and syscalls.h wrappers. Fast 214 + 2. Note: the
  tests-first commit 304cf9e does not build on its own (quoted failures in its message). The real
  mount(8)/libmount path is untested until 15.6. Lane-6 free.
- 15.6 dispatched to lane-6 (new investigator): a released Debian cloud image pinned through
  Bazel, systemd as PID 1, mount.dcfs through real mount(8)/libmount, fstab lines mounted by
  `mount -a` and by systemd's units at boot, remount via mount.fuse.dcfs, exit statuses as mount(8)
  reports them, journald logging, a reboot with a warm cache, nofail on a missing backing. PUSH
  POINT at 6fc3064 (since 9a67552: 23.8/23.9 atime + LINK, 15.1/15.2 wrapper, plan).
- 2026-10-09: 7.1c reported (lane-5, two commits): libc6/libc6-dev to trixie 2.41-12+deb13u4 from
  the same snapshot, `extract.py usrmerge` adds the merged-usr lib/lib64 links (libc.so's linker
  script names /lib/x86_64-linux-gnu/libc.so.6), four `<runtime>` banned-symbol allows for members
  glibc 2.41's libc.a links in (popen, posix_spawn_file_actions_adddup2, __fprintf_chk,
  __sprintf_chk), the shipped-debs OSV scan gates with 21 ignores expiring 2027-01-06 (none fixed in
  trixie). Review passed; sent back for a rebase onto 4b204d9 and a fast-tier run, the wrapper's
  first compile against the new glibc.
- 8.2e/8.2f reported (lane-4, one commit, test-only): 11 of 12 survivors now die (the 12th,
  CreateChild End, was already in equivalent.txt; 1751 was already dead on today's tree and got a
  contract test anyway). Review passed under 25.4; sent back for a rebase over the atime/LINK test
  additions and a fast-tier run. Not yet pushed: GitHub main is still 9a67552.
- 12.8 merged (638590d, one squashed commit; pre-rebase tip kept in lane-1 as step-12.8-pre-23.8).
  Rebase over 23.8's model F done by the lane agent (dcfs.tla re-applied by script; Trace.tla,
  BUILD.bazel, README kept both sides); F now stamps from bSeq; CrashRefines covers D only. Counts
  under seq match main exactly with one worker; with two workers MC_atime varies by tens of states
  (23.8's VIEW keeps whichever representative a worker reaches first). formal minus large/nolock 132
  pass, trace shards pass, fast 217 + 2. Orchestrator rebased the last two commits (design.md only
  in common, clean) and merged on the agent's run: formal/ was untouched on main in between. Lane-1
  free: 23.10 directory-level dirty set dispatched (new dcfs-protocol).
- 8.2e + 8.2f merged (fd8f607, one test-only commit; rebased by the agent onto 4b204d9 with no
  conflicts, then by the orchestrator over 12.8, formal-only). Fast 214 + 2 on the agent's tip.
  Lane-4 free.
- 17.1 dispatched to lane-4 (new investigator): xfstests' generic group against dcfs through the
  real mount.dcfs (its prerequisite from Phase 15 is the wrapper, merged), runtime pinned through
  Bazel (Alpine packages for bash and the tools, BUILD.xfstests extended), per-backing expected-
  failure lists with a known-bad self-check, large tier sharded like pjdfstest. Test-only: dcfs
  bugs it finds are findings for later steps, since 23.10 owns dcfs/ now. Told to stop early with a
  measurement if the busybox guest cannot carry xfstests (the alternative is 15.6's Debian guest).
  Chosen over Phase 13/16 (both need dcfs/ or the stubs) and 7.3-7.7 (tree-wide, not a quiet point).
- 7.1c merged (dea789b, two commits; the agent rebased onto 4b204d9 with no conflicts, fast 214 + 2,
  banned_symbols and hermetic gates pass, reproducible build byte-identical; the orchestrator
  rebased over 12.8 and 8.2e/f, no overlap). Every lane's next rebase re-extracts @dcfs_llvm: the
  background-fetch rule in process.md now names 7.1c too. Lane-5 free: 15.7 docs next.
- 15.7 dispatched to lane-5 (new implementer): README usage/fstab/remount/exit statuses/daemon
  logging/operations/log-message sections and design.md's wrapper section from the code and man
  page as built (not the plan text where they differ), packaging/dcfs.service removed; 15.5 and
  15.3 items left out rather than promised; systemd behaviour only as far as the code and man page
  show it (15.6 confirms the rest).
- 26.14 reported (lane-2, seven commits on 40d779d): quiet-kernel sysctls in guest/init with the
  reasons, vfs_cache_pressure kept at 100 (at 1 drop_caches dropped 2 of 1091 dentries and three
  tests lost their FORGETs: a finding worth the step by itself), ASLR left (Abseil hash seed order
  only), `cpus` knob default 1 with CONCURRENT_CPUS=2 for stress/cancel/pjdfstest/bench, fast tier
  1701 s to 1282 s uncached under load 11-13, coverage_diff.py + test, determinism note. Review
  passed; sent back for the rebase onto 93384a9 (BUILD and trace.bzl conflicts expected), the llvm
  refetch, fast tier plus the new main targets. Three follow-ups queued in phases/26 (26.14b
  cleanup race in the test scripts after 6.5; 26.14c a 4294967295 branch count at
  dir_cache_fs.cc:817; 26.14d the ARowGoneDuringPhase3 flake with a pid-0 WARNING).
- 15.7 merged (50fee50, two commits, docs only; fast 227 + 2, man tests green). Every claim checked
  against the code or man page by the agent; the systemd/util-linux statements it could not verify
  are listed in phases/15 for 15.6 to assert. Lane-5 free.
- 26.15 added and dispatched to lane-5 (new investigator): standalone dcfs-free reproducers, the
  upstream code path, a lore search for existing reports, and draft mails for the ext4 casefold
  tune oops and the btrfs_destroy_inode WARNING, for russ to send. Chosen for the free lane because
  15.3/Phase 14 (schema, startup) would collide with 23.10's code half, and 12.9 with its model.
- 26.15 BLOCKED (lane-5, no commits, branch step-26.15 at fdd1d99): the agent's first write (a
  heredoc creating tools/kernel_bugs/ext4_casefold_tune/*.c and a BUILD file) was refused by the
  auto-mode safety check ("Auto mode could not evaluate this action ... because of earlier
  conversation content"), and every later Bash call in that agent, read-only ones included, got the
  same refusal. It stopped and reported, per the standing rule; not re-dispatched (a fresh agent
  with a reworded prompt would be a workaround of a permission denial). Needs russ: run 26.15 in a
  session or mode where writing a kernel-oops reproducer is allowed, or do the reproducers by hand
  from the design below. Read-only findings worth keeping:
  - ext4: on 6.18.55-0-virt the oops is `BUG: kernel NULL pointer dereference, address: 0x18`,
    `RIP: utf8nlookup+0x14/0x240`, task `ls`, chain utf8nlookup <- utf8byte <- utf8_casefold <-
    ext4fs_dirhash <- htree_dirblock_to_tree <- ext4_htree_fill_tree <- ext4_readdir <- iterate_dir.
    Source at v6.18 and at ~/Sources/linux HEAD (d530980dfbb7, v7.3-rc4-607 + the fuse patch):
    ext4_ioctl_set_tune_sb defaults the encoding to UTF8_12_1 when it enables casefold, but
    ext4_sb_setparams writes the feature bit and encoding only to the on-disk superblock;
    ext4_encoding_init (super.c) is called only at mount; ext4_ioctl_setflags's +F check tests only
    the feature bit; ext4fs_dirhash (hash.c:318-329) dereferences sb->s_encoding with no NULL
    check. Still present at that HEAD by source reading; lore not yet searched. Fix ideas (prose):
    refuse the casefold part of the tune ioctl on a mounted fs, or load the encoding after the
    ioctl, or test s_encoding instead of the feature bit in setflags/dirhash.
  - btrfs: the WARNING is `fs/btrfs/inode.c:8047 btrfs_destroy_inode+0x224/0x290`, seven times
    within 80 ms; at v6.18.55 line 8047 is `WARN_ON(inode->csum_bytes)` (delalloc_bytes and
    new_delalloc_bytes at 8045/8046 did not fire). The tree's attribution to
    btrfs_read_locked_inode's error path (fault_recover.sh's pin_inodes comment, log) is UNPROVEN:
    csum_bytes is not normally non-zero for an inode whose read failed; candidates are an inode
    evicted with csum accounting left after failed writes (the writer in the error-writes/error-io/
    dead modes) or the iget_failed path (inode.c 4180-4190). run-qemu.sh keeps only the cut-here,
    WARNING and Call Trace header lines, so the reproducer must print dmesg itself.
  - Design for whoever resumes: tools/kernel_bugs/{ext4_casefold_tune,btrfs_failed_inode_read}/
    with reproduce.sh (POSIX sh, scratch device, MKFS=0 to skip mkfs), README, a small static
    helper (ext4: `enable <mountpoint>`, `mark <dir>`; btrfs: name_to_handle_at/open_by_handle_at)
    and BUILD; mkinitramfs.sh needs an additive case installing tools/kernel_bugs/* under
    /kernel_bugs/ (outside the prompt's file list: flagged); a generic guest/kernel_bug_repro.sh
    using lib.sh's `disabled` helper; two manual qemu_test targets with kernel_failure="expected"
    (ext4 vdb 64M; btrfs vdb 320M + dm_flakey, padded with ~6000 files). .scratch/kernel-src/ in
    lane-5 holds read-only copies of the v6.18, v6.18.55 and HEAD files read.
- 26.14 merged (b0d57de, seven commits; clean rebase, fast 229 + 2, new main targets green under the
  quiet init). Lane-2 free: 26.14c (the 4294967295 branch count) dispatched, a bounded investigation
  that touches no dcfs/ code. 26.14b waits for 6.5 (same scripts); 26.14d waits for 23.10's test
  changes to land (same test file).
- 23.10 model half reported (lane-1, d49aa87): the simple rule ("mark the parents the cache knows")
  is unsound in all three review cases; the sound rule needs a per-file home directory column,
  which rename and rmdir maintain and recovery re-stats by. Three named conditions with premise
  tests, nine MC_dirset configs, five known_bugs, all properties hold under the three regimes;
  formal small+medium 146 pass. Finding: 23.8's FSnapView is not an exact view. Opus review
  dispatched (soundness of the home design incl. rmdir/rename of the home, fidelity of S2, the ext4
  tightening, FSnapView, the trace plan) with a candid comparison for russ: home design vs per-row
  dirty status quo vs a hybrid. NEEDS RUSS: whether 23.10 with a home column is still the
  simplification wanted. Code half on hold until both.
- 23.10 review (Opus): not ready for code; nine findings in phases/23 (home movement unmodelled and
  the rmdir re-home rule unsafe as written, a concrete power-loss trace; one dirty reason discards
  a directory's listing on any file write; recovery scope undecided and design.md overstates it;
  FSnapView exactness to bisect). Reviewer's answer to russ's question: directory-only dirty bits
  are not available as stated; recommends the status quo plus a born-dirty create (no fsync per
  create, no schema change, precise recovery) over the home design. NEEDS RUSS: (a) home design
  after findings 1-4, (b) status quo and close 23.10 as investigated, or (c) born-dirty create.
  The 23.10 branch stays in lane-1 unmerged. 23.8b dispatched to the same agent: the view
  exactness bisection through Bazel targets (the reviewer was denied running TLC outside bazel
  test and stopped; the bisection is normal lane work).
- 26.14c merged (8f0d3c2, one commit; clean rebase; tools and harness tests green). Cause: plain
  profile-counter increments losing updates between the daemon's threads; fixed with
  `-fprofile-update=atomic` under coverage, the gate rejects artifact counts, docs/coverage.md
  exists now. Deviation noted by the agent: one early bazel coverage call was piped through tail
  (not in an && chain). Lane-2 free.
- Phase 13 (13.1-13.3, connected backing fds) dispatched to lane-2 (new implementer; the phase
  says Opus not needed): OpenNode chooses by cache state, present dentry -> openat under the
  connected parent, else handle; out-of-band mismatch WARNING + name unknown + handle fallback;
  strace goldens/budgets updated deliberately with before/after; told to keep off the dirty set,
  sync points, recovery and formal/. Started because 23.10's hold frees OpenNode; Phase 14 follows
  it per the phase order.
- 26.14d dispatched to lane-5 (new investigator): which requests carry pid 0 and whether a
  WARNING is right for them; reproduce with --runs_per_test=40; fix the credentials code (likely)
  or the test's assertion, failing first.
- russ (2026-10-09, morning): quiet kernel questioned (slower? worse coverage?): measured faster
  (fast tier 1701 -> 1282 s uncached); coverage concern accepted: 26.14e, a weekly noisy CI job
  (default sysctls, two vCPUs, runs_per_test) whose failures become deterministic tests,
  approved ("Yes to noisy job"). 26.16 (per-job Bazel profiles, one cold run) added as proposed,
  since no cold-cache profile exists. Formal-methods expansion wanted: russ asks whether TLA+
  coverage is thorough and the C++ is shown to follow it; answered with the known gaps (per-file
  trace, cross-directory interleavings, directory streams, reachability direction) and proposed a
  model-mutation step, 12.11b, 12.10 and modelling born-dirty first; a read-only audit to produce
  the gap list. Born-dirty create explained (row and mark in one phase-3 transaction; cleared at
  the next sync point like any dirty row; open-for-write rows held back). Decision on 23.10 still
  open.
- russ (2026-10-09): approved the formal expansion: 12.12 read-only audit (dispatched now, reads
  main at c1b1142, no lane, no Bazel), 12.13 model mutation, 12.11b per-file and 12.11c
  cross-directory trace validation, 12.10 test generation from TLC's state graph as a Bazel target
  that generates the tests every build (never checked in). 23.10 closed as investigated; 23.11
  born-dirty create approved: model first, build if sound ("mirror the way a real filesystem
  works: files are created dirty until fsync or dirty_writeback"). 26.16 (bazel --profile per CI
  job) goes to the first lane that frees, ahead of everything: russ waits for it to push.
- russ (2026-10-09): "I approved the kernel bug report commands". 26.15 resumes in lane-5 (its
  branch step-26.15 and .scratch/kernel-src are there) once 26.14d, which is running in the same
  lane, reports; 26.16 takes the next lane that frees after that.
- russ (2026-10-09): "Yes to all" on four more formal steps: 12.14 the model's SQLite durability
  abstraction tested by power cuts, 12.15 runtime invariant checks derived from (or paired with)
  the model, 12.16 a refinement target (ideal POSIX spec, checked by TLC), 12.17 the mount.dcfs
  handoff modelled. Ordered after 12.12's audit, which may reorder the whole formal queue.
- 12.12 audit done (Opus, read-only, no Bazel): 24 ranked gaps in notes/formal-coverage-audit-
  2026-10-09.md. Worth russ's eye: G5, a latent crash-safety gap (a parent fill between a create's
  syscall and its phase 3 records the child clean; unreachable today under the kernel's directory
  lock and one thread, reachable in the harness and with parallel dirops; it bears directly on
  born-dirty); G1 writable opens have no model and emit no trace events; G4 the concurrency guards
  are only checked in the large tier; G11/G12 traces check few properties and cover four scripts,
  cut at the first link or cross-directory rename. The audit's order replaces the earlier plan
  order (phases/12). G16 sent to the Phase 13 agent: ident.tla in the same change.
- russ (2026-10-09): "Start an additional temporary lane for the bazel --profile change." lane-7
  created (a clone like the others, user.bazelrc copied from lane-1, .scratch/ excluded); 26.16
  dispatched there (new investigator): --profile per CI job uploaded as artifacts, tools/ci_profile.py
  with a fixture test summarising fetch/@dcfs_llvm/third-party/our compile/tests into the job
  summary, a workflow_dispatch `cold` input skipping the cache restores, the question whether the
  extracted @dcfs_llvm is cached at all (answer only, no fix), one local fast-tier table. lane-7 is
  removed after the merge (its Bazel server shut down first).
- russ (2026-10-09): "If another lane frees up first, cancel lane-7 and do the --profile change
  there. Don't interrupt any current work, but the --profile change is P0 max priority." Standing
  order: the first lane to report hands its checkout to 26.16 (fetch step-26.16 from lane-7, shut
  lane-7's server down, re-point the agent); no other dispatch takes a freed lane before 26.16 has
  one with an extracted toolchain.
- russ (2026-10-09): fsuuid_compat.h unnecessary with hermetic headers. Confirmed against the
  sysroot's linux/fs.h (6.12 defines fsuuid2 and FS_IOC_GETFSUUID); testutil.c's FS_IOC_SHUTDOWN
  shim is the same case. 7.1d queued (mechanical, after 26.16's P0), with a style rule: no copies
  of UAPI definitions.
- russ (2026-10-09) on the syscalls.h / syscalls_backing.h split: "Keep it. Mechanical invariant
  enforcement is more valuable." The reason goes into style.md with 7.1d.
- 26.14d merged (dae95e1): the flake was the fixture forging pid 0 on every request; the daemon is
  unchanged. Lane-5 freed. P0 rule considered: lane-7's @dcfs_llvm extraction completed at 10:31
  (marker present, 2.1 GB), so 26.16 is productive there and moving it to lane-5 would cost more
  than it saves; 26.16 stays in lane-7. Lane-5 resumes 26.15 (russ approved its commands).
- 23.8b merged (162fd19, six commits, clean rebase): FSnapView was the inexact view (a never-taken
  snapshot's default 0 read as "current" at clock 0, which the view also zeroed: not a congruence);
  fixed to normalise only where read; a no-VIEW run of all 1,457,405 states passes every property,
  so 23.8's results stand; bisection targets and a `workers` attribute on tlc_test are committed.
  large_test/nolock_test did not finish under load 30-35 (view field constant there; CI confirms).
  Audit G3 closed. Lane-1: 23.11 born-dirty model dispatched to the same protocol agent.
- 15.6 reported (lane-6, two commits): the systemd guest works and found a real deployment bug,
  the restart race (systemctl restart of a dcfs mount starts the new daemon while the old one
  still holds the cache database; a non-nofail line then lands in emergency mode): 15.6b, with
  "start waits bounded for an exiting holder" recommended over a umount helper. Also: mount(8)
  resolves UUID= before the helper, so decision 11 (source = spec as written) cannot hold; README
  corrected. Sent back for the rebase over 26.14/26.14c/7.1c/15.7 (init, run-qemu.sh,
  qemu_test.bzl, MODULE.bazel, README conflicts expected), the README restart sentence from 15.7
  corrected, and a table of 15.7's nine unverified systemd statements (asserted / contradicted /
  unverified).
- 26.16 merged (86a3775, three commits; lane-7 shut down and removed). Finding: CI caches neither
  the repo contents cache nor the output base's external/, so every job re-extracts @dcfs_llvm
  (about 14 times per push); 26.16b queued to cache it, decided on the push's numbers. Coverage
  baseline bumped to 95.97 / 79.68 as run 37853130640's gate suggested. PUSH POINT at this
  commit: since 9a67552 it carries 23.8/23.9, 15.1/15.2/15.7, 12.8, 8.2e/f, 7.1c, 26.14/c/d,
  23.8b, 26.16 and the plan.
- russ (2026-10-09) on 15.6b: no timers; a umount.fuse.dcfs that waits (no timeout) for the daemon
  to exit, plus a style rule against timers as a way to wait ("from an idle 256 core supercomputer
  to a 1 core raspberry pi under 40 loadavg"). Phase file updated.
- russ (2026-10-09): "Agreed with all" on the no-timers discussion: banned-symbols entries for the
  sleep/timer family and sqlite3_busy_timeout, a repo-shape check against bare `sleep` in guest
  scripts, a SQLite busy-handler check, all in 15.6b; 11.3c (event-driven fault window) queued;
  the backing stall stays a concurrency-design fix, never a watchdog.
- Found while recording the above: dcfs/sqlite.cc:511 sets `PRAGMA busy_timeout=5000` on every
  connection (a 5 s timer in disguise; sqlite.h:338 documents it), and 29 guest scripts still
  contain `sleep`. Both are 15.6b's to handle: the busy handler goes (one writer per database by
  design; a lock conflict is either the startup "in use" check or a bug, so an immediate clear
  error, with the reasoning in design.md), and the sleep sites are listed against 6.5.
- Phase 13 built (lane-2): connected fds by name under the parent, handle fallback, ident.tla
  extended with a known_bug, goldens/budgets updated. Blocker: a named open with write access takes
  freeze protection, so fault_freeze_test fails six checks (dcfs opens O_RDWR for read-only user
  opens). NEEDS RUSS: options (a) accept, (b) named opens for read-only only, (c) read-only opens
  get a read-only connected fd and writable opens take the writable fd (blocking as the backing
  would). Agent investigating (c)'s cost; no code change until russ decides.
- Phase 13: (c) investigated and rejected (one passthrough backing_id per inode makes a writer
  after a reader lose passthrough; writer-first stays disconnected). 13.4 experiment dispatched to
  the same agent: (e) O_PATH-by-name to reconnect the dentry, then the handle open; (f)
  AT_HANDLE_CONNECTABLE handles (6.13+, handle-only). Decision waits for its table.
- 13.4 experiment done: (f) AT_HANDLE_CONNECTABLE chosen (deterministic, no freeze blocking on
  ext4/xfs/btrfs, one extra syscall, handle-only); (e) rejected as alias-order dependent. Agent
  implementing it on step-13.1 with the experiment kept as a kernel-behaviour guard test;
  fault_freeze_test stays unchanged. Russ informed; may veto.
- 26.15 reported (lane-5, two commits; the fresh agent was not blocked): standalone reproducers
  under tools/kernel_bugs/ with manual guest targets proving each (ext4 oops, btrfs WARNING 6 of 6
  listed directories), the btrfs cause PROVEN: a directory whose inode item was updated only in
  memory (relatime from a listing) takes btrfs_fill_inode's fast path with index_cnt = -1; when the
  on-disk read fails, iget_failed's make_bad_inode sets S_IFREG and btrfs_destroy_inode reads
  index_cnt as csum_bytes (shared storage since d9891ae28b0d, v6.11): a spurious diagnostic, not a
  data bug; an unanswered syzbot report exists (2024-09, no reproducer), so the draft is a reply.
  ext4 still present in mainline af32da41b032; no earlier report found (lore returned 403 to the
  tool; a mirror and web searches used: russ to search lore by hand). Drafts in
  notes/kernel-bugs-2026-10-09.md. Review passed; one round for the stale pin_inodes comment and
  the rebase, then merge.
- 26.15 merged (badacf3, three commits). Needs russ: send the two mails after the pre-send checks
  in the note. Lane-5 free: 12.13 (model mutation tool) dispatched (new implementer); the first
  sweep waits for 23.11's model to land so it runs against one model, not two.
- russ (2026-10-09): style rule, no test-only things in production code; a knob a test needs is a
  real feature; zero "only use in tests" comments. 25.7 queued with a repo-shape check.
- CI run 37973594236 (bc7eee7): fast, presubmit, reproducible, mutation-changed, osv, subjects,
  full(2), ubsan(1,2), asan(2) green; coverage FAILED on 26.14c's new artifact gate: six branch
  counts of 2^32 (or 2*2^32) plus a few hundred in main.cc's daemonisation and startup_channel.cc,
  a different shape from 26.14c's -1 (a 32-bit wrap plus a real count, in the fork path under
  continuous profiling). 26.14f queued for an investigator. Five shards still running.
- russ (2026-10-09): production-default fakes (the no-op ProtocolEvents) are the pattern 25.7 wants,
  not a violation. And 25.8: all of https://abseil.io/tips/ adopted as design guidance, softer than
  the style rules; agents cite a tip number in advisory findings; agent definitions updated by the
  step.
- CI run 37973594236 (bc7eee7) finished: every job green except coverage (26.14c's artifact gate
  on the fork/daemonise path: 26.14f). The earlier "full (0)" entry was a false alarm (the job's
  conclusion is success; its only annotation is GitHub's Node 20 deprecation warning for the
  pinned upload-artifact action: bump that pin with the next CI housekeeping). First runner
  profiles are in each job's summary and artifacts for 26.16b's decision.
- 15.6 rebased (40bed61, three commits; only qemu_test.bzl conflicted, 26.14's cpus kept beside
  systemd_image/boots). Nothing in the Debian image resets the quiet-kernel sysctls (checked under
  systemd). Of 15.7's nine systemd statements: six asserted, `x-systemd.requires-mounts-for` and
  the fstab-generator "finished at helper exit" partly or not asserted, `systemctl restart`
  contradicted and the README fixed. Two more findings for 15.6b: `umount` then `mount` at once
  fails the same way, and after a reboot about half the daemons get systemd's SIGTERM while still
  finishing after their unmount (nothing dirty, nothing lost; "shutdown: clean" missing). One more
  round: the README's interim recipe used `flock -w 10` (a timeout, against today's rule) and the
  leftover-daemon checks' wait needs to be event-based; then merge.
- 23.11 model half reported (lane-1, step-23.11): born-dirty sound under all regimes with two code
  rules (phase-3-inserted rows only count as born dirty; a fill-inserted row under a dirty parent
  is born dirty). A LATENT BUG in today's code found on the way: daemon crash between a create's
  syscall and phase 3, restart, lookup records the child clean, power loss -> a clean row of a
  file that no longer exists, served by handle; neither swept nor probed at start. Opus review
  dispatched (is the bug real in C++, are the two rules minimal, fidelity, known_bugs, trace note).
- 15.6 merged (4fd3136, four commits; the last makes every wait in the test and the README recipe
  event-based: pidfd select, journalctl --sync, a fifo; `flock` with no bound). Lane-6 continues
  with 15.6b (same agent: the umount.fuse.dcfs helper waiting on the daemon's pidfd, the
  banned-symbols and repo-shape enforcement of the no-timers rule, busy_timeout=5000 removed, the
  two remaining polls, style.md's rule).
- 23.11 review (Opus): sound; the latent bug CONFIRMED in the C++ and a second path found that
  needs no crash (a create whose phase 3 fails after its syscall, answered EEXIST). Exposure by
  nodeid only (NFS/saved handles), never swept. Code half dispatched to lane-1 after four model
  items (model the failed phase 3, drop or fix the vacuous metaprefix config, README table,
  large/nolock green), with the reviewer's four failing-first tests, two-sided OriginOK plus a
  negative log, InsertDirty counting (closes G15), and 26.4b to measure rule 2's cost.
- 12.13 tool merged (9f5d17f, three commits; tools fast 29 pass, mutation presubmit 6 pass incl. an
  end-to-end run of real TLC on a tiny fixture with one kill, one survivor, one invalid). First
  run on dcfs.tla: 11 killed / 9 survived of 20, small tier; survivors listed in phases/12 for the
  sweep after 23.11. Lane-5 free: 26.14f (coverage artifacts in the fork path; the coverage job is
  red on every push until it is understood) dispatched (new investigator).
- Phase 13 built on connectable handles (lane-2, step-13.1 rebuilt as four commits): per-open
  connectable handle through the one handle-open abstraction, fallbacks to the plain handle,
  fault_freeze_test unchanged and green on ext4/xfs/btrfs, a kernel-behaviour guard test, goldens
  +1 call per named open. Correction to the orchestrator's earlier claim: the kernel floors differ
  (6.9 for FS_IOC_GETFSUUID, 6.13 for AT_HANDLE_CONNECTABLE; EINVAL fallback between). Opus review
  dispatched (identity safety of the derived handle, phase-3 context, error handling, tests,
  ident.tla, docs).
- Phase 13 review (Opus): another round. H1 identity weakened for objects without a generation or
  birth time (TLC: MC_ident_power with OutOfBand violates HeldResolvesToItsObject in 4 states on
  the branch, none on main); the guard test uses a timer and a dentry-cache-dependent control; the
  documented diagnostics benefit holds only for fstype=none (cloned mounts show clone-relative
  paths; AppArmor treats them as disconnected); cost understated below the root. NEEDS RUSS:
  orchestrator recommends shelving (keep the branch, merge the TLC config that found H1).
- 26.14f merged (6d74ef5): ForkDaemon returned in both processes, shared continuous-mode counters
  made llvm-cov's derived branch counts negative; ForkSplit primitive, per-test artifact check in
  cov-lcov.sh, fork fixture in coverage_pipeline_test. Lane-5 free: 25.7 + 25.8 + 7.1d dispatched
  as one implementer step (style rules, the no-test-only-knobs inventory and repo-shape check, the
  Abseil tips precedence, the UAPI shims removed).
- russ (2026-10-09) asked why 23.11's latent bug escaped testing and agreed to the answer: path-
  shaped crash oracles, no handles held across cuts, single-fault sequences, no row lifecycle in the
  model. Queued: 11.7 (identity oracle after every crash + a row-durability checker rule, first
  free lane, dcfs-protocol), mixed-fault three-event sequences folded into 26.14e, counterexample
  replay into 12.10, and a "row lifecycles under crash prefixes" rule for formal/README via 12.12a.
- russ (2026-10-09): Phase 13 SHELVED ("Agreed, we're shelving it"); his design note for a much-later
  13.5 (a dcache patch making `__d_obtain_alias` prefer a connected alias, then O_PATH-then-handle
  opens with identity untouched) recorded verbatim in phases/13. The H1 TLC configuration
  (MC_ident_power + out-of-band) goes into main on its own as a 12.12a item (lane-2's agent, from
  its branch); lane-2 then takes 11.7.
- 25.7 + 25.8 + 7.1d reported (lane-5, three commits): style.md sections (precedence and the Tips
  of the Week; no test-only things, with the production-default fake as the pattern; no UAPI
  copies; why three syscall libraries), repo_shape `no_test_only_comments` (seven sites reworded,
  `ErrnoNameTable()` removed as the one real conversion), agent definitions name the tips;
  fsuuid_compat.h and an FS_CASEFOLD_FL shim gone; FS_IOC_SHUTDOWN stays (the sysroot lacks it:
  plan corrected). Fast tier: one failure that is main's, from 26.14f (the fault test's forked
  wrapper binary missing from the syscalls_backing golden list): sent back as a `26.14f:` fix in
  the same branch, then rebase. Needs russ (small): `DirCacheFS`'s `friend
  testonly::DirCacheFSPeer` is a test-only seam in production code that the checker's tests use;
  style.md records the option of exposing that bookkeeping through ProtocolEvents instead.
- 12.12a's first item merged (71c8059): `MC_ident_power_oob` (generation-only evidence, out-of-band
  changes), medium, 721,557 distinct states, passes on main; no known_bug variant possible on
  main's ident.tla (no inode-only switch). Lane-2 free.
- 25.7 + 25.8 + 7.1d merged (704f11e, four commits incl. the 26.14f golden-list fix that had left
  the fast tier red on main; fast 233 + 2 on the rebased tip). russ on the DirCacheFSPeer friend:
  "This is exactly the thing I want to get rid of. A test peer allows test code to reach into the
  private internals of a class. Don't do that." 25.7b queued: remove the peer, expose the
  bookkeeping as a real feature, a repo-shape check against testonly friends.
- 17.1 reported (lane-4, three commits, 27 files): xfstests' generic group runs against dcfs on
  ext4/xfs/btrfs through the real mount(8) (an Alpine tools repo, ~125 helper programs built
  static, a musl-strerror LD_PRELOAD shim, a guest patch to `check`, a mount wrapper giving each
  FUSE mount its cache_db, an umount wrapper that waits for the daemon), 150 pass / 14 listed /
  572 not run per backing (reflink 173, dmsetup 76, godown 32, quota 31), 48 excluded with reasons,
  a 14-fixture gate self-check failing first, six eternal shards per backing (11-21 min each at
  load 9-19), 1024 MiB guests. Findings: (a) dcfs keeps S_ISGID after an unprivileged write where
  the backing clears it (generic/683-685; minimal repro in the report) -> 17.2; (a) 29 metadata-
  heavy tests 20-50x slower through dcfs, daemon CPU-bound, one in a SQLite fsync until /cache
  moved to tmpfs -> 17.3 remeasure on a quiet host (born-dirty removes that fsync); (b) remount,ro
  ignored, shared-mmap mtime late, chattr attributes not in statx, a second mount of one source
  refused (README bullet added); the umount race seen again (15.6b). Not settled: CI placement
  (18 eternal shards would land in the full/asan/ubsan matrix) and sanitizer memory. Opus review
  dispatched; the agent asked for a dedicated CI job and sanitizer handling.
- 26.16b dispatched to lane-2 (new implementer): cache the extracted toolchain in CI, sized from
  the bc7eee7 run's profiles.
- 26.16b merged (4e21a50): the repo contents cache cannot hold @dcfs_llvm (only http_archive/
  http_file repos live there), so the output base's extracted tree plus marker is cached by pin
  hash (0.58 GB zstd). The profiles put the extraction at about an hour of runner time per push
  (199 s fast, 456 s presubmit, 263 s coverage, 276 s per reproducible build, about 253 s hidden
  in each shard's unprofiled cquery). Verify on the second push after this lands. Lane-2 free:
  12.14 (the model's SQLite durability abstraction tested by power cuts with dm-log-writes
  replay) dispatched (new dcfs-protocol).
- 17.1 review (Opus): one more round. Required: the gate accepts a pass turning into "not run"
  (a per-backing notrun list; listed timeouts fail), two new sleep-and-poll waits (flock on the
  database; read -t on a fifo), the sanitizer configs break the guest (incompatible under
  asan/ubsan for now; 17.1c later), vacuous gates, mkfs failures discarded, doc/code mismatches,
  exclusion reasons (musl getopt, raw-device tests, per-backing). The setgid finding diagnosed:
  fuse_setattr's legacy rule plus dcfs's root-credential fallocate/write/copy paths; fix = run them
  inside AsCaller (17.2), plus a one-line kernel fix worth sending upstream. 17.1b (Debian rootfs
  via rules_distroless, after 17.1) and 17.1c recorded; 17.3 gets daemon CPU per test. Sent to the
  agent together with the CI-placement round.
- 25.7b merged (a10f58c): the DirCacheFS test peer is gone; the checker reads a read-only
  `events::Bookkeeping` view that fuse_ops passes to the hooks; tests tamper a copy; repo_shape
  refuses testonly friends. Lane-1 (23.11) warned: invariant_checker.{h,cc} and the checker tests
  changed under it. Lane-5 free: 26.14e (noisy weekly job with mixed-fault sequences) dispatched
  (new implementer).
- 15.6b built (lane-6, 7e3e42a): the helper must be `umount.fuse` (libmount drops the subtype), so
  it runs for every FUSE mount and the administrator links it; it waits on a per-mount flock in
  /run/dcfs the daemon holds to exit (no pid file, crash-safe through the kernel); `umount -l`
  does not wait; busy_timeout gone (only FinishRun's reader case relied on it); no-timers
  enforcement (banned symbols, repo_shape sleep allowlist of 77 sites with reasons, style.md
  1.11); the three disabled systemd checks are real and green. NEEDS RUSS: owning `umount.fuse`
  system-wide. Opus review dispatched (lock races, device numbers across namespaces, non-dcfs FUSE
  mounts and user mounts, SQLITE_BUSY paths, the allows, shutdown-model fidelity).
- 6.5 reported (lane-3, eight commits): real wins (nfs_test 221 s to 48 s via nfsd grace/lease,
  mmapwrite usr1, release_leak on the warning, the ASan bench OOM explained: five ASan daemons at
  410-625 MiB, asan_mem 4352), tier walls not comparable (load 10-35), per-guest fixed cost 4.5-6 s.
  Two changes only sped up polls (start_daemon 1 s to 0.1 s; wait_for_line 1 s to 0.13 s), against
  the no-timers rule that landed meanwhile: sent back to become events (poll() on /proc/self/mounts
  via testutil; READY lines through a fifo), then rebase, again after 15.6b for its sleep allowlist.
- 15.6b review (Opus): not yet mergeable. libmount tries umount.fuse.dcfs FIRST (the agent's debug
  reading missed it), and systemd unmounts with -c, so the dcfs-only name suffices: decided, ship
  umount.fuse.dcfs by default, umount.fuse as an opt-in; russ's question is moot. Blocker: the
  helper hangs when the unmount does not end the superblock (binds, other namespaces, umount -r);
  fix via fusectl's connection directory nlink. High: device-number reuse can make an unrelated
  daemon refuse to start; blocking lock with re-check or the unique mount id. Plus -N, process
  setup before dispatch, a test that can hide a failed restart, the 90 s stop timeout documented,
  coverage gaps, banned-symbols reasons. Sent back.
- russ (2026-10-09): Fable subagents may be used a little more where judgement is the bottleneck,
  and effort levels should vary per task. CLAUDE.md's orchestration section updated. Planned uses:
  the final review of 23.11's code half (crash safety across create, fill and recovery), the
  refinement target's design (12.16); mechanical batches at low effort.
- 26.14e built (lane-5, three commits): the DCFS_NOISY knob with self-checks both ways, the weekly
  six-runner noisy job, the mixed-fault generator (one op + three events incl. crash3, fail3,
  cutahead) with path and identity oracles and known-bad fixtures; 20 sequences per backing in the
  large tier, 120 weekly. No true positive on main; the agent doubts its timing reaches 23.11's
  state. Opus review dispatched: can the generator reach that state (what event is missing), is the
  identity oracle sound (GETATTR vs open, recycled inodes, false positives), event fidelity, the
  job's red-for-other-reasons risks, fhtest hygiene.
- 23.11 code half built (lane-1, 773ca36): rules 1 and 2, the ghost-row fix with A1/A2/A3 and the
  fault_power `born` scenario failing first, the checker rule, two-sided OriginOK with `sync_p`,
  create fsyncs 1n+1 -> 0n+1, budgets with before/after; presubmit found and fixed two things
  (trace shards vs a mkdir during a sync point; the btrfs born ordering). FABLE REVIEW dispatched
  (the first use of Fable for a subagent, per russ's 2026-10-09 allowance: crash safety across
  create, fill, recovery and sync is the hardest judgement of the day); the agent rebases onto
  main (25.7b's checker shape) meanwhile. large_test/nolock_test still unrun under load: CI.
- 12.14 built (lane-2, 91172d3): the model's durability abstraction HOLDS on ext4/xfs/btrfs under
  dm-log-writes replay (447/338/2026 crash states, FLUSH prefixes, FUA prefixes, ordinary-write
  subsets, torn writes; the bound's margin reaches 0 on btrfs; real losses exercised; oracle
  failing first with four checks). Own parsers for the log and WAL formats; loop devices over
  tmpfs instead of a fifth virtio disk (IRQ 8 collides with the RTC). Opus review of the oracle's
  soundness dispatched (fingerprint matching vs invisible commits, reference states across the WAL
  restart, SQLite's own recovery vs the prefix property, reach of the reordered states, parser
  checks against the real tools).
- 26.14e review (Opus): the generator could not reach 23.11's state at all; the oracle's own
  content check after a restart marks the ghost-to-be atime-dirty and so hides it, and fail3's
  global sync closes the other path. The clean runs were a false negative. Sent back with the
  fixes (metadata-only mid-sequence checks, a dropahead event, automatic handles before every cut,
  the two 23.11 sequences pinned and shown failing on pre-fix code, a real-ghost fixture, a
  handle-based identity comparison, fhtest.c restored, noisy_report's artifact name).
- russ (2026-10-09): 26.17 approved and queued (RAM-backed guest disks for the fault and ACE tests,
  budgets in three deterministic parts, wall time only as the hang guard, a load-starvation flag,
  17.3 attributing by CPU and I/O counts); after the 15.6b/17.1/6.5 rounds merge; Gantt at
  dispatch.
- russ (2026-10-09) shared the plan usage: session 26%, weekly all-models 65% (resets Friday 1:00
  AM, a week out), weekly Fable 19%. Tokens bind again: process.md's budget paragraph amended
  (about three lanes after the running rounds land, Opus reviews only for protocol/identity/oracle
  steps, rules front-loaded into prompts, low effort for mechanical work, the protocol queue
  sequenced, stop dispatching near 90%).
- 23.11 rebased onto main (eba08e4; one conflict in invariant_checker.h, 25.7b's shape kept; fast
  247 + 2, trace shards, fault_power x3, asan x3, formal small+medium green). Waits on the Fable
  review.
- russ (2026-10-09): style rule 25.9, no intentional crashes (no LOG(FATAL)/CHECK/abort in
  production; RET_CHECK fine; a static Create for fallible construction, per TotW #42; a crash
  that is really needed goes to russ case by case). Queued with a repo_shape/banned_symbols gate
  and an audit: a first grep found 25 CHECK-family sites, one abort (today's fork_split.h), one
  assert and ten exit sites in production code, none in LOG(FATAL).
- 6.5 merged (6bf6bdb, twelve commits): the two polls are events (`testutil waitmount` on a
  pollable /proc/self/mounts, `waitline` on inotify, both unbounded, with a pidfd so a daemon that
  exits first fails fast); fast 233 + 2, presubmit qemu 126, nfs/ace/pjdfstest green. 15.6b told:
  its sleep allowlist is trimmed against this tree on its rebase.
- 23.11 code-half review (Fable): no crash-safety or protocol defect; merge after three small
  fixes (an untested ParentOf branch needs a harness test under the recorder; a stale Create
  comment; a stale formal/README sentence) plus cheap advisories (design.md's reason for fill-born
  rows not being durable, a handle-length check, the born scenario pinned against vacuity, comment
  trims, a sentence on every phase 1 of a born-dirty row taking the fast path). The reviewer's
  statement for russ is in the phase file. Sent back; merge next.
- russ (2026-10-09): exception to 25.9 and its inverse: test code may crash when something not
  under test fails (setup, infrastructure), and ASSERT/EXPECT are only for the property under
  test, so "the test failed" and "the infrastructure failed to run the test" stay distinct. Setup
  uses plain CHECK/CHECK_OK (russ: no custom helper, googletest already tells a crash from an
  assertion failure); for a StatusOr a `CHECK_OK_AND_ASSIGN` macro in ASSIGN_OR_RETURN's style
  (russ approved adding it); the audit converts setup ASSERTs, the guest scripts get the same
  split, and repo_shape refuses ASSERT_OK_AND_ASSIGN in fixtures.
- 12.14 review (Opus): oracle sound, parsers verified against the kernel source and real SQLite;
  not merged yet: an uninitialised awk counter left the first write of every epoch (the WAL
  writeback on ext4/xfs) untorn, so the reordering half was never exercised; the forged-WAL
  self-check can pass vacuously; the docs overstate; the WAL-copy facility moves to 12.10. Sent
  back with a reach check and a host self-check required.
- 26.14e second round: the generator reaches the state; 10 of 22 pinned sequences fail on main
  with the ghost, identically on ext4/xfs/btrfs, eight of them needing only crash3/fail3, a
  listing and the final cut (the bug's reach is wider than the model's two paths); listed as
  expected failures with reason 23.11, strictly. Review passed on the evidence; merging after a
  rebase over 6.5, ahead of 23.11, which then empties the list (lane-1 told).
- Pushed: 2ee9cc7, CI run 38009667624 being watched.
- russ (2026-10-09): 26.18 approved, "but after our weekly quota resets" (Friday 2026-10-16): the
  end-to-end layer's assertions and oracles move to C++ (a guest-side gtest library; shell stays
  for command glue; port the two oracles and the ACE mixed test as the pattern; measure before
  porting the rest); and googletest's structured output (`--gtest_output=xml|json`) copied out of
  the guest and merged into test.xml so breakdowns aggregate per check across both kinds.
- 26.14e merged (628af12, four commits; clean rebase over 6.5; fast 236 + 2; the three mixed
  targets green with their ten expected failures). The noisy job runs weekly from here; its first
  real run sizes the 300-minute limit. Lane-5 idle by the budget throttle.
- russ (2026-10-09): support fstab's sixth field (fsck via fstab for a dcfs line). Recorded as the
  refinement of 15.5: an `fsck.dcfs` helper that finds its fstab line through util-linux,
  delegates the backing's fsck with the flags passed through, checks the cache database
  (integrity, schema, lock, dirty-set sanity; rebuild with -y, report with -n), fsck(8) exit
  statuses, no waits on a held database; README examples get passno 2; the systemd guest asserts
  the boot-time check. After 15.6b merges.
- 23.11 review fixes done (lane-1, 4281fb0 on 2ee9cc7): the ParentOf harness test under the
  recorder (failing first with the mark removed; shard c validates the real `parent` begin line),
  the Create comment, the formal README sentence, the advisories, `born` in the kill-mode list
  (green on three backings). Final round sent: rebase over 26.14e, empty its ten expected
  failures, run the mixed targets.
- russ (2026-10-09): allow_other always on, refused in fstab. Recorded as 15.8 (with the reasoning:
  root daemon plus default_permissions makes FUSE's mounter-only default pointless here); README,
  man page, fixtures and guest wrappers drop the option; after 15.6b.
- CI run 38009667624 (2ee9cc7) FAILED in fast before any test: Bazel could not fetch the Debian
  cloud image ("Connect timed out"); cloud.debian.org times out over IPv6 from here too and
  answers over IPv4; Bazel's downloader prefers IPv6. Everything behind fast skipped, so 26.14f
  and 26.16b are unverified in CI. 15.6c dispatched (mechanical, lane-5): preferIPv4Stack in
  .bazelrc's startup options and a mirror list for the image. Push again when it merges.
- 23.11 MERGED (421b2aa, nine commits): born-dirty create and the ghost-row fix; the ten
  mixed-fault sequences that failed on main hold on all three backings, the expected-failures
  file is empty; fast 250 + 2. Lane-1 free.
- 15.6c merged (ddadd31): cloud.debian.org first (immediate 302), the UMU mirror, cdimage last;
  `startup --host_jvm_args=-Djava.net.preferIPv4Stack=true`; `bazel fetch --force` of the image
  succeeded through Bazel (cloud.debian.org still timed out in that run and the mirror served it,
  so the mirrors are the fix that matters). PUSH POINT: main after this entry; since 2ee9cc7 it
  carries 26.14e, 23.11 and 15.6c. Lane-5 free. Three lanes from here (budget).
- 15.6b round 2 re-review (Opus): MERGE. Both names installed (umount.fuse.dcfs default, umount.fuse
  opt-in, measured with LIBMOUNT_DEBUG in the guest), fusectl's nlink decides the wait, blocking
  LOCK_EX with the unlink-while-locked re-check (ABA-free: the open fd pins the inode), no
  daemon-to-daemon deadlock (one path-based cycle noted for the README: a mount covering another
  instance's cache dir or /run/dcfs; Ctrl-C or systemd's timeout breaks it), -l/-N skip the wait,
  the systemd restart check cannot hide a failed child. Items 16-18 and 21 were never sent to the
  agent (orchestrator's omission): follow-ups. Final round: rebase over 23.11/26.14e/15.6c,
  regenerate the sleep allowlist, make the two waiting tests prove the wait, rename the restart
  check, the README deadlock note; then merge.
- 12.14 MERGED (73ed7e5, two commits; clean rebase): the reordering half is now exercised (48
  states per run on ext4/xfs kept a later WAL frame past a lost one; SQLite recovered the last
  synced commit each time), 0 violations on all three cache filesystems, the regime and gaps stated
  honestly. Lane-2 free. The push point moves to main after this entry (fd9f437 plus 12.14).
- 12.12a (oracle hygiene) dispatched to lane-1's protocol agent (the third lane under the budget:
  15.6b's final round and 17.1's fix round are the other two): premises for the properties with no
  failing variant, five more invariants in Trace.cfg with timing, MC_nolock_small plus the
  effect-point properties in MC_nolock, negative trace logs for the guard-decision checks, a TLC
  coverage report with a zero-count gate, a two-name CrashRefines variant, the row-lifecycle rule
  in formal/README.
- 15.6b MERGED (461fc6a, six commits; clean rebase): the restart race is closed for systemd and
  `umount -c`, the no-timers rule is enforced by banned symbols, a sleep allowlist and style.md
  1.11, busy_timeout is gone. Follow-ups listed in the phase file, including 23.11's new
  `sleep 1` after a thaw in the born scenario. Push point: main after this entry (537a7ef plus
  15.6b). Lane-6 continues with 15.5 (fsck.dcfs) + 15.8 (allow_other always on), same agent,
  same wrapper code.
- russ (2026-10-10) asked whether FUSE blocks the unmount on a daemon callback: only fuseblk and
  virtiofs get a synchronous FUSE_DESTROY; plain fuse mounts learn after the fact, hence the
  15.6b helper. Recorded 15.6d (an INIT opt-in for synchronous DESTROY; the helper becomes the
  fallback) and started `notes/kernel-patches.md`, the list of kernel patches we want: the FUSE
  generation series, the ext4 casefold tune oops, the btrfs destroy_inode warning, fuse_setattr's
  setgid rule, the dcache connected-alias preference, and this one.
- russ (2026-10-10, 01:30): weekly all-models at 75% (65% two hours earlier), Fable 27%, session
  65%. FREEZE: no new dispatches until the reset (Friday 2026-10-16 01:00); the three steps in
  flight (15.5/15.8, 17.1's round, 12.12a) finish; then merges, CI watching and plan upkeep only;
  no review agents unless a landing step touches crash safety or identity; small agents only for
  something urgent (a red push, a bug russ hits). Queue resumes Friday: 26.17, 26.18, 11.7, 25.9's
  audit, 15.3, 12.11b/c, 12.15-12.17, 17.2/17.3/17.1b.
- russ (2026-10-10): the fstab for production (two lines per data disk: the raw btrfs mount and a
  `none` dcfs mount over it, requires-mounts-for, nofail, passno 0 until 15.5, allow_other until
  15.8) and the none-vs-bind question: `none` for directories; `bind` adds only a detached
  lifetime, submount isolation and backing-side ro. A README section "none or bind" folded into
  lane-6's 15.8 round, and the agent asked whether anything (15.4's recorded bind mount points)
  needs `bind` at all; dropping it is a candidate simplification for russ.
- russ (2026-10-10): "Delete it." 15.9 recorded and folded into lane-6's round: `dcfs.fstype=bind`
  refused with a pointer to the native-bind recipe (no alias; russ floated one, declined for the
  same strictness as allow_other, two lines if wanted); the capture helper keeps only the
  native-type branch; tests converted or deleted with the reasons; README's section becomes
  "`none`, and the native bind recipe"; 15.4 loses the bind bookkeeping.
- russ (2026-10-10): "Kernel bind mounts aren't detached clones, and we've never shipped dcfs."
  The refusal rationale was wrong; `bind` becomes an alias of `none`, documented as a synonym,
  with one behaves-as-none test. Lane-6 told; the rest of 15.9 stands.
- russ (2026-10-10): "maybe we rename none to bind and drop the name 'none' entirely." Done that
  way: the directory form is `dcfs.fstype=bind`, `none` is an unknown value; lane-6 told to rename
  across code, tests, docs and guest wrappers and report the grep.
- CI run 38013240343 (eecb91f): fast, subjects, osv, reproducible, mutation-changed green (the
  Debian image fetch fix worked); presubmit and coverage FAILED on the same two new medium tests:
  `sqlite_durability_short_test` (`both-durability-levels`: 2 synced / 5 normal, an expectation
  written before 23.11 removed the create's fsync; 12.14b in lane-2) and `mutate_tla_e2e_test`
  (`Exec format error` executing the fake bazel it writes to a temp file: no interpreter line;
  12.13b in lane-5). The full/asan/ubsan shards were skipped, so the toolchain cache and the
  coverage gate remain unverified. Both fixes are small agents under the freeze's "red push"
  exception.
- 12.13b merged (15c9c13): the fake bazel's `#!` line was the hermetic interpreter's sandbox path,
  over the kernel's 127-byte limit for interpreter lines, hence ENOEXEC on the runner and not in
  the lane; now a two-line /bin/sh wrapper under TEST_TMPDIR, and `cleanup` tolerates a failing
  `clean --expunge`. 12.14b (the durability count) is the remaining red before the next push.
- 12.14b merged (e133eca): the short test's count became two checks, "both durability levels
  present" and "each operation's durability matches dcfs's rules" (synced: a phase 1 naming an
  inode not durably dirty; normal: all inodes already durably dirty), per-operation expectations
  in the script; a subtlety learned: a mkdir'd directory is born dirty but not durably, so its
  rmdir syncs. Short 137 states, large 419/396/1114, 0 violations. PUSH POINT: main after this
  entry (eecb91f plus 12.13b, 12.14b and plan); both CI reds fixed.
- 15.5 + 15.8 + 15.9 merged (31e795c, five commits): fsck.dcfs (findmnt --fstab, the backing's
  fsck delegated, cache integrity/schema/lock/dirty checks, rebuild with -y, fsck(8) statuses
  or'ed; the systemd guest sees systemd-fsck@ finish before the mount), allow_other always on
  (refused in a line; a remount may carry the mount's own, libmount adds it), the directory form
  renamed `dcfs.fstype=bind` with `none` refused and the clone capture removed (-44 lines in
  backing_capture.cc plus tests). Two corrections from the systemd guest to the orchestrator's
  recipe: an overmount plus a native bind of the original needs `mount --make-private` on the raw
  path (shared propagation shows fuse.dcfs on it) and cannot be expressed in fstab (a cyclic
  transaction); the working fstab shape is the disk at the raw path and dcfs on the consumers'
  path with requires-mounts-for. `enospc_cache_test` is timing-sensitive (`sleep 3` vs a 2 s
  interval): a 6.5/no-timers follow-up.
- 12.12a merged (20da809): 20 of 22 unbitten properties have premise tests (BackingAtSyscall and
  KernelForgotAfterCrash hold by construction), Trace.cfg checks four more invariants (TLC time
  per shard roughly tripled, within the limit; CleanMeansNoDirty unusable on harness traces, which
  never run StartRun: G19), MC_nolock_small (343k states, 80 s; R4 bites there), negative logs
  for the guard-decision checks, TLC coverage reports gated per action, the two-name CrashRefines
  variant violates on crash_f1, the row-lifecycle rule in formal/README.
- CI run 38018425705 (f616181): fast, presubmit, COVERAGE (the gate passed: 26.14f verified),
  reproducible, mutation-changed and 6 of 9 shards green; fast took 7 min against 11 before (the
  toolchain cache hit). Three shard failures: `fault_ace_fs_test_xfs` ace-dsplit (plain and asan:
  the backing after the cut differs from the persistence-point snapshot on xfs only),
  `destroy_test` under asan (`destroy.sh: line 98: arithmetic syntax error`), `nfs_test` under
  asan (`nfs-handle-read-after-restart` read 10 of 43 bytes through an NFS handle after a
  restart: a correctness-bug candidate). 26.20 CI triage dispatched (investigator, lane-1) under
  the freeze's red-push exception: reproduce, bisect among the merges since 9a67552 if needed,
  fix tests in tests and dcfs bugs with a failing-first harness test.
- 17.1 review round done (lane-4, 733c486 on 66bff51): per-backing notrun lists (about 572 entries
  each) and the gate failing on unlisted or changed notruns and listed timeouts, which at once
  caught generic/770 failing where it should not run (generic/740 mkfs'd the raw scratch device
  earlier in the same batch; raw-device tests now end their batch); the umount wrapper waits on
  the daemon's flock, the watchdog is a blocking `read -t` on a pipe; two justified timers remain
  (a 0.2 s mount retry while the kernel drops the old namespace; `timeout 1 logread -f`); all 63
  xfstests targets incompatible under asan/ubsan (the shim's `__asan_report_load1` relocation
  failure measured); the gate test runs under the pinned busybox and gawk (25 fixtures); mkfs
  status checked; six exclusions left, the 40 slow tests in an `xfstests_slow` set (eight manual
  shards, 1500 s limit, lists empty until 17.3); a dedicated `xfstests` CI job (one runner per
  backing, 25-45 min of tests estimated, 70-90 with a cold build), test.sh's default selection
  excludes the xfstests and manual tags with a unit test; daemon CPU ticks per test in the results.
  18 of 18 shards green at the tip. Sent for a rebase over 15.6b/15.8/15.9 (its mount wrapper adds
  the now-refused allow_other; the sleep allowlist; the bind rename), then merge without further
  review.
- 17.1 merged (lane-4, rebased tip 51859d9 → main 5ea0719, eight commits, subject gate exit 0):
  the rebase dropped the image's umount wrapper (15.6b's helper) and the mount retry, and the
  mount wrapper no longer passes allow_other (15.8 refuses it). Phase 17 file and README row 17
  updated; 17.2, 17.3, 17.1b, 17.1c stay queued behind the freeze. Lane-4 is free.
- Style rule (russ, 2026-10-10): no retry loops; one that seems necessary goes to russ first.
  `docs/style.md` 1.12 written; the inventory of existing loops (three `kAttempts` loops in
  dir_cache_fs.cc for the coroutine future, `ListXattrOPath`'s ERANGE loop, the helper's EINTR
  restart, which is not one) is `docs/plan/notes/retry-loops-2026-10-10.md` for russ's ruling;
  enforcement via repo_shape identifiers proposed as 25.10 after the ruling. Weekly all-models
  usage 84% (Fable 32%): the freeze holds, only the 26.20 triage agent keeps running.
- 26.20 merged (lane-1, rebased onto 17456d9 → 9fd3ff2, three commits, subject gate exit 0,
  diff read by the orchestrator): all three shard failures of run 38018425705 were test bugs
  (uptime_ms octal, exportfs -f racing the asan daemon's readiness, xfs log commits between a
  persistence point and the cut since 23.11). No dcfs bug; the suspected stale-size read was
  ESTALE from nfsd. Fixes wait on events (a blocking stat) or remove the timing dependence
  (drop-writes after the persistence point); no retry loops or timers. Phase 26 file has the
  details. Push point announced to russ: main at the plan commit after 9fd3ff2. Lane-1 free;
  its Bazel server shut down.
- russ asked whether backing.cc's xattr write path duplicates the /proc reopen: it does (the
  path form works for every type); queued as 25.11 after the reset. Two more retry loops added to
  the inventory (GetGroups' EINVAL loop, GetXattrOPath's ERANGE loop). russ pushed 8f528d8; CI
  run 38025358346 watched in the background.
- russ ruled on the retry inventory's own-thread loops (unlink/rename/readdir): no bounded
  retries, no EAGAIN fallback; a mutex or an unbounded optimistic retry whose progress the TLA+
  model shows. Style 1.12 amended with his words; the cross-process xattr sizing loops stay open
  for his ruling. The duplicate-mechanism question (backing xattr reopen) gets a standing review
  question in the agent prompts; the reopen allowlist idea was withdrawn (a reopen is not wrong on
  its face, so a reason column would have read true).
- russ on the xattr sizing loops: do what the backing filesystem does (ERANGE, the program
  retries). Inventory updated; all production retry loops now have rulings: unlink/rename/readdir
  become unbounded optimistic retries with a model liveness property, the xattr and getgroups
  loops become one call pair. One step (25.10) after the reset: the code, the model's branch, the
  repo_shape check on attempt counters. The one-way review question is in all agent definitions
  (6a41abf).
- russ sharpened the retry rule: each FUSE operation keeps the backing syscall's contract
  (ERANGE on races where the backing says so; atomicity where the syscall is atomic, with
  unbounded internal retry if needed). Style 1.12 has his words; 25.10 now audits every handler
  against it.
- CI run 38025358346 (8f528d8): presubmit red on `fault_power_kill_test_ext4` (power cut 'atime':
  served and backing atimes differ by about 35 ms across the tree) and coverage red on
  `stress_short_test_btrfs` (`no-reclaim`: 872 pages scanned). 26.21 triage dispatched
  (investigator, lane-1) under the red-push exception; the other jobs were skipped.
- russ asked for an external source of the POSIX contract: `notes/posix-contract-sources-2026-10-10.md`
  (SibylFS as the reference for 12.16, a 12.18 spike on its trace checker, DFSCQ/Ferrite for the
  crash vocabulary, AtomFS's linearizability for atomicity). Nothing fetched or dispatched.
- russ approved the POSIX-contract proposal: 12.16 refined (SibylFS semantics restricted to
  our operations, cited to Lem definitions; EAGAIN never an ideal outcome; crash contract ours in
  DFSCQ/Ferrite vocabulary; atomicity as linearizability; Fable review of the mapping design),
  12.18 added (spike: SibylFS's trace checker through Bazel as a second oracle). Both after the
  reset, 12.16 after 25.10's model changes.
- 26.21 merged (lane-1, rebased → ca65a1c, one commit, gate exit 0, diff read by the
  orchestrator): both failures of run 38025358346 were harness (snapshot reads moving atimes
  behind a regressed clock; coverage initramfs growth near the watermark). Push point announced:
  main after this plan commit. Lane-1 free, its Bazel server left running for the reboot.
