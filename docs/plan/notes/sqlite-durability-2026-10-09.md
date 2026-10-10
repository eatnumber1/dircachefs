# SQLite durability against the model (step 12.14), 2026-10-09

What `sqlite_durability_test` (`test/qemu/guest/sqlite_durability.sh`)
found when it checked `formal/dcfs.tla`'s abstraction of the cache
database (`Commit(new, sync)`, `dbOpts`) against SQLite on a disk that
reorders writes. Branch `step-12.14`, lane-2, based on 484f084. All numbers
are from QEMU/KVM guests on russ's machine with a host load average of 13
to 18 (other lanes running), Linux 6.18.55 (Alpine linux-virt), SQLite
3.53.4 (the BCR module, no compile options: `fsync`, not `fdatasync`;
autocheckpoint at 1000 frames; powersafe overwrite on).

## The abstraction as tested

A commit is normal (`synchronous=NORMAL`) or synced (`Durability::kSync`).
A power loss leaves the database in the state after some commit, never
before the last synced commit that returned. So: the surviving commits are
a prefix of the commit order (no commit survives an earlier lost one), an
acknowledged synced commit survives, a normal commit may be lost.

How each part is checked:

- **The commit order and the states.** The reference is the database and
  its WAL as the page cache had them at the end; the state after commit k
  is the reference database with the WAL cut after its k-th commit, as
  SQLite recovers it. A crash state is judged by fingerprint (the md5 of the
  database after SQLite's recovery, `integrity_check` "ok" and a checkpoint
  into a copy): it must equal some reference state. A state that is no
  reference state is a commit kept after a lost one, or no database at all.
- **Which commits were synced, and when they were acknowledged.** dcfs's
  intent per operation from the cost counter's `SqliteTransaction(durable)`
  count; what SQLite did from strace of the daemon (every `pwrite64` and
  `fsync` of the -wal: each fsync names the commit whose last frame it
  follows). They must agree (each synced transaction is exactly one WAL
  fsync, right after its commit frame); they did in every run. The
  acknowledgement is bounded by the operation's mark in the dm-log-writes
  log; only marks before a FLUSH are used (a mark is logged at once, but
  dm-log-writes holds ordinary writes back until the next FLUSH, so a mark
  orders nothing against them).
- **The bounds.** For a crash state of the epoch from FLUSH f to the next
  FLUSH g: at least the last synced commit of every operation whose mark is
  before f; and the first commit with the state's fingerprint at most the
  last commit of the first operation marked after g (a later state would be
  a replay bug).
- **Reach.** `crash_states wal-info` counts, in each judged state's WAL,
  the current generation's frames past the recovered ones and those after a
  frame that did not land. On ext4 and xfs some state of an epoch that ends
  before the cut mark must have one of the latter
  (`some-state-kept-a-later-frame`); on btrfs the check is skipped
  (below).

## Replay coverage

The cache disk's log is replayed (CrashMonkey's method) to: every entry
before each FLUSH after the start mark (all of them, always) and the whole
log; per epoch between two FLUSHes, every prefix of its FUA writes without
its ordinary ones; all its FUA writes with a subset of its ordinary writes
(every subset if there are at most 64 or they fit the epoch's share of the
budget, else each write lost, each write kept, then seeded random subsets);
and each ordinary write of more than one 4 KiB block torn at five cuts
(first block, first half, all but the last block, last block, all but the
first), with the epoch's other writes and without them. Torn writes count
in the epoch's share (at most half of it), in log order, so the epoch's
first ordinary write (the WAL's writeback on ext4 and xfs) is torn first.
Our `log-apply` of one whole epoch must leave the replay disk byte for byte
as `replay-log` does (`log-apply-agrees-with-replay-log`, md5 of the
device). Not explored: an epoch's ordinary writes with only some of its FUA
writes (the log does not order them against each other), and tears finer
than 4 KiB.

Before the measured part dcfs creates files until SQLite checkpoints and
restarts the WAL (44 creates in every run), so every later frame overwrites
an old one in place: no allocation for the filesystem's journal to order,
so the frame writes' reordering reaches SQLite's recovery. Without the
restart (the first version of the test), ext4's ordered mode made every
crash state in an epoch the same: new WAL blocks are invisible until the
journal commits the file size.

**The first version did not reach the main state** (review of 91172d3):
`epoch_states` used its counters uninitialised, so the first ordinary
write's size went under the subscript "" and that write was never torn; on
ext4 and xfs it is the WAL's writeback in every script epoch, so every
script epoch recovered exactly two values and no state kept a later frame
while dropping an earlier one. The run with the reach check and the bug
still in (ext4):

```text
SQLITE-DURABILITY reach: 0 states with this generation's frames past the recovered ones, 0 with one after a frame that did not land, 0 of them in the script's epochs (ending before the cut mark)
TEST some-state-kept-a-later-frame FAIL (no judged state of an epoch before the cut mark held a WAL frame past one that did not land (0 states held frames past the recovered ones, 0 after a gap, all at the unmount): the reordering of the WAL's writes never reached SQLite)
```

and `sqlite_durability_lib_test` (host, synthetic log) on the same code:
`FAIL: no torn state for 12:0-8: the first ordinary write too must be
torn`.

Final run (seed 1, budget 1500; the short variant 500), host load 8 to 10:

| Target | Log entries | FLUSHes | States | Distinct recovered | Kept a later frame (script epochs) | Wall time | Peak used |
|---|---|---|---|---|---|---|---|
| ext4 | 613 | 35 | 657 | 70 | 50 (48) | 83.3 s | 106 MiB |
| xfs | 328 | 31 | 565 | 70 | 50 (48) | 103.1 s | 123 MiB |
| btrfs | 616 | 27 | 1486 | 27 | 0 (skipped) | 193.0 s | 127 MiB |
| short (ext4) | 513 | 17 | 207 | 20 | 12 (12) | 52.4 s | 104 MiB |

One state that kept a later frame past a lost one (ext4, epoch 0):
`state-432-tear-10: recovered commit 19 (frames=60 commits=19
pagesize=4096 ckptseq=1 salt=e75c0828-545315e0 beyond=10 aftergap=10);
entries: 433 435 436 434:8-96`: every write of the epoch but the first 4
KiB block of the WAL's writeback (entry 434, 12 blocks) landed; the WAL
holds 10 frames of the current generation past the recovered end, all
after the hole, and SQLite recovered commit 19, the last synced one, which
is a reference state at the bound. With the WAL's writeback torn, ext4 and
xfs now recover 70 distinct states (each epoch several: 19, 21, 26, 28,
31, ...: a prefix of the next synced commit's frames), against 26 to 28
before.

The full script makes 204 commits after the restart (494 frames), which
leave 138 distinct databases: several commits change nothing (a release
record equal to the row), so a lost one is not observable. 18 to 58 states
per run lost a commit of an operation done before the crash; xfs and ext4
also recovered states after the last synced commit (194) but short of the
last (200, 203 of 204). The smallest margin over the bound was 0 commits
on btrfs and 3 on ext4 and xfs. The restart recovered: 15 dirty rows, 13
dirty inodes with valid attributes before the start and 0 after (read
from the database; syslog is asynchronous, and the first version's check
of the recovery WARNING lost that race once on btrfs).

**btrfs.** Every state of an epoch recovers the same database: btrfs
writes data copy-on-write, so a frame's write goes to a new extent that
only the log tree's commit, at the epoch's FLUSH, makes part of the file.
Its 1486 states amount to FLUSH-prefix coverage; the reach check is
skipped there.

Timing with budget 4000 under the same load: btrfs took 839.5 s (3277
states, 0.24 s each), too close to `long`'s 900 s, hence 1500. Memory:
`mem = 384` (peak 128 MiB used plus the kernel's 30 MiB is 158; plus
128 MiB is 286, but the log and the replay disk live in tmpfs and grow with
the script); the ASan allowance was not measured (the default, 384).

## Verdict

The abstraction holds on ext4, xfs and btrfs in the regime tested: 0
violations in every run of every target. The oracle's self-checks pass
and fail with an oracle that accepts everything (final script, ext4):

```text
TEST oracle-refuses-a-commit-kept-after-a-lost-one FAIL (commit 201 lost and 202 kept (lost pages: 12 24 25): the oracle said 'ok -1 -1')
TEST oracle-refuses-a-lost-synced-commit FAIL (the state before synced commit 194 (operation 16), bound 194: the oracle said 'ok -1 -1')
TEST oracle-accepts-an-allowed-state FAIL (the state after synced commit 194: the oracle said 'ok -1 -1')
TEST full-log-recovers-the-final-state FAIL (the whole log recovered the state after commit '-1' of 204)
```

The forgery must now open as a database (a forgery that fails
`integrity_check` made the check pass for nothing: the oracle answers
"none" to an empty fingerprint); the first candidate opened in every run.

Observations, none a finding against the model:

1. SQLite fsyncs the WAL header when it starts or restarts a WAL, whatever
   the synchronous level, before the first frame (the "header" fsync in the
   strace). The model has no such durability point; it only makes the disk
   more durable than modelled.
2. Each kSync transaction issued exactly one WAL fsync, after its commit
   frame, and no normal one issued any: dcfs's intent and SQLite's
   behaviour agree, and the model's `Commit(new, sync)` is what happens.
3. A create's writable open is a second synced phase 1 (the new file is not
   durably dirty), so `creates` of n files make n + 1 synced commits when
   the parent is not durably dirty and n when it is; the model's fast path
   (D alone) is coarser, as `formal/README.md` already says.

Gaps left:

- **The growing-WAL regime.** `FinishRun`'s TRUNCATE checkpoint
  (`dcfs/sqlite.cc`, `Connection::Checkpoint`) empties the WAL at every
  clean shutdown, so every run after one starts with an empty WAL that
  grows: frames are appended, the filesystem allocates, and on ext4 its
  ordered mode keeps new WAL blocks invisible until the journal commits
  the file size. Only one WAL generation after a forced restart, frames
  overwritten in place, is tested; the first version of the test ran the
  growing regime, but without the tear fix above, so it is not covered.
- Crash points inside a checkpoint and the WAL's restart, the database's
  creation, and `FinishRun`'s checkpoint and clean-shutdown commit (all
  before the start mark or after the cut).
- Crashes before the first FLUSH after the start mark.
- An epoch's ordinary writes with only some of its FUA writes; tears finer
  than 4 KiB.
- btrfs's within-epoch states (all equal by design, above).

A follow-up could run the script twice (a growing WAL from empty, then
the overwrite regime) and build reference states across a checkpoint.
