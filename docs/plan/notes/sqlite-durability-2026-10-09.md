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
- **The bound.** For a crash state at or after FLUSH f: at least the last
  synced commit of every operation whose mark is before f.

## Replay coverage

The cache disk's log is replayed (CrashMonkey's method) to: every entry
before each FLUSH after the start mark (all of them, always) and the whole
log; per epoch between two FLUSHes, every prefix of its FUA writes without
its ordinary ones; all its FUA writes with a subset of its ordinary writes
(every subset if there are at most 64 or they fit the epoch's share of the
budget, else each write lost, each write kept, then seeded random subsets);
and each ordinary write of more than one 4 KiB block torn at five cuts
(first block, first half, all but the last block, last block, all but the
first), with the epoch's other writes and without them. Not explored: an
epoch's ordinary writes with only some of its FUA writes (the log does not
order them against each other), and tears finer than 4 KiB.

Before the measured part dcfs creates files until SQLite checkpoints and
restarts the WAL (44 creates in every run), so every later frame overwrites
an old one in place: no allocation for the filesystem's journal to order,
so the frame writes' reordering reaches SQLite's recovery. Without the
restart (the first version of the test), ext4's ordered mode made every
crash state in an epoch the same: new WAL blocks are invisible until the
journal commits the file size.

Final run (seed 1, budget 1500; the short variant 500):

| Target | Log entries | FLUSHes checked | States | Wall time | Guest peak used |
|---|---|---|---|---|---|
| `sqlite_durability_test_ext4` | 617 | 37 | 447 | 71.5 s | 103 MiB |
| `sqlite_durability_test_xfs` | 328 | 31 | 338 | 54.0 s | 121 MiB |
| `sqlite_durability_test_btrfs` | 616 | 27 | 2026 | 209.2 s | 128 MiB |
| `sqlite_durability_short_test` (ext4) | 512 | 18 | 199 | 29.7 s | 101 MiB |

Most epochs were taken exhaustively; the sampled ones were the unmount's
(ext4: 122 of 1023 subsets plus 20 torn writes; btrfs: 16 of 1048575
subsets plus 190 torn writes) and one 7-write epoch (102 or 38 of 127). The
full script makes 204 commits after the restart (494 frames), which leave
138 distinct databases: several commits change nothing (a release record
equal to the row), so a lost one is not observable. 13 to 218 states per
run lost a commit of an operation done before the crash; xfs and ext4 also
recovered states after the last synced commit (194) but short of the last
commit (200, 203; 204 is the last): the final operations' normal commits
surviving as a prefix. The smallest
margin over the bound was 0 commits on btrfs and 3 on ext4 and xfs.

Timing with budget 4000 under the same load: btrfs took 839.5 s (3277
states, 0.24 s each), too close to `long`'s 900 s, hence 1500. Memory:
`mem = 384` (peak 128 MiB used plus the kernel's 30 MiB is 158; plus
128 MiB is 286, but the log and the replay disk live in tmpfs and grow with
the script); the ASan allowance was not measured (the default, 384).

## Verdict

The abstraction holds on ext4, xfs and btrfs: 0 violations in every run
of every target while the test was developed (the table is the last run,
with the final script). The oracle's self-checks pass and
were shown to fail first (an oracle that accepted everything: the forged
"commit 130 lost, 131 kept" WAL, the state before the last synced commit
and the full-log state all reported wrong).

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

Gaps left: crash points inside the checkpoint itself and the WAL's restart
(they are before the start mark), the database's creation, `FinishRun`'s
TRUNCATE checkpoint and clean-shutdown commit, and the mixed FUA/ordinary
subsets above. A follow-up could start the log before the warm-up and
build reference states across one checkpoint.
