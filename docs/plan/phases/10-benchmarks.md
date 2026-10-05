# Phase 10 — Benchmarks

A small benchmark suite using google/benchmark (`google_benchmark`, BCR
1.9.5), run in the QEMU guest like the tests.
- Cases: cached `stat`, `lookup` (path walk), `readdir` of a large
  directory, `open`+`close`, small read through passthrough; the same
  operations directly on the backing filesystem for comparison; dcfs
  startup with a large cache (100k+ entries); crash-recovery time with a
  large dirty set.
- High-latency backing: a device-mapper `delay` target (kernel config
  `DM_DELAY`) under the backing filesystem to show cached operations stay
  fast while direct ones pay the latency; optionally an NFS backing once
  supported.
- Memory: dcfs's RSS after `find` over a 1M-entry tree, and after the
  kernel drops its inode cache (`echo 2 > /proc/sys/vm/drop_caches`, which
  sends FORGETs). A test (pass/fail) asserts the per-inode lookup map
  shrinks back near its baseline after the drop, i.e. memory is bounded by
  the kernel's inode cache, not by the tree; the benchmark records the
  bytes per referenced inode.
- Idle test (pass/fail, russ 2026-10-05): with dcfs mounted on a warm
  cache and only the activity a quiet server sees (`df`/`statfs`,
  `stat` and `ls` of cached paths, dcfs's own timers, sync points and
  database checkpoints, an NFS client idling on the export), the backing
  device's I/O counters (`/proc/diskstats`, reads and writes) stay at
  zero for several minutes. Test first: it must show today's behavior
  (if `statfs` or a timer touches the disk, that is the failing test).
  Short version in the `medium` tier, a longer one in the `slow` tier.
- Output: benchmark results printed to the test log, not pass/fail (VM
  timing is noisy). A smoke test runs each benchmark for one iteration so
  the suite keeps building and running in CI.
- Run before and after Phases 8, 9 and 11; record the numbers in the step's
  merge notes.
Owner: Sonnet. Order: after Phase 5 (CI runs the QEMU suite), before Phase 13 (Connected backing fds) (baseline numbers).
