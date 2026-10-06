# Phase 11 — Crash, stress and failure testing

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
