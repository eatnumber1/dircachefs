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
| 4 | [Pinned host tools; `third_party/` convention](phases/04-pinned-host-tools.md) | done 2026-10-06 |
| 5 | [CI runs the QEMU suite](phases/05-ci.md) | planned |
| 6 | [Test tiers and test speed](phases/06-test-tiers-and-test-speed.md) | in progress (6.3 first fix merged; 6.1 with 5; 6.2 throughout) |
| 7 | [Pinned toolchain, coverage, warnings, UBSan, clang-tidy](phases/07-toolchain-coverage-warnings-ubsan.md) | planned |
| 8 | [Coverage close to 100%](phases/08-coverage-to-100.md) | planned |
| 9 | [File names are bytes](phases/09-file-names-are-bytes.md) | planned |
| 10 | [Benchmarks](phases/10-benchmarks.md) | planned |
| 11 | [Crash, stress and failure testing](phases/11-crash-stress-failure-testing.md) | planned |
| — | **Trial point:** [21.1 trial on a spare disk](phases/21-real-hardware-checks.md) (russ, manual) | after 11 |
| 12 | [Formal model (TLA+) and trace validation](phases/12-formal-model-and-trace-validation.md) | planned |
| 13 | [Connected backing fds](phases/13-connected-backing-fds.md) | planned |
| 14 | [Identity from the backing filesystem](phases/14-identity-from-backing-fs.md) | planned |
| 15 | [The `mount.dcfs` wrapper](phases/15-mount-dcfs-wrapper.md) | planned |
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
