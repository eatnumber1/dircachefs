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
