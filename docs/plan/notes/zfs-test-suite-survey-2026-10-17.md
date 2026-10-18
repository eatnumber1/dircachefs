# OpenZFS test suite: is anything worth bringing into dcfs?

Source: /home/russ/Sources/zfs/tests (read only, nothing run). dcfs context: pjdfstest, xfstests (generic, fsstress, fsx), sibylfs, QEMU guests, guest tests mostly .sh plus C++ via //tools:testutil.

## Verdict
Adopt as a pinned third-party suite: NO. Run the suite itself: not worth it (ksh93, ZFS pools in nearly every script). Port a handful of ideas and 2-3 small C programs' behaviour into our own guest tests: yes, a few cheap ones. Most of the generic POSIX coverage in ZTS is already in xfstests/pjdfstest, which dcfs runs.

## Licence
- dcfs LICENSE: MIT. ZFS tree: CDDL-1.0 (file-level copyleft, weak) per COPYRIGHT/LICENSE and SPDX headers; a few files differ (cp_files/seekflood.c is MIT).
- Fetching + running unmodified, pinned, built by Bazel as a test-only dep: allowed (same pattern as GPL xfstests in third_party/xfstests/README.md: "fetched and built by Bazel, code never copied"). Patching it: CDDL applies to the modified files, source of the changes must stay available; keep patches in third_party/<name>/ like 0001-dcfs-guest.patch (fine in a repo, labelled CDDL).
- Copying CDDL code into dcfs source: possible only per file, keeping the CDDL header on that file; mixes licences in an MIT repo, avoid. 
- Reimplementing an idea (the sequence of syscalls, the race shape) from scratch: no copyright issue (ideas/facts not protected); write our own code, cite the origin in a comment (style 1.9 allows a pointer). MIT seekflood.c could be copied outright with its header.
- Needs a lawyer? No; but do not paste ZTS code.

## 1. Test groups (tests/zfs-tests/tests/functional/*)
ZFS dependence: libtest.shlib has ~139 zfs/zpool refs; every script runs under setup/cleanup that creates a pool ($TESTPOOL, $TESTDIR). Generic scripts mostly only need a directory plus helper binaries, but they source libtest.shlib, which assumes pool, DISKS, users (staff/zfs user via user_run).

| group | what it checks | ZFS-dependent? | size | dcfs value over pjd/xfstests |
|---|---|---|---|---|
| ctime (ctime_001 + cmd/ctime.c, 389 lines) | st_mtime/st_ctime changes after creat, write, utime, chmod, chown, link, setxattr (do_xattr, 291) | loop over `zfs set xattr=sa/on`; the C program is pure POSIX (takes a file) | seconds | xattr -> ctime is covered by xfstests? setxattr ctime bump is the one dcfs-relevant row (cache must mark ctime unknown). Cheap port. |
| truncate_timestamps / fallocate_extend_timestamps | truncate and ftruncate (-f) update mtime and ctime; fallocate extend does, --keep-size does not | no ZFS beyond pool dir; shell uses `stat -c`, `fallocate`, sleep 1 | 5 s | xfstests has generic/313 and t_truncate_cmtime (already in our helper list); keep-size fallocate no-bump is a nice dcfs cache probe. Port idea (~20 lines). |
| rename_dirs + cmd/rename_dir.c (76 lines) | parent and child process rename a nested dir into/out of itself repeatedly (race, 10 s) | pure C, shell only forks, sleeps, kills | 10 s | concurrency on rename of ancestors vs descendants; xfstests fsstress has renames but not this adversarial two-process cycle. Good dcfs target (cache of parent/".." and path-to-inode maps under cross-dir directory renames). Port. |
| link_count + rm_lnkcnt_zero_file.c (149 lines) | 3 threads: rename a file onto 1000 names, remove names, and open O_APPEND|O_CREAT / close in a loop, watch for st_nlink==0 on an existing name (hung process = failure) | C pure POSIX; shell polls with sleep 10 | up to 100 s | nlink accuracy under racing rename/unlink/create is exactly dcfs's tri-state attribute cache; xfstests t_* don't do this. Port (high value). |
| append/threadsappend (126 lines) | N threads O_APPEND write to one file, final size must be exactly 1310720 | pure C | 1-5 s | append atomicity through FUSE write-through with size caching; simple, port. |
| cp_files + seekflood.c (187 lines, MIT) | N processes run copy_file_range / SEEK_DATA/HOLE on hole-y files concurrently, checks for corruption (zfs #15526 dnode dirty race) | pure C, MIT licence | 3x12 s | SEEK_DATA/SEEK_HOLE and copy_file_range through dcfs (README says copy_file_range reaches backing) under concurrency; xfstests generic has seek tests but not stress. Port or even use as reference. |
| cp_files_001/002 | cp of sparse/reflink files | `cp` + zfs props | s | covered by xfstests generic copy_file_range/cp tests; skip. |
| xattr_001..014, xattr_compat | create/read/write/append/delete xattr, no-permission neg, cp -a/tar/mv preserve xattrs, xattr size accounting (xattrtest 100 files x 128 xattrs x 16k) , noxattr mount | 001,002,014 ZFS props (xattr=dir/sa), 004 zvol+ext4, 005/006/007 snapshots/clones, 013 `zfs mount -o noxattr`, 012 pool accounting. 003, 011 (cp -a, tar, mv, find) are portable shell. | 1-2 min | xfstests generic/xattr tests already cover; useful only for xattrtest (scale and 64k limit); see section 2. |
| mmap group (mmap_mixed uses fio 60 s; mmap_read/write/seek/sync/ftruncate/read_truncate) | mixed buffered+mmap IO (fio), mmap write then ftruncate to 0 racing writeback, mmap read racing truncate, msync latency bound, SEEK_DATA on mmaped | mix; most are `log_must mmapwrite`/helpers on pool dir; mmap_sync and read_truncate assert ZFS internals (zpl_writepages, dmu_read) | 10-60 s each | dcfs is FUSE write-through: mmap of a FUSE file uses the page cache, writeback goes through dcfs write path; mmap_ftruncate and mmap_read_truncate are the sort of dirty-page-vs-truncate race dcfs's size attr tracking could get wrong. fsx covers mmap but not this race. Port mmap_ftruncate shape. |
| tmpfile (tmpfile_001/002/003, stat_mode, test.c) | O_TMPFILE: open, write, linkat via /proc/self/fd, O_EXCL cannot be linked, mode/umask in stat, linkat result visible | env TESTDIR; pure C | s | dcfs docs claim O_TMPFILE and linking work; xfstests generic/004? only light. Port (cheap): O_EXCL refuse, mode honouring umask, linkat visible after cache. |
| renameat2 (exchange, noreplace, whiteout + cmd/renameat2.c 118 lines) | flags behaviour | C helper trivial | s | dcfs rename.sh already tests noreplace/exchange via testutil; whiteout (RENAME_WHITEOUT) is the only gap, and FUSE doesn't support it (EINVAL) - check what dcfs returns. Skip beyond whiteout line. |
| unlink/unlinked_fh_accessible + file_unlink_fh.c | reopen via /proc/self/fd of an unlinked-but-open file | pure C | s | dcfs removed.sh likely covers open-unlinked; worth a 10 line check of /proc/self/fd reopen. Low. |
| suid/* (suid_write_to_*) | write by non-root to suid/sgid files clears the bits; zil replay | file_write helper; zil replay ZFS-only | s | kernel clears setuid via setattr(killpriv) -> dcfs must cache mode change; xfstests generic/193/ 'suid' cover. Port the 4-row matrix (cheap). |
| sparse, large_files, largest_file | sparse files, 2^63 write offset boundary | largest_file.c is pure C | s | boundary.sh exists; skip. |
| write_dirs | fill pool with dirs/files, ENOSPC | ZFS pool size dependent | long | covered by enospc_*.sh; skip. |
| mv_files/random_creation, longname | many files mv, 255+ names (longname ZFS-specific >255) | mix | | names.sh covers; skip. |
| stat/statx_dioalign, getversion, dos_attributes, crtime, projectquota, idmap_mount, user_namespace, zoned_uid | ZFS-specific props, statx btime, DIO alignment, idmapped mounts, user ns | mostly ZFS | | idmap_mount and user_namespace use the unshare / mount_setattr helpers (idmap_util.c, user_ns_exec.c) and could be generic: do they work over FUSE? FUSE does not support idmapped mounts (kernel gate) - only worth it if dcfs plans it. Skip. |
| lease, syncfs, fadvise, exec, chattr | setlease/ F_SETLEASE, syncfs, posix_fadvise, exec of file in fs, chattr +i/+a | chattr: needs fs flags (ZFS), lease: FUSE lacks leases | | chattr: dcfs forwards flag ioctls; xfstests generic has them. Skip. |
| nopwrite, casenorm, dedup, ... | ZFS only | yes | | skip. |

## 2. Generic C helpers (tests/zfs-tests/cmd)
Self-contained (libc only unless noted); all CDDL except seekflood (MIT):
- rename_dir.c (76): two-process rename cycles; no deps.
- rm_lnkcnt_zero_file.c (149): 3-thread rename/remove/open-append; pthread.
- threadsappend.c (126): O_APPEND multi-thread; pthread.
- ctime.c (389): timestamp rules table (above).
- xattrtest.c (705): phases create/setxattr/getxattr/unlink, N files x M xattrs x S bytes, random sizes 16..S, --verify; libc; only setxattr/getxattr via `sys/xattr.h`; scale+random-size checker of xattr persistence, up to Linux's 64k limit; a good model for an xattr scale test.
- tmpfile_*.c, mmap_ftruncate.c (75), mmap_read_truncate.c (163), mmapwrite.c (152; pthread mmap write while truncation), readmmap.c, mmap_seek.c (161), mmap_sync.c (307, uses ZFS timing bound), clone_mmap_write.c, clonefile.c (434, FICLONE / copy_file_range), file_write.c, file_append.c, file_trunc.c, file_unlink_fh.c, truncate_test.c, mkfile.c, mkfiles.c, mktree.c, dir_rd_update.c (126: read a dir fd opened O_SYNC while another process creates/removes entries), statx.c (307), renameat2.c, getversion.c (FS_IOC_GETVERSION), manipulate_user_buffer.c (325, O_DIRECT; ZFS Direct IO specific), seekflood.c (187, MIT).
All use libc and fork/pthread only; none link libzfs. Excludes: anything with `libzfs`, `nvpair`, `zcp`, send/recv, `draid`, `ereports`, `checksum`, `btree`, `crypto`.

## 3. Running a subset against dcfs in a guest
- Runner: tests/test-runner/bin/test-runner.py.in (python3, configparser; reads runfiles/*.run with pre/post (setup/cleanup), user, timeout), invoked by scripts/zfs-tests.sh (bash, ~700 lines, sets DISKS, loopbacks, users, ksh93 symlink, STF_PATH of ~100 required commands from include/commands.cfg).
- Tests: /bin/ksh -p scripts; libtest.shlib (139 zfs/zpool mentions), requires `log_must`, `$TESTPOOL`, `$TESTDIR`, users, `fio` for mmap_mixed, `fallocate`, `sudo`, `stat -c`, ksh93 (Alpine has `ksh`? only mksh/loksh/ksh93 via edge; not in our busybox initramfs; plus python3 for the runner).
- Pools: setup.ksh/default_setup requires creating a pool; our own mount (dcfs over ext4/xfs) would have to impersonate $TESTDIR. You would write a custom runfile with `pre =`/`post =` empty (common.run does this at line 101) and a stub libtest.shlib: libtest.shlib is sourced by every script and calls zfs for cleanup/verify_runnable/get_prop; tests that don't call zfs would work with a trimmed libtest. That is a patch of ~1500 lines to maintain (sub-ZTS fork). 
- Effort to run a subset (ctime, truncate_timestamps, fallocate_extend, rename_dirs, link_count, threadsappend, xattr_003/011/012, tmpfile, mmap_ftruncate, mmap_read_truncate, suid): build helpers C (Bazel rules, ~15 files, cc_binary; fine, no libzfs), ship ksh93 + python3 + coreutils (Alpine `ksh`?? not in guest image today), stub libtest.shlib/libraries, runfile: estimate 3-5 days plus ongoing patch maintenance, to get about 25 tests of which ~12 are non-redundant with xfstests. Compare: porting 6 ideas in C++ is ~1 day total. Also shell tests (sleep 10 / pgrep) violate our no-timer/load-agnostic rules in spirit and take wall time.
- Verdict: not worth pinning the suite. If russ wants it anyway: fetch release tarball with sha256, build only cmd/ helpers via a BUILD overlay (like helpers.bzl for xfstests), and drive them from our own C++/sh.

## 4. Ideas to reimplement (priority order)
1. link_count race (rm_lnkcnt_zero_file.c, tests/functional/link_count/link_count_001.ksh): rename/remove/open-append threads; assert st_nlink>=1 on any name that exists; no sleeps: bounded iterations, stop on fixed count. Value: high (nlink and rename in cache). Effort: ~0.5 day C++.
2. rename_dirs race (rename_dir.c, rename_dirs_001_pos.ksh): two processes rename nested dirs into each other; after N iterations compare cache vs backing tree (stress.sh oracle exists). Value high, effort 0.3 day. Bounded loop (count, not 10 s).
3. threadsappend (threadsappend.c, threadsappend_001_pos.ksh): size must equal threads*writes; also check content records are not torn. Value medium-high, effort 0.2 day.
4. ctime/mtime rule table (ctime.c; truncate_timestamps.ksh; fallocate_extend_timestamps.ksh): rows: creat, write, utimensat, chmod, chown, link, setxattr, truncate, ftruncate, fallocate (+ KEEP_SIZE no bump) vs dcfs after a cache read; compare dcfs stat vs backing stat after each. Add a "second-granularity" free comparison (compare dcfs vs backing directly instead of sleep 1). Value high (cache staleness of times), effort 0.5 day. Check how atime.sh/setattr.sh cover already.
5. mmap vs truncate races (mmap_ftruncate.c: dirty all pages then ftruncate(0); mmap_read_truncate.c: read fault after truncate, expect SIGBUS or zero but correct size). Value medium (dcfs size cache after writeback), effort 0.5 day. fsx -W/-R covers mmap, not this race.
6. xattr scale (xattrtest.c; xattr_012_pos.ksh): files x xattrs x sizes up to 64k, verify values after drop caches and remount; plus the 003 negative (mode 000 file, non-root cannot read/write xattrs; root can with CAP_DAC). Value medium (dcfs caches xattrs, side-effect xattrs; check cache size limits), effort 0.5 day.
7. O_TMPFILE trio (tmpfile_001..003, tmpfile_stat_mode.c): O_EXCL tmpfile cannot be linkat'd, mode honours umask, linkat visible by readdir afterwards. Value medium (dcfs README claims it works; one test gate), effort 0.3 day.
8. seekflood (cp_files/seekflood.c MIT; copy_file_range/SEEK_DATA/HOLE concurrent on sparse): value medium; effort 0.4 day; licence-free to copy with header.
9. dir_rd_update.c: read a directory fd while entries change (EISDIR read must fail with EISDIR), cheap check, low.
10. suid matrix (suid_write_to_*): non-root write clears suid/sgid; read mode back through dcfs. Value medium-low (xfstests generic/193 etc.), effort 0.2 day.

## Ranking
- ADOPT as pinned third-party: none.
- PORT idea (do): 1, 2, 3, 4 (high value, under 2 days together), then 5, 6, 7.
- PORT optionally: 8, 10; 9 low.
- SKIP: the rest of ZTS (ZFS commands, snapshots, zvols, pools, send/recv, casenorm, nopwrite, dedup, quota, idmap/userns (FUSE no support), chattr/lease/syncfs/fadvise).
Caveat: I did not verify that dcfs lacks these tests: the existing coverage (xfstests incl. fsstress, readdir-while-renames, t_truncate_cmtime, renameat2; pjdfstest; stress.sh; rename.sh) overlaps ideas 2 and 4 partly; check xfstests output logs before writing 2/4. Also claim marked "??" about Alpine ksh and about xfstests generic numbers is from memory, unverified.
