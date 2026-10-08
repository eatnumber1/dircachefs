# dcfs plan

The plan for dircachefs (dcfs) after the original plan was completed on
2026-10-04. Phases are numbered in execution order; each has its own file
under `phases/`. Every phase writes its tests first and shows them failing
before the fix (see `process.md` and `/AGENTS.md`).

- `execution.md`: the schedule: waves of at most two parallel lanes,
  the model for each lane, and the sync points between waves.
- `process.md`: how phases are executed (conventions, subagent waves,
  models, review, budget).
- `history.md`: the original plan (its Phases 0-7, all done) and
  amendments 1-20. Its phase numbers are unrelated to the ones below.
- `log.md`: the execution log, including items waiting on russ.
- `decisions.md`: what was decided while planning these phases, what was
  rejected, and why.
- `notes/`: background verified while planning (kernel facts, the LKML
  discussion and libfuse_passthrough).
- `review-fixes.md`: fix steps R1-R3 from the 2026-10-06 review of waves
  1-2 (in progress).
- `audits/`: the race, crash, tri-state, style and test-coverage audits
  from the original plan.

To resume: read `/AGENTS.md`, this file, `decisions.md`, then the next
phase's file; `log.md` says where things stand.

## Order

| # | Phase | Status |
|---|-------|--------|
| 1 | [AGENTS.md](phases/01-agents-md.md) | done 2026-10-05 (reviewed by russ) |
| 2 | [Fix: world-readable cache database](phases/02-fix-cache-permissions.md) | done 2026-10-05 |
| 3 | [Drop the kernel patch; download and build the test kernel](phases/03-drop-kernel-patch-and-build-test-kernel.md) | done 2026-10-05 (deprecated patched-kernel path removed with Phase 4's wiring) |
| 4 | [Pinned host tools; `third_party/` convention](phases/04-pinned-host-tools.md) | done 2026-10-06 (R3: scratch filesystems from pinned mkfs tools, 4 KiB blocks) |
| 5 | [CI runs the QEMU suite](phases/05-ci.md) | in progress (5.1, 5.2 done 2026-10-07: workflow green under act for fast/presubmit, not yet published; ASan tier red: see log; 5.3 OSV done 2026-10-07; 5.3c narrowed the gate to shipped dependencies (abseil, gloop, libfuse, liburing, numactl, SQLite, matched by git commit): `osv` job is green under act, the 101 test-only Debian findings are informational; 5.4a test cache trusted, no forced rerun, 2026-10-07) |
| 6 | [Test tiers and test speed](phases/06-test-tiers-and-test-speed.md) | in progress (6.1 tiers done 2026-10-06; 6.3 first fix merged, 6.3b tool builds config-independent; 6.2 guest memory right-sized 2026-10-07; 6.4a-d 2026-10-08 56980ad: host-only tests skipped under sanitizer/coverage configs (100 of 226), destroy_test 20k, suites sharded 3 ways in CI) |
| 7 | [Pinned toolchain, coverage, warnings, UBSan, clang-tidy](phases/07-toolchain-coverage-warnings-ubsan.md) | in progress (7.1 pinned clang done 2026-10-07 f218d45: LLVM 22.1.8 via toolchains_llvm, static lld/libc++, hermeticity build; 7.1b sysroot-or-Alpine-clang follow-up; 7.2 coverage done 96c53e1 (baseline dcfs/*.cc 92.4% lines, 73.3% branches; CI coverage job); warnings audit merged 55caee3 (-Werror scoped to our sources, links fail on warnings: 7.3's scoping half); 7.4 UBSan done 1a65836 + 7.4b 56980ad (memory tests sized by reclaim, asan/ubsan as sharded jobs, osv restores the repo cache); 7.1b sysroot done in lane-6 pending merge; 7.3/7.5-7.7 planned); 7.1b merged 2026-10-08 508ad34: no host compiler, libc or headers (LLVM 22.1.8 + Debian 12 sysroot in @dcfs_llvm, 1.9 of 11.6 GB extracted, clang.cfg pins sysroot/lld/compiler-rt/libunwind/libc++ and reaches sandboxed actions, hermeticity gate + self-check, extract_test, glibc in the shipped SBOM; host still provides glibc >= 2.36 to run clang, bash/mktemp/realpath/rm, /bin/sh + dirname for the Alpine wrappers, rules_distroless tar/grep for the test rootfs); 7.3/7.6/7.7 next once the dcfs/ tree is quiet |
| 8 | [Coverage close to 100%](phases/08-coverage-to-100.md) | in progress (8.1 gate merged 6788fc6: baseline 92.40/73.30, ratchets up; cheap gaps in lane-5; 8.2 survivors #1/#3/#4 killed 4af0981; 8.2b merged 55d0128 (two more survivors killed; a 60-mutant sweep: 51 killed, 7 survived -> 8.2c); 8.2c merged 6757e55 (nine survivors killed, one equivalent listed in tools/mutation/README.md; the harness got a fake kernel passthrough via --wrap of fuse_passthrough_open/close); 8.3 cheap gaps merged 56980ad; 8.4 merged 2026-10-08 1b2fb6d: default_permissions required (INIT refused without it, ACCESS fails closed), dead openat2 wrapper and FuseRequest move/default constructors deleted, read fallback covered in the harness; the dcfs-over-dcfs variant is impossible, see the phase file) |
| 9 | [File names are bytes](phases/09-file-names-are-bytes.md) | done 2026-10-06 (mountinfo/fstab/exports escaping in Phase 15; flag paths in main.cc deferred) |
| 10 | [Benchmarks](phases/10-benchmarks.md) | done 2026-10-06 (baseline recorded; readdir was 27x slower than the backing, 8x after the v3 indexes: 916 ms vs 114 ms for 10k entries, 2026-10-07; reduction plan in log) |
| 11 | [Crash, stress and failure testing](phases/11-crash-stress-failure-testing.md) | in progress (11.1 harness + first fault/power-cut tests merged 2026-10-08 ba7687d, no violations; 11.1b merged 461edcf (a cache-disk I/O error is EIO, not EAGAIN); 11.2 merged f3a2d2b (fault tests over ext4/xfs/btrfs, real power cuts with a verdict on both boots and a kill-mode self-check, ACE sequences a/b/fs on the checking build with negative fixtures, `testutil fsync`/`syncfs`; CI: asan limit 240 min); 11.6 fsfreeze running, lane-5; 11.2b merged 0beb46e (fsstress + fsx from a pinned xfstests, short/long/random modes, three comparisons against the backing, per-op success floors, self-check of the comparison; CI's shard lister now excludes `manual`); 11.6 fsfreeze merged 5840bf0 (what blocks and what does not; finding: a mutation held in a backing syscall blocks the whole single-threaded daemon, cached reads included; `testutil sql` read-only queries of the live cache); 11.3 merged 18c077a (fault_recover_test: 60 cells, outcomes marked asserted/truth-only/skip; bounded flakey window with a self-check; unpinned btrfs reproducer with an expected kernel WARNING); 11.3b ESTALE-as-ENOENT fix queued behind 11.5; 11.5 + 11.4 running, lane-2) |
| — | **Trial point:** [21.1 trial on a spare disk](phases/21-real-hardware-checks.md) (russ, manual) | after 11 |
| 12 | [Formal model (TLA+) and trace validation](phases/12-formal-model-and-trace-validation.md) | 12.1-12.2 done 2026-10-07 (two review rounds; russ to read formal/README.md); 12.3 revalidation model done 2026-10-07 (2c3d79c); 12.4 inode lifetime done 2026-10-07 (def2352); 12.4b fixes merged 2026-10-08 (7136ac2: schema v5, stub ids monotonic, recovery probe, sweep at every start); 12.5 identity model merged 2026-10-08 (e3fe0aa; Phase 14 finding recorded); 12.6 + 12.7 merged 2026-10-08 5776861 (RecoveryIdempotent/CrashSafe through recovery, crash during recovery, effect points; 12.6b: recovery keeps rows dirty until a sync point, daemon crash vs power loss in the model, two known_bugs variants; large_test eternal: 9.16M states); 12.7b reply ghost running (lane-1); 12.8-12.10 planned |
| 23 | [Semantics gaps: mmap/FORGET reconcile, removed objects, relatime, O_TMPFILE/copy_file_range/reflinks/ioctls, boundary stubs](phases/23-semantics-gaps.md) | done 2026-10-07 (23.1-23.5 e5c27cc; 23.6 held fd + 23.7 review fixes a64e666, two review rounds; one open investigation, see phase file); 23.8 atime from the held fd + cache-only directory atimes (russ, 2026-10-08) running, lane-4 |
| 24 | [Alpine series pins for the kernel and host tools](phases/24-alpine-series-pins.md) | done 2026-10-07 (d0b2cc2: kernel linux-virt 6.18 + QEMU, e2fsprogs, xfsprogs, btrfs-progs, busybox from Alpine v3.24; ~30 min of cold compiles -> 22 s fetch; two review rounds) |
| 25 | [Style convergence](phases/25-style-convergence.md) | in progress (25.1 done 2026-10-07 2f5515e; 25.1c done 752c573: every syscall through syscalls.h, enforced; 25.2 at a quiet point after 26.2/26.10/22; format items in 7.6/7.7); 25.3 logging levels + Status message rules merged 2026-10-08 (docs 7d8fab7, code 76ab735: 12 sites WARNING->ERROR, log-or-return pairs removed, ProducedByDcfs marks dcfs's own errors so the reply handler logs ERROR only for those, INFO lifecycle lines, --v=1/2/3; post-merge review pending) |
| 26 | [Bumpers: narrow checks around generated code](phases/26-bumpers.md) | in progress (26.1, 26.8, 26.9, 26.13 done 2026-10-07; 26.3 and 26.4 done 2026-10-07; 26.2 done 4511503 (checking build in the small/medium tiers; +5-15% fast, +25-35% presubmit); 26.6 started (lane-3); 26.7 and 26.10 done a6b86df; 26.5 done 6788fc6 + 26.5b 4af0981 + 26.5c 1be6511 (mutation-changed and subjects as their own jobs, cold input, cold timeouts, trace test split); 26.6 done (fault sweep by call site: 49 sites, 111 iterations, +4 s to fast, no violation) and 26.4b done (one observer, request budgets, slope tests) pending rebase/merge; 26.6 approved as a try, must report its runtime; 26.11 after 25.2; 26.12 done 96c53e1); 26.4b + 26.6 merged 2026-10-08 b866c15 (ProtocolEvents observer, request budgets, slope tests, fault sweep; 26.6 cost: +4 s fast tier, 26 s in the guest); 26.5d mutation expansion (operators, arid nodes, sampling, equivalence file; russ 2026-10-08) running, lane-3 |
| 22 | [Request cancellation (Ctrl+C, EINTR)](phases/22-cancellation.md) | done 2026-10-08 (87b35a0: option (b) checkpoints + session loop; `timeout -s INT 1 ls` on 20k entries 0.8 s instead of 14-20 s; kernel-blocked syscalls uninterruptible until coroutines/io_uring, documented) |
| 13 | [Connected backing fds](phases/13-connected-backing-fds.md) | planned |
| 14 | [Identity from the backing filesystem](phases/14-identity-from-backing-fs.md) | planned |
| 15 | [The `mount.dcfs` wrapper](phases/15-mount-dcfs-wrapper.md) | planned (15.0 man page done 2026-10-07) |
| 16 | [Reject case-insensitive and encrypted directories](phases/16-reject-casefold-and-encrypted-dirs.md) | planned |
| 17 | [xfstests subset](phases/17-xfstests-subset.md) | planned |
| 18 | [Disk quota (EDQUOT)](phases/18-disk-quota-edquot.md) | planned (just before 19) |
| 19 | [Soak test (manual)](phases/19-soak-test.md) | planned (last automated phase) |
| 20 | [Statistics](phases/20-statistics.md) | design to be discussed first |
| 21 | [Real-hardware checks](phases/21-real-hardware-checks.md) (russ, manual) | 21.1 after 11; 21.2 last |

Execution started 2026-10-05 (wave 1).

**Cut line.** The trial point after Phase 11 is the first time dcfs is
worth trying on real hardware (a spare disk, nothing precious): by then
it runs on a stock kernel with pinned tools, the cache permissions are
fixed, coverage is near 100% under ASan and UBSan, file names are
handled as bytes, the idle test proves no background disk I/O, and the
crash, power-loss and failure tests pass. Deployment on a server with
precious data waits for every phase, including the soak test, and the
final hardware check (Phase 21.2).

## Open decisions

- **Statistics design** (Phase 20).

## Future work (not scheduled)

- **More backing filesystems.** Phase 14 (Identity from the backing filesystem) supports ext4, xfs and btrfs
  handles. Extending it:
  - The generic encoder `generic_encode_ino32_fh` (fs/libfs.c; types
    `FILEID_INO32_GEN` and `_WITH_PARENT`) is used by ext2, ext4, f2fs, fat,
    jfs, ntfs3, ntfs, squashfs, ufs, affs, befs, jffs2, the SMB client and
    overlayfs; one decoder covers all of them. Each still needs a way to
    open by inode number when uncached (generation 0 accepted, or a
    filesystem-specific lookup like xfs bulkstat) and a device identity
    (FS_IOC_GETFSUUID or an equivalent).
  - Filesystems with their own encoders need one decoder each: gfs2,
    ocfs2, nilfs2, udf, isofs, erofs, ceph, orangefs, tmpfs (mm/shmem.c).
  - NFS does not use inode+generation: the client's handle is the fileid,
    the file type and the server's opaque handle (`nfs_encode_fh`,
    fs/nfs/export.c). Supporting it needs a different identity scheme, e.g.
    a generation derived from the server handle, or dcfs-assigned
    generations kept in the cache (handles then do not survive a wipe).
    FUSE backings are the same case.
  - A FUSE backing that itself uses passthrough needs dcfs's
    `max_stack_depth` raised to stay within the kernel's stacking limit
    (fs/fuse/backing.c).
- **Kernel: passthrough mmap should keep the FUSE file referenced** so
  RELEASE follows `munmap`. Until then dcfs holds an fd per written file
  and reconciles at the last FORGET (Phase 23.6, marked in the code);
  delete that workaround when the kernel change is the minimum.
- **Kernel: a FUSE remap_file_range operation** so FICLONE/FICLONERANGE/
  FIDEDUPERANGE can reach a FUSE server (today the VFS handles them and
  they fail EOPNOTSUPP on every FUSE filesystem; copy_file_range shares
  extents on btrfs/xfs meanwhile).
- **FUSEX, FUSE's next protocol version** (possible direction). The
  proposal on fuse-devel (Miklos Szeredi, 2026-04-29) identifies objects
  by a variable-size id instead of a nodeid, and Luis Henriques's
  LOOKUP_HANDLE work for file-handle-based NFS export is to be rebuilt on
  top of it (LKML thread on russ's kernel patch, 2026-09-29). If it lands,
  dcfs could pass backing file handles through as object ids, replacing
  the inode-number-plus-generation scheme of Phase 14 and its "open by
  inode number" step, and lifting the ext4/xfs/btrfs restriction to any
  filesystem with stable handles. Track it; no work until it is merged.
- Coroutines over io_uring (the coroutine phase in history.md).
