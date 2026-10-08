# Phase 11 — Crash, stress and failure testing

**Status as built (2026-10-08).** The steps below are the original plan;
what landed differs:
- 11.1 (merged ba7687d) is the dm-flakey/dm-error harness
  (`test/qemu/guest/fault_lib.sh`: healthy, error-writes, drop-writes,
  error-reads, error-io, dead) with `fault_backing`, `fault_cache` and
  `fault_power` tests, not dm-log-writes: drop-writes plus the real
  power cuts of 11.2 cover the same states without a replay tool. It also
  covers most of 11.3 (error-reads/error-writes under the backing).
- 11.1b (in review): a cache-disk I/O error reaches the caller as EIO,
  not EAGAIN (russ, 2026-10-08).
- 11.2 is restated as: the fault tests over ext4, xfs and btrfs
  backings; real power cuts (`run-qemu.sh --power-cut` kills QEMU at the
  guest's cut marker and boots a second time over the same images; five
  cut points, three filesystems); ACE-style one- and two-operation
  sequences with a cut, with and without an fsync through dcfs, remount
  and restart (served tree equals the backing's; after an fsync the
  backing is the fsynced state). Merged 2026-10-08 (f3a2d2b) after two
  review rounds: boot 1 of a cut has a verdict (its own dmesg scan before
  the marker; QEMU must die of our kill, status 137), seven fake-QEMU
  self-checks, a synced/unsynced witness as the kill-mode "before"
  scenario, snapshots with nlink + md5 shared in lib.sh, `direct`/`dsplit`
  persistence points via `testutil syncfs`, a `fixtures` kind as the ACE
  checker's negative fixture, ACE split a/b/fs on the checking build.
  Open: a cold CI run to measure the asan shards (estimated 116-159 min;
  limit raised to 240) before choosing a fourth shard or a time-weighted
  partition. Neither drop-writes
  nor a kill loses writes the disk acknowledged without a flush, so
  FLUSH/FUA ordering stays untested (that is what dm-log-writes would add).
- 11.2b fsstress and fsx (merged 2026-10-08, 0beb46e): xfstests tag
  v2026.05.17 as an http_archive, only ltp/fsstress.c and ltp/fsx.c built
  (static, testonly; AIO, io_uring, libbtrfsutil and the xfsprogs headers
  disabled or shimmed), run in the guest on ext4/xfs/btrfs in short
  (medium), long (large) and random (enormous, `manual`) modes. After
  each tool the tree is compared against the backing three times: through
  the kernel's cache, after drop_caches with the same daemon (dcfs's
  cache) and after a restart (type, mode, uid/gid, size, nlink, mtime,
  ctime, rdev, symlink target, md5; left out: directory sizes, st_blocks,
  absent records only via listings). fsstress successes are counted per
  op with floors, any EIO fails, fsx's disabled-feature set is pinned
  (clone/dedupe range, atomic writes, dontcache, four fallocate modes).
  `stress_checks_test` is the comparison's self-check under busybox.
  Found on the way: `.github/ci/test.sh --list` included `manual` tests.

**11.1 Real power-loss testing with dm-log-writes.** The kernel's
`dm-log-writes` target (kernel config `DM_LOG_WRITES`) records every write,
flush and FUA to a log device. Put the backing filesystem and the cache
database's filesystem each on their own log-writes device, run a workload
through dcfs, then replay each device's log up to a chosen flush/FUA mark
onto a scratch device: that is exactly what survives a power loss at that
point, inside one boot (today's power test only fabricates such a state).
- For each mark (and for pairs of marks, since the two devices stop
  independently: every backing mark against the cache marks around it),
  mount the replayed backing and cache and start dcfs: it must start (dirty
  set recovery), and every entry it then serves must match the replayed
  backing filesystem (no "cache ahead").
- Replay tool: our own small one from the documented log format
  (Documentation/admin-guide/device-mapper/log-writes.rst), modelled on
  xfstests' `replay-log`; xfstests is GPL-2.0, so do not copy its code into
  the repo.
- Workloads: the existing mutation mixes plus fsstress (11.2).

**11.2 fsstress and fsx.** Built from xfstests' sources as an external
test-only dependency (like pjdfstest), run in the guest against dcfs on
each backing filesystem: fsstress for random concurrent namespace and data
operations, fsx for data/size/mmap correctness. After each run, verify the
cache against the backing filesystem (every present/absent dentry and
cached attribute matches). Fixed seeds in the test; a longer random-seed
mode for manual runs.

**11.3 I/O error injection.** device-mapper `error` and `flakey` targets (dm-dust dropped, russ
2026-10-07; a bad block is one block mapped through dm-error) under the backing filesystem make reads or writes fail on demand.
A failed backing operation must leave the affected entries unknown (never
cached as having succeeded), return the error to the caller, and leave the
cache consistent once the device recovers (checker).

**11.4 Out of space.** Fill the backing filesystem: mutations fail with
ENOSPC and nothing is cached as done. Separately fill the filesystem
holding the cache database: SQLite's "disk full" must make dcfs fail the
request (or stop serving) without ever serving wrong data; after space is
freed and dcfs restarts, the checker passes.

**11.5 Instant backing crash.** The shutdown ioctl ext4, xfs and btrfs
support (`EXT4_IOC_SHUTDOWN`, `XFS_IOC_GOINGDOWN`, `BTRFS_IOC_SHUTDOWN` or
their common `FS_IOC_SHUTDOWN`) makes the backing filesystem fail every
operation at once, like xfstests' `godown`. dcfs reports errors, then after
remounting the backing filesystem and restarting dcfs, recovery leaves a
cache that passes the checker. A fast complement to 11.1.

**11.6 fsfreeze.** With the backing filesystem frozen, dcfs mutations
block (reads of cached state keep working); after thaw they complete and
the checker passes; dcfs's own sync points and shutdown do not deadlock
while frozen (or are documented to wait).

Owner: Opus for 11.1 and 11.3-11.5 (crash and failure invariants), Sonnet
for 11.2 and 11.6. Every step uses the cache checker built in Phase 8.
