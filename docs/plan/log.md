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
