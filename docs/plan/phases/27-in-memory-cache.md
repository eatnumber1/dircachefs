# Phase 27: an in-memory cache, by putting the database on tmpfs (russ, 2026-10-17)

russ: "I want users to request an in-memory database (effectively acting
only as an inmemory cache) ... Let's schedule adding that feature
somewhere between M2 and M3." Then, against a durability-policy
abstraction: "I'm concerned this will balloon into a lot of abstraction.
Can we do something like have the database behind a memfd, or something?
That way we don't have to rewrite any of the existing code, and the
fsyncs can stay but are no-ops."

A memfd does not fit SQLite's WAL (it creates `-wal` and `-shm` beside
the database's path). tmpfs does, with no code: `dcfs.cache_db=/run/dcfs/
<name>.db`. fsync on tmpfs returns at once; the WAL and its shared memory
work; a daemon restart finds the files and starts warm through ordinary
recovery; a reboot or power loss empties tmpfs, so the next start is a
first mount (always safe: nothing stale survives).

## 27.1 Prove it, document it

- **Tests:** a representative set of guest suites with the cache database
  on tmpfs; a daemon crash and restart (starts warm, recovery correct); a
  reboot-equivalent (tmpfs emptied: a fresh start, then correct).
- **Docs:** README ("An in-memory cache": tmpfs, `size=`, what a restart
  and a reboot do) and design.md. No new option, no new code path.

## 27.2 The kernel's caches when both are in memory (measure, then tune)

russ: "why cache inmemory twice?" With the cache on tmpfs, measure what
dcfs's cache adds over the kernel's dentry, inode and attribute caches
(answers without the backing after the kernel evicts, whole listings,
long negative entries) and whether this setup wants the kernel leaned on
more (26.40's effectively infinite timeouts, FOPEN_CACHE_DIR,
keep_cache). Tuning, not abstraction; decisions go to russ with numbers.
The answer goes into the code, not into the docs as advice (russ,
2026-10-17: "When we get an answer, put the answer into the code. Don't
make users have to figure out the right settings here."): dcfs detects
that its database is on tmpfs (statfs) and sets the kernel-facing
choices itself; no new option.

Scheduled between M2 and M3 (milestone M2.5).
