# Phase 27: an in-memory cache mode (russ, 2026-10-17)

russ: "I want users to request an in-memory database (effectively acting
only as an inmemory cache). If we do that it has some implications too on
in-kernel inode/attr caching (why cache inmemory twice?). Let's schedule
adding that feature somewhere between M2 and M3."

## 27.1 Design (protocol agent, with the model)

- **The option:** `dcfs.cache_db=:memory:` (or a dedicated flag; decide),
  an SQLite in-memory database: the cache lives only as long as the
  daemon. A restart, clean or not, starts cold.
- **What goes away in this mode, by construction, not by branches
  everywhere:** durability. No WAL, no fsync of the cache, no synchronous
  phase-1 commit, no dirty set and no recovery (a crash loses the whole
  cache, which is always safe: nothing stale can survive it). Decide how
  the code expresses that once (a durability policy chosen at open), so
  25.28b's `MakeDurable` and 25.28d's poison have a defined meaning
  (no-ops) rather than a RET_CHECK.
- **What stays:** the backing syncfs at sync points? Decide: dcfs's dirty
  set exists to bound the re-read after a crash; with no persistent cache
  it may have no job, and the sync point may reduce to nothing.
- **The model:** a configuration where a crash discards the cache; the
  correctness properties must still hold and be simpler to check.
- **Kernel caching ("why cache in memory twice?"):** the kernel's dentry,
  inode and attribute caches already hold what was looked up, but they
  are evicted under memory pressure, hold no complete directory listings
  (unless FOPEN_CACHE_DIR), and keep no negative entries for long. Decide,
  with measurements, what dcfs's in-memory cache still adds (answers
  without waking the backing after kernel eviction; whole listings; long
  negative entries) and whether this mode should lean on the kernel more
  (effectively infinite timeouts, FOPEN_CACHE_DIR, keep_cache) and keep
  less itself. Builds on 26.40's timeouts and 12.20's kernel-cache model.
- **Memory:** a bound or a measurement per entry, documented.

## 27.2 Build, tests, docs

Implementation per 27.1, tests (the mode under the existing guest suites
where they apply, crash tests showing a cold start), README and
design.md, the man page.

Scheduled between M2 and M3 (docs/plan/README.md, Milestones).
