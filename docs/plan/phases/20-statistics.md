# Phase 20 — Statistics

Goal: show whether the cache is doing its job. No work before russ agrees
on a design. Questions to settle:
- What to count: requests answered from the cache vs. requiring the
  backing filesystem, per operation; fills; out-of-band detections;
  dirty-set size; backing syscall counts and latency; cache DB size.
- How to expose them: a file under `/run/dcfs/<instance-id>/`, a `dcfs
  stats MOUNTPOINT` command, a log line on a signal or interval, a virtual
  xattr on the mount root, or something else.
- Persistence: reset per run or kept in the cache DB.
- Cost: counters must not add backing I/O or measurable latency (Phase 10 (Benchmarks)
  benchmarks check).
