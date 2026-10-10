# Phase 6 — Test tiers and test speed

**Rule (russ): quality over speed.** No optimization may weaken a test:
same assertions, same coverage, same isolation (one fresh guest per test).
Every speed change is reviewed for that, with before/after timings.

**6.1 Tiers, using Bazel's native test sizes.** Every test's `size`
attribute is its tier, filtered with `--test_size_filters` (Bazel 9.2):
- `small` = fast: unit tests and quick e2e tests; run constantly during
  development (`bazel test --config=fast //...` =
  `--test_size_filters=small`).
- `medium`: the rest of the per-feature e2e matrix (ext4/xfs/btrfs),
  pjdfstest, NFS; run before submitting (`--config=presubmit` =
  `--test_size_filters=small,medium`).
- `large` / `enormous` = slow: power-loss replay, fsstress/fsx, xfstests,
  the systemd guest, full benchmark runs, the ASan matrix; run by CI (no
  filter).
`size` also sets Bazel's default timeout and its resource estimate for
scheduling (small assumes ~20 MB of RAM), which is wrong for a QEMU guest
(256 MB-1 GB). So each QEMU test also declares its real needs (the `cpu:N`
tag and a memory resource tag; verify the exact tag syntax Bazel 9.2
accepts) and an explicit `timeout`, so a `small` QEMU test does not make
Bazel start too many guests at once. Today every QEMU test is `large`
(`qemu_test.bzl` default); sizes are reassigned from measured times
(target: all small tests in about a minute with KVM, presubmit in about
ten).
**6.2 Speed, without compromise.** Candidates, each measured:
- Replace sleeps and polling loops in guest scripts with waits on the
  actual event (mount appears, process exits, file exists).
- Shard long tests (pjdfstest by test directory, xfstests by group,
  fsstress seeds) with Bazel `shard_count`.
- Let Bazel run guests in parallel: keep guest memory small and check host
  limits so parallel runs do not swap.
- Build images (filesystem images, initramfs, rootfs) once per build and
  copy them per test instead of rebuilding.
- The minimal kernel (Phase 3 (Drop the kernel patch; download and build the test kernel)) and the TCG investigation (5.1) for boot
  time.
**6.3 Why ASan is so slow, and fixing it** (russ, 2026-10-06; quality over
speed applies: every test keeps running under ASan with the same
assertions).
- Measure first: wall time of the full suite plain vs. `--config=asan`,
  split into build (cold and disk-cached) and test time; per-test guest
  boot and run time under each; and which targets the ASan configuration
  rebuilds.
- Leading hypothesis (unverified): `--config=asan` adds `-fsanitize=address`
  to every C/C++ compile, including third-party code, and since Phase 4
  rules_foreign_cc passes those flags into the configure scripts of QEMU,
  glib, e2fsprogs and busybox. An ASan run would then rebuild those tools
  instrumented and run every guest on an ASan-instrumented emulator.
  Likely fix: sanitize only our code (a Bazel feature or `per_file_copt`
  scoped to `//dcfs` and `//tools`, or building the tools in a
  configuration without the sanitizer flags), so the tools are identical
  in plain and sanitized runs.
- Other candidates to measure: ASan's runtime options in the guest
  (leak detection at exit, symbolization cost, `malloc` behavior), the
  dynamic ASan initramfs, guest memory size versus ASan's shadow memory.
- Done when: the full ASan suite's time is measured before and after,
  with each change's effect, and every test still runs under ASan.
- 6.3b (done 2026-10-07): the six foreign_cc tools (QEMU, mke2fs,
  debugfs, mkfs.xfs, mkfs.btrfs, busybox) are built once for every
  configuration. Their public labels are `exec_file` rules
  (`third_party/exec_file.bzl`, `cfg = "exec"`) over the `configure_make`
  outputs, so `--copt`/`--linkopt` never reach them: a cold sanitizer
  configuration no longer rebuilds about 890 s of tools, and e2fsprogs is
  built once instead of twice. Guarded by `//tools:tool_identity_test`
  (built files identical across configurations; every tier) and
  `//tools:tool_keys_test` (`aquery` action keys equal under plain, asan
  and ubsan; the weekly `tool-keys` CI job, since it starts a second
  Bazel server). Numbers in `notes/build-speed-2026-10-07.md`.

Order: 6.1 with the CI phase (CI needs the tiers); 6.2 continuously,
first pass right after 6.1.

## 6.4 Test speed from the first GitHub runs (2026-10-08)

Slowest (plain / ASan): formal:large_test 634/634 s, idle_long 624/627,
formal:nolock_test 506/506, destroy_test 500/224, bench_full 299/350,
nfs 219, pjdfstest shards 100-200 each. Per push: fast ~4 min, presubmit
~7, full = plain 35 + ASan ~40 + UBSan ~60-90 sequential (7.4b splits
them), coverage ~40 and reproducible ~12 in parallel.
- 6.4a Host-only tests (formal TLC, man, sbom, tools, repo shape) rerun
  under every sanitizer and coverage config because the config hash
  changes: mark them `target_compatible_with` incompatible under
  asan/ubsan/coverage (the banned_symbols pattern): -19 min per such job.
- 6.4b destroy_test writes 100k files at ~5 ms per create on the runner
  (489 s): 20k keeps every assertion (shutdown reads nothing, exact held
  counts) and the measurable shutdown cost; the 5 ms per create is the
  product finding (2 durable fsyncs per CREATE): 26.4b's create slope
  test decides whether fsyncs are per create or per sync point.
- 6.4c TLC worker count follows the runner (4 vCPUs; today 2 workers).
- 6.4d (russ, 2026-10-08): shard the ASan, UBSan and plain large-tier
  suites across runners with a job matrix (3 shards per suite to start:
  the sanitizer compile is ~10-15 min per runner, so more shards flatten
  out): a deterministic partition of the tier's test targets (sorted
  `bazel query`, index modulo N) computed by `.github/ci/test.sh
  --shard=i/n`, unit-tested; explicit targets so the test-result cache
  still applies; expected wall for a push ~30-40 min instead of ~2.5 h.

## 6.5 Test performance scrub (russ, 2026-10-08)

A dedicated investigator profiles the whole suite and takes the
low-hanging fruit, under the Phase 6 rule (same assertions, same
coverage, one fresh guest per test; before/after timings for every
change, each change its own commit):
- Measure first: per-test wall and guest time for every tier from
  `test.xml` and the serial logs (a quiet run of each tier in the lane,
  plus CI run 37841442160's testlogs artifacts for the runner's numbers),
  split into boot, setup, workload and checks; the ten biggest sinks per
  tier and what dominates each.
- Known candidates: guest scripts that create or stat thousands of files
  from the shell (a fork plus a FUSE request each: `testutil mkfiles`
  style batching, as 6.2 did for readdir_boundary); dcfs's own create
  cost (~4.5 ms of daemon CPU each: the fsync per create, which 23.10
  removes; measure, do not pre-empt); the ACE sequences (182 in ~10 min:
  per-sequence setup that could be shared without sharing state); the
  trace shards (~5 min each: how much is TLC startup per trace); TLC
  configurations (symmetry, view, worker count); initramfs size and boot
  time after 11.2b's two static binaries; `sleep`s and polling waits left
  in guest scripts (`grep -n sleep test/qemu/guest`); guest memory sizes
  that make boot slower than needed; pjdfstest shard balance; the strace
  tests' per-test dcfs restarts.
- Report: a table of before/after per changed test and per tier, and a
  list of what was left (with the reason: would weaken a test, or needs
  a protocol change).
Owner: dcfs-investigator, in the first lane that frees.

Status 2026-10-09: merged 6bf6bdb (lane-3, twelve commits; note
`notes/test-speed-scrub-2026-10-08.md`). The second round made the two
polls events: `testutil waitmount <mp> present|absent [pid]` (reads
/proc/self/mounts, then poll()s it for POLLPRI|POLLERR; with a pid it also
watches a pidfd and returns 1 if the process exits first) and `testutil
waitline <file> <text> [pid]` (an inotify watch on the directory installed
before the first read), both unbounded; start_daemon, power.sh's copy and
cancel_inventory's start_timed use waitmount (the `mount` check 0.11 s to
0.02 s, fault_ace_b 45.8 s to 24.8 s on a quiet host, 41 start checks
7.10 s to 4.63 s of guest time); `wait_for_line` removed, seven scripts use
waitline, the 40 s and 300 s caps gone. A hang now shows as the guest or
Bazel timeout, not a 10 s message. Waits left for 15.6b's allowlist: the
by-design ones, `quiesce_daemon`, and polls in write.sh, fault_freeze,
fault_recover, strace_lib, fault_dcfs_lib, mount_dcfs.sh. First-round
report, for the record: the host
was never quiet (load 10-35), so tier walls are bounds, not pairs: fast
1,699 s baseline to 791 s at load 11 (host-only tests also 56% down on
the same code, so the tier effect is inside the noise); from the guests'
own markers about 5% of fast-tier guest time, 4% of medium, 10-20% of the
ACE tests, 75% of nfs_test. Fixed cost per guest 4.5-6 s under load: 282
of the fast tier's 890 guest seconds (boot and initramfs: zstd or raw cpio
saves 0.3-0.6 s per boot but needs a pinned zstd or 400 MB more cache;
stripping dcfs loses crash backtraces: left). Changes: nfsd started with
`--grace-time 10 --lease-time 30` (nfs_test 221 s to 48 s); `mmapwrite
usr1`, a SIGUSR1 after the stat instead of fixed delays (3.15 s to 0.31
s); release_leak lets go of the lock when the warning appears (36.9 s to
22.1 s); pjdfstest `chmod/` moved to the rename shard (CI decides: here
rename+chmod exceeds chown, on the runner chown is longest); bench_full
under ASan is not a leak (five ASan daemons at 410-625 MiB VmHWM against
10-21 MiB plain): asan_mem 4352. Two changes made polls faster (start_daemon
1 s to 0.1 s: cache_permissions 18.2 s to 7.2 s, ACE 3.3 s to 1.9 s per
sequence; `wait_for_line` for the holders' READY lines 1.03 s to 0.13 s
in seven scripts): against russ's no-timers rule (2026-10-09), sent back
to become events (`testutil` waiting on /proc/self/mounts with poll(),
which reports mount-table changes; the holders writing their READY line
to a fifo the script reads). Left with reasons: dcfs's create cost
(readdir_boundary 82 s, destroy 253 s; 23.11), TLC (about 4.4 s of CPU per
run; no GC or AppCDS gain; batching needs the specs to change; MC_large
and MC_nolock have a VIEW but no SYMMETRY), the by-design waits (idle
windows, sync intervals, timestamp settles, flakey windows,
quiesce_daemon), rename.sh's 8 s lock (tried, no gain). Ten biggest
fast-tier sinks here vs the runner: dir_cache_fs_test 129/7 s (CPU),
fault_shutdown 54/27 (13 daemon starts), bench_smoke 43/6, fault_power
40/18, write 36/15 (fixed mmap delays, now gone), rename 33/18, fault_sites
32/3, syscall_traces 27/13 (quiesce waits), two TLC known_bug tests 25/9 and
23/5 (one JVM each). Final tree: fast 230 + 2, presubmit qemu 126, large
40 of 43 with three stress_long load timeouts that passed on rerun.

