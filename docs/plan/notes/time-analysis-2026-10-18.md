# Where the time goes in dcfs's build, test and CI loop (measured 2026-10-18, lane-8, main 9f8135a1)

Measured at load 23 to 72 on 8 cores; the CI figures are the load-free evidence. Copied into docs/plan/notes by the clerk, 2026-10-18.

Read this first. The host was never quiet: load average 23 to 72 on 8 cores (62 GB RAM, 7 GB swap in use,
`/proc/pressure/cpu` "some" 75 to 90 %) the whole session, because seven other lanes run Bazel, QEMU and TLC
(TLA+ model checker) at the same time. So:

* CI numbers (4-vCPU GitHub runners, no neighbours) are the load-free evidence. Every local wall time below
  says its load.
* Local CPU seconds are inflated by SMT/cache contention at that load (TLC: 0.7 to 1.2 ms of CPU per
  distinct state locally, against about 0.35 ms on a CI runner), but they are comparable between runs
  made side by side.
* My own Bazel runs were not capped (lane-8 has no `user.bazelrc`; the other lanes use `--jobs=4
  --local_test_jobs=2`), so they added to the load. The 2 h uncached run used `--jobs=4 --local_test_jobs=2`.

Scratch data (not in the repo): `.../scratchpad/{runs.json,jobs/,api/,jl/,jl2/,prof-39064376955/,citests-*.json,loc/,tlc/}`.

## 1. CI

Sources: `gh run list -R eatnumber1/dircachefs --json ...` (31 runs exist in all: 28 CI, 3 mutation),
`gh api repos/eatnumber1/dircachefs/actions/runs/<id>/jobs` (per-step timestamps; `gh run view --json jobs` has
no step times), `gh api .../actions/jobs/<id>/logs` (job logs), `gh run download 39064376955 -p 'bazel-profile-*'`
and `tools/ci_profile.py` (Bazel's own profiles of the last green run).

### 1.1 Shape and wall time

Job graph (ci.yml): `fast` -> {`presubmit`, `coverage`, `reproducible`, `mutation-changed`}; `presubmit` -> {`full`
x3 shards, `asan` x3, `ubsan` x3, `xfstests` x3}; `osv`, `subjects` stand alone; `badges` last. The wall of a run
is the serial chain fast + presubmit + the slowest wave-3 job.

Last green run 39064376955 (2026-10-17 19:53, wall 50.9 min):

| Stage | Starts at | Job wall (min) |
|---|---|---|
| fast | 0.0 | 11.7 |
| presubmit (and coverage 21.4, reproducible 18.2, mutation-changed 4.3) | 11.8 | 10.2 |
| wave 3: full 22.3/19.6/23.9, asan 21.5/27.1/17.9, ubsan 21.2/28.9/20.7, xfstests 15.5/17.5/17.7 | 22.0 | 28.9 (ubsan 1) |
| badges | 45.9 | 0.1 |

Trend (CI push runs since 10-08; wall min; G = green): 29 59 52 52 48 12 55 49 29 95 58 51 | G53 G45 G70 | 15 7 51 | 55 66 G51:
**flat at 45 to 70 min for the runs that got past `fast` (the early reds are 7 to 15 min), no improvement and no regression**; 23 of 28 CI runs ended red (xfstests/ASan/coverage findings), and a red run still spends
the whole 300 runner-minutes. What did change: cache restore of `fast` grew from 9 s (10-08) to 124 to 225 s
(10-17), save from 9 s to 117 to 228 s (section 1.3).

Runner-minutes of that run: 325 in all = tests/build steps 234 (72 %), cache restore 49 (15 %), cache save 34
(10 %), OSV badge scan 5, setup/checkout/uploads 3. Cache work is 83 runner-min, more than a quarter.

Mutation workflow (weekly; 10-11 runs): 97 to 148 min wall, 558 to 623 runner-min; the TLA+ mutation shard 2 of 3
takes 94 to 146 min while shards 1 and 3 take 31 to 48 min (imbalance), the mutate shards 45 to 109 min.

### 1.2 Inside a job (last green run, step durations from the jobs API, profile table from `tools/ci_profile.py`)

| Job | Restore Bazel cache | Test step | of which before Bazel's profile starts (cquery of all 951 targets) | profile wall | test execution wall (busy) | Save |
|---|---|---|---|---|---|---|
| fast | 124 s | 428 s | 0 | 428 | 106 (357) | 117 s |
| presubmit | 199 | 239 | 0 | 238 | 191 (714) | 127 |
| coverage | 125 | 1124 | 0 | 1124 | 439 (1632) | none |
| full 0/1/2 | 212/180/122 | 965/851/1177 | 175/328/309 | 790/523/868 | 785/519/864 | 128/120/117 |
| asan 0/1/2 | 148/242/229 | 995/1176/684 | 310/223/185 | 686/953/499 | 639/909/478 | 120/171/132 |
| ubsan 0/1/2 | 123/154/205 | 1004/1428/855 | 314/311/237 | 690/1117/618 | 496/942/456 | 124/122/157 |
| xfstests ext4 | 246 | 611 | 228 | 383 | 377 | 161 |
| reproducible | 22 (repo cache only) | 1060 | | 377 | 0 (two builds, each extracts LLVM 277 s) | none |

Reading it:

* `fast`'s test step is 428 s only because the extracted-LLVM cache missed: **@dcfs_llvm extraction 292 s**
  (profile row). Without it the step is about 136 s, and its Bazel critical path is one test,
  `//test/qemu:mount_dcfs_test` (75.8 s of the 79 s critical path).
* In the 12 shard jobs (full, asan, ubsan, xfstests) **175 to 328 s per job is spent before the profiled
  invocation**, in `test.sh`'s `bazel cquery 'tests(//...)'` over all 951 targets (13,200 configured targets,
  420 packages; in log full_0: 20:19:03 to 20:21:46 = 173 s, then the 19-target test invocation analyses
  "0 targets configured"). That is 3079 s (51 runner-min) per run. When the repo and LLVM caches have been
  evicted (section 1.3) the same window also fetches 2.8 GB of archives.
* Every wave-3 job's critical path is one test: full-0 `idle_long_test` 617 s, full-2 `formal:nolock_test`
  864 s (6.0 M states), asan-1 `idle_long_test` 621 s, ubsan-1 `idle_long_test` 646 s, ubsan-0 `cancel_test`
  193 s, asan-0 `pjdfstest_chown_btrfs` 302 s, xfstests ext4 `xfstests_5` 326 s. ubsan-1's profile wall is 1117 s
  although its critical path is 667 s: tests queue behind each other on 4 cores.
* Compile work that still happens in CI because of cache layout: asan "our compile and link" 133/98/63 s wall
  (411/249/113 busy), ubsan 253/234/212 (882/770/724 busy) in each shard (the shards rebuild the same
  instrumented objects), coverage 714 s wall / 2281 busy (it saves no cache by design), reproducible 2 x 277 s of LLVM
  extraction (it does not use the LLVM cache by design).
* Bazel's action-cache outcome per job, from the logs (`INFO: N processes`): fast 1731 disk-cache hits vs 198
  executed (90 %), presubmit 2064 vs 145 (93 %), asan-0 338 vs 174 (66 %), ubsan-0 255 vs 260 (50 %), coverage 261
  vs 1354 (16 %). Tests: with `--cache_test_results=no` the test results still come from the **spawn cache in
  the disk cache**, e.g. `formal:large_test` (9 min of TLC) shows 0.0 s in full-0. So the formal tests only
  cost when their inputs change (section 4).

### 1.3 Cache behaviour (the biggest finding)

Evidence: `gh api repos/eatnumber1/dircachefs/actions/caches` (2 live entries: `bazel-ubsan-1-...` 11.8 GB
and `bazel-asan-1-...` 11.7 GB, `actions/cache/usage` 23.4 GB), the "Cache Size" and "Cache hit/not found"
lines of 20 fast/presubmit job logs since 10-10 (jl2/) and 16 jobs of the last run (jl/).

* **The disk cache is not bounded in CI.** `prepare.sh` writes `build --experimental_disk_cache_gc_max_size=4G`,
  but the saved entry grows monotonically: 4.0 GB (10-10) -> 5.0 -> 5.7 -> 6.6 -> 7.3 -> ... -> **10.6 GB**
  (10-17, compressed 11.1 GB). Bazel garbage-collects the disk cache only when the server has been idle
  (`--experimental_disk_cache_gc_idle_delay`, which `bazel help build` documents with a 5-minute default; I
  did not re-verify the default, the server was busy), and in a CI job the server is used until the job
  saves its cache. Each restore then moves 10.6 GB and each save 10.6 GB: restore 124 to 242 s, save 117
  to 228 s in every one of 16 jobs.
* **The quota is 10 GB; each job saves its own 11 GB entry** (15 saves per run: fast, presubmit, 3 full, 3 asan,
  3 ubsan, 3 xfstests), so GitHub keeps only the last one or two. The small caches evict with them: the
  repository cache (key `bazel-repo-<lock hash>`, 2.8 GB) missed in **10 of 20** fast/presubmit jobs since
  10-10 and in **12 of 16** jobs of the last run; the extracted-LLVM cache (0.57 GB) missed in 11 of 20.
  A miss costs the 2.8 GB re-download plus the **277 to 292 s LLVM extraction** (fast 292 s in the last run,
  reproducible 2 x 277 s) and shows up as the 175 to 328 s `cquery` window of every wave-3 shard.
  Per-job own caches (`bazel-asan-N-...`) are essentially never restored; wave-3 jobs restore presubmit's
  ("Cache hit for restore-key: bazel-presubmit-..."), which holds none of the sanitizer objects.
* The primary key never hits (it ends in the commit); restore always goes through restore-keys (20 of 20 jobs
  found one).

Items found by diffing a second identical run: not applicable in CI; locally see section 5.

### 1.4 Slowest steps per job, what else is in the step lists

Setup (checkout, `prepare.sh`, Bazelisk download) is under 20 s everywhere; uploads 2 s; `osv` is 5.9 min of
which the SBOM/scan "Badge (OSV)" step is 292 s (parallel, not on the critical path); `badges` 6 s.

## 2. Local tiers

All with `--config=fast //...` on lane-8's output base (external repositories already present), shared disk
cache `~/.cache/bazel-disk-cache` (52 GB; `.bazelrc` caps it at 50 GB). Test log of every run:
`scratchpad/loc/`.

| Run | Command | Load (start -> end) | Wall | Notes |
|---|---|---|---|---|
| fast1 | clean server, warm disk cache, local action cache cold for this commit | 39 -> 42 (47 mid-run) | 547 s (Bazel "Elapsed") | 3340 processes: 2089 action-cache hits, 923 disk-cache hits, 155 sandbox, 68 local; 58 tests executed (sum 1959 s busy), 258 cached; critical path 145.7 s (`mount_dcfs_test` 126.9 s). Launch 15 s, pattern evaluation 8.6 s, analysis done by about 30 s. 4 tests FAILED: load-starved (`run-qemu.sh: load-starved ... host CPU pressure 87 % of the wall`), see below |
| fast2 | clean server, everything cached | 39 -> 57 | 264 s | 28 processes; 5401 action-cache hits; only the 3 `external`-tagged tests (`//tools:format_test` 48 s, `repo_shape_test` 23 s, `totw_citations_test` 9 s) and the 4 failed tests of fast1 re-ran; critical path 87 s is a re-run of `write_test_ext4` |
| fast3 | **uncached**: `bazel clean`, empty `--disk_cache`, `--cache_test_results=no`, `--jobs=4 --local_test_jobs=2` | 41 -> 28, 26 to 60 in between | **7347 s (2 h 02 min)** | 5429 actions, 302 tests, critical path 684 s; 2 failed (load-starved); the external repos were already extracted, so fetch/extraction is not in it |

Uncached fast tier, Bazel's busy seconds by kind (fast3 profile; total 28,618 s):

| Kind | n | busy s | share |
|---|---|---|---|
| Tests (302) | 302 | 8503 | 29.7 % |
| `StyleTidy` (clang-tidy, 100 files) | 100 | 7458 | 26.1 % |
| `CppCompile` | 526 | 6588 | 23.0 % |
| `StyleQuery` (clang-query, 118 files) | 118 | 4719 | 16.5 % |
| Genrule (Debian rootfs 60 s, xfstests rootfs 39, initramfs 32, Debian .debs) | 194 | 761 | 2.7 % |
| CppLink | 231 | 336 | 1.2 % |

`tools/style_checks.bzl` runs clang-tidy and clang-query as ordinary actions per source file inside
`//tools:style_checks_test`, a **small** test, so they are part of `--config=fast`. They are 42.6 % of the
uncached busy time, more than compiling; clang-tidy of `dcfs/dir_cache_fs_test.cc` alone took 589 s (the
critical path) at that load. They are cached per file and its transitive headers, so a change to a header
included everywhere reruns them for every dependant (100 + 118 actions, about 2 h of CPU at this load).
A single .cc edit costs about one tidy + one query (about 2 min at this load).

Presubmit and plain `bazel test //...` were **not** run locally: the medium tier is 119 more guest tests (CI:
presubmit test step 239 s on a quiet 4-core runner, 714 busy s) and the large/enormous tier is hours (CI: shard
walls 523 to 868 s three-way split, plus idle_long's 10 minutes), which at load 25 to 70 would be hours and
would starve the other lanes. Use the CI figures of section 1.2 for them.

`bazel analyze-profile` does not exist in Bazel 9.2 ("Command 'analyze-profile' not found"); I parsed the
JSON trace directly (`scratchpad/prof.py`, and `tools/ci_profile.py` for the table).

Load-starved failures: `run-qemu.sh` reports `[load-starved]` itself when host CPU pressure is >= 20 % of a
guest's wall. fast1 had 4 targets fail (create_test_ext4, fault_backing_test_ext4, sibylfs_selftest_test, write_test_ext4), fast2 3
and fast3 2. I verified the cause for sibylfs_selftest_test (guest timeout, `[load-starved]`, pressure 87 % of the wall) and
create_test_ext4 (`over its cost budget`, `[load-starved]`, 86 %); I did not read the others. Many passing tests also print
the `load-starved` warning. So at this load even a green verdict is partly luck.

## 3. Slowest 30 tests

CI wall of the Bazel test action in run 39064376955 (4-vCPU runner, 2 tests at a time, so each is slower than alone). "plain" =
fast/presubmit/full/xfstests job, asan/ubsan = the same test in those configs. Size/timeout from `bazel query
'tests(//...)' --output=xml`.

| # | Test | size/timeout | plain s | asan | ubsan | What dominates |
|---|---|---|---|---|---|---|
| 1 | formal:nolock_test | large/eternal | 864 | | | TLC, 6.0 M distinct states, 26.6 M generated; only reruns when dcfs.tla changes (else spawn-cache hit) |
| 2 | qemu:idle_long_test | large/long | 617 | 621 | 646 | by design: a 10-minute idle window (`guest/idle_long.sh`); near-zero CPU; three times per run |
| 3 | qemu:bench_full_test | enormous/eternal | 303 | 469 | 266 | benchmark loops in the guest |
| 4 | qemu:pjdfstest_rename_ext4 | enormous/eternal | 126 | 464 | 230 | guest work (pjdfstest suite), x3 configs |
| 5 | qemu:pjdfstest_rest_ext4 | enormous/eternal | 422 | 163 | 189 | same |
| 6 | qemu:pjdfstest_chown_ext4 | enormous/eternal | 168 | 399 | 165 | same |
| 7 | qemu:cancel_test | large/long | 332 | 42 | 193 | cancellation scenarios with injected delays |
| 8 | qemu:xfstests_5_ext4 | enormous/eternal | 326 | | | xfstests shard (no sanitizer runs) |
| 9 | qemu:xfstests_3_xfs | enormous/eternal | 325 | | | same |
| 10 | qemu:xfstests_3_btrfs | enormous/eternal | 325 | | | same |
| 11 | qemu:pjdfstest_rename_btrfs | enormous/eternal | 322 | 250 | 102 | pjdfstest |
| 12 | qemu:stress_long_test_btrfs | large/long | 312 | 99 | 92 | stress loops |
| 13 | qemu:pjdfstest_chown_btrfs | enormous/eternal | 235 | 302 | 174 | pjdfstest |
| 14 | qemu:stress_long_test_ext4 | large/long | 255 | 162 | 48 | stress |
| 15 | qemu:pjdfstest_rename_xfs | enormous/eternal | 243 | 204 | 167 | pjdfstest |
| 16 | qemu:xfstests_4_btrfs | enormous/eternal | 237 | | | xfstests |
| 17 | qemu:xfstests_1/2/4_xfs | enormous/eternal | 235 each | | | xfstests |
| 20 | qemu:destroy_test | large/long | 126 | 30 | 235 | destroy/recover loops |
| 21 | qemu:pjdfstest_chown_xfs | enormous/eternal | 180 | 204 | 225 | pjdfstest |
| 22 | qemu:idle_short_test_xfs | large/moderate | 76 | 215 | 78 | a 1 to 2 minute idle window, by design |
| 23 | qemu:pjdfstest_rest_btrfs | enormous/eternal | 166 | 215 | 132 | pjdfstest |
| 24 | qemu:xfstests_3_ext4, 2/1_btrfs, 6_btrfs, 1/2/4_ext4 | enormous/eternal | 178 to 198 | | | xfstests |
| 27 | formal:known_bug_atime_fill_no_touch_test | large/long | 157 | | | TLC 492 k states |
| 28 | formal:interrupt_nolock_test | large/long | 152 | | | TLC 1.04 M states |
| 29 | qemu:mount_dcfs_test | **small**/moderate | 76 | 172 | 77 | guest waits: 5 scenarios that slow the backing writes by `SLOW_WRITE_MS=2000` (dm-delay) and wait out the daemon's exit (host CPU is 15 to 19 s of the 78 s) |
| 30 | dcfs:dir_cache_fs_trace_a/b/c_test | medium/long | 63 each | 83 to 119 | 62 to 88 | trace replay against the TLA+ model (TLC + gtest) |

Per family, busy s in that run summed over configs: pjdfstest 5856 (22 % of all test busy time), xfstests 3702,
idle_long 1884, stress_long 1363, bench_full 1037, idle_short 917, nolock 864.

Tier vs measurement:

* **Too big for the tier**: `mount_dcfs_test` is `small` at 76 s (the whole small tier is 106 s of busy time
  otherwise, and it is the fast job's critical path). `idle_short_test_*` is large/moderate at 76 to 215 s.
* **Timeouts far above reality**: every `enormous/eternal` pjdfstest/xfstests/bench test measures 100 to 470 s
  against 3600 s (7200 s without KVM); `large/long` (900 s) tests measure 13 to 157 s except idle_long;
  `medium` tests: 41 of 72 measured plain tests run under 5 s (the formal known-bug and trace tests take
  0.3 s, the QEMU xfs/btrfs variants 1 to 2 s). The timeouts are only guards (they are not load-agnostic
  verdicts), so lowering them would shorten hangs but not CI.
* Medium TLC tests are pure JVM startup: of 168 `tlc_test` targets, 79 are small (a few hundred to tens of
  thousands of states, 0.3 s), 68 medium, 21 large.

Repeated expensive setup that could be shared:

* ASan and UBSan each rebuild the same instrumented objects in each of their three shards (section 1.2).
* The `pjdfstest` suite (3 filesystems x rename/chown/rest) runs in plain, ASan and UBSan: 97 busy minutes in one
  CI run. Likewise `idle_long` (3 x 10 min). Whether a sanitizer adds value on pure-idle or conformance
  workloads is a coverage decision (not mine); the measured cost is given so russ can choose.
* Every guest repeats a fixed boot (section 6) and `qemu_test_matrix` builds a guest per filesystem.
* `style_checks_test` actions re-run for the coverage configuration (section 1.2 / opportunity 6).

## 4. Formal (TLC)

Distinct states and times: per-target `Finished in` and "states found" lines in the other lanes' `bazel-testlogs/formal/*/test.log`
(read only; 154 targets with logs) and the CI profile. Because the spawn cache serves unchanged TLC runs, CI
executed only 4 formal TLC tests in the last run (nolock_test 864 s, known_bug_atime_fill_no_touch 157 s,
interrupt_nolock 152 s, nolock_small_coverage 97 s). When `dcfs.tla` changes, everything re-runs.

Dominant configs (best observed time over the lanes, distinct states): small_coverage_test 841 s (1.48 M; TLC
with `-coverage`, about 5x slower per state than small_test's 160 s), large_test 460 s (7.24 M), nolock_test 321 s
(6.04 M), interrupt_muts2 194 s (0.89 M), interrupt_nolock 170 s (1.04 M), nolock_small_coverage 163 s,
small_test 160 s (1.48 M), known_bug_rename_stale_source 111 s (0.98 M); 154 targets sum to 3714 s best case
(about 1 h at 8 free cores, 27.9 M distinct states in all). The two `-coverage` tests are 1004 s of the 3714 s
(27 %) and exist to check that every action is still taken, on the biggest configs.

Under today's lane load the same runs take much longer (same-day logs): large_test 36 and 59 min (8 workers),
nolock_test 48 and 50 min, against 7 min 40 s on 10-07 (4 workers, quiet machine) and 10.5 / 14.4 min on CI.

### 4.1 TLC scaling experiment (requested addition)

Command (mirrors `third_party/tlaplus/tlc_test_runner.sh.tpl`; the pinned JDK 21 and `tla2tools.jar` from lane-8's
output base): `java -XX:+UseParallelGC -XX:ParallelGCThreads=N [-Xmx..] -Xss4m -jar tla2tools.jar -workers N
-cleanup -metadir ... -config MC_x.cfg MC`, wrapped in `/usr/bin/time -v`; `scratchpad/tlc/run.sh`,
results in `scratchpad/tlc/results{1,2,3}.txt`. The machine was never idle, so wall times are shares of an
oversubscribed CPU: **CPU seconds are the load-robust column**.

interrupt_test (MC_interrupt: 280,869 distinct, 1.31 M generated), GC threads = workers:

| workers | wall | CPU (user+sys) | CPU/state | max RSS | load during |
|---|---|---|---|---|---|
| 8 | 343 s | 325 s | 1.16 ms | 5.69 GB | 38 -> 57 |
| 4 | 505 s | 316 s | 1.12 ms | 5.67 GB | 57 -> 44 |
| 2 | 761 s | 322 s | 1.15 ms | 5.70 GB | 44 -> 40 |
| 1 | 1698 s | 293 s | 1.04 ms | 5.69 GB | 40 -> 53 |

liveness (MC_liveness: 146,504 distinct; part of the run is single-threaded liveness checking), load 22 to 27:

| workers | wall | CPU | max RSS (default heap) |
|---|---|---|---|
| 8 | 114 s | 105 s | 3.01 GB |
| 4 | 131 s | 96 s | 2.90 GB |
| 1 | 261 s | 104 s | 3.17 GB |

small_test (MC_small, 1,479,307 distinct), 8 workers default heap, load 64 -> 55: wall 24 min 53 s, CPU 1438 s
(0.97 ms/state), RSS 5.97 GB.

Reading:

* **TLC burns the same CPU whatever the worker count** (interrupt_test 293 to 325 s for 1 to 8 workers, within
  load noise; liveness 96 to 105 s). So fewer workers do not make TLC itself cheaper in CPU, and more workers do
  not waste CPU either. The wall difference is just the share of the oversubscribed CPU each thread
  wins: at load 40 to 57 on 8 cores a 1-worker TLC gets about 17 % of a core (1698 s for 293 CPU-s), 8 workers about 1 core.
  Where the CPU was free the speed-up was less than linear because of serial phases (liveness: 8 workers 2.3x of
  1 worker, 4 workers 2.0x, so 4 -> 8 is worth 1.15x).
* **Memory is the loud effect, not CPU.** The default heap (a quarter of 62 GB = 14.3 GB; no `-Xmx`) lets
  ParallelGC grow the heap to what it likes: RSS 5.7 to 6.2 GB for every JVM, whatever the model. Capped:

| config | -Xmx | workers | pass | max RSS | CPU |
|---|---|---|---|---|---|
| nolock_small (343,294 distinct) | default | 4 | yes | 5.71 GB | 285 s |
| nolock_small | 1g | 4 | yes | 1.05 GB | 243 s |
| nolock_small | 512m | 4 | yes | 0.67 GB | 294 s |
| liveness (146,504) | default | 4 | yes | 2.90 GB | 96 s |
| liveness | 1g | 4 | yes | 0.58 GB | 99 s |
| small_test (1,479,307; 124 k states max on queue) | default (8 workers) | 8 | yes | 5.97 GB | 1438 s |
| small_test | 1536m | 4 | yes | 1.75 GB | 1051 s (load 33 to 44, so not comparable) |

  The largest targets I could not re-run (7.2 M states at about 1 ms/state would be 2 h of CPU under today's load).
  Evidence from the logs: lane-5's 10-07 run of `large_test` and `nolock_test` had `14290MB` replaced by
  **2660 MB heap** (the host had 11.9 GB), 4 workers, and both passed (7 min 40 s, 5 min 21 s; max 492 k and 372 k
  states on the queue, against 709 k / 523 k on the 14 GB runs).
* Right now about 4 TLC JVMs x 5.7 to 6.2 GB = 23 to 25 GB of the 43 GB the host showed in use at one moment
  (free 929 MB, 21 GB page cache squeezed, 7 GB swapped).
* The runner script's `-workers auto` = number of cores: 8 locally (JVM has 35 threads incl. 8 GC threads), 4 on CI.

Recommendation (fixed per target class; `tlc_test` macro gets `workers` and passes `tags`, the runner template
gets `-XX:ParallelGCThreads=@@WORKERS@@` and `-Xmx@@XMX@@`; classes by distinct states):

| Class | Targets | -workers | cpu tag | GC threads | -Xmx | memory tag | Basis |
|---|---|---|---|---|---|---|---|
| tiny, < 50 k | 79 small + about 25 medium (recovery 39 k, trace/known-bug tests) | 1 | cpu:1 | 1 (or SerialGC) | 256m | resources:memory:400 | JVM startup dominates (0.3 s each); not measured at this size directly |
| medium, 50 k to 500 k | interrupt 281 k, liveness 147 k, nolock_small 343 k, atime*, lifetime, crash*, reval* | 2 | cpu:2 | 2 | 1g | resources:memory:1300 | measured: 512 m and 1 g pass at 0.67 to 1.05 GB RSS; CPU equal |
| 0.5 M to 2 M (small_test, interrupt_muts2/nolock, rename_stale_source, atime_view*) | about 15 | 4 | cpu:4 | 4 | 1536m (liveness 1g measured; small_test 1.5 GB measured) | resources:memory:2000 | measured for small_test |
| large, 6 M to 10 M | large_test, nolock_test (+ coverage twins of small/nolock_small) | 4 | cpu:4 | 4 | 3g | resources:memory:3500 | lane-5's 2.66 GB run passed; 4 workers equals a CI runner (CI times and timeouts unchanged); unmeasured today |

Expected machine-wide effect, honestly: TLC's own throughput (states per CPU-second) is not changed by the worker
count (+/- 5 %, within noise), so I do not claim a gain in TLC throughput. The gains are (a) memory: from 5.7 to
6.2 GB per JVM to 0.6 to 1.8 GB for everything below the large class, about 80 % less, 20+ GB freed when four are running;
(b) honest accounting: today every TLC test is 1 CPU to Bazel and takes 8 (+8 GC threads), so
`--local_test_jobs=2` per lane really means up to 16 TLC threads per lane against 8 cores; with cpu tags a lane
can bound itself (add `build --local_resources=cpu=4` to the lanes' user.bazelrc and a lane can then no longer start
more than one 4-worker TLC next to a guest); (c) the guests that share the machine stop being starved:
a guest's vCPU thread competes with 8 TLC threads x number of JVMs for shares (CPU pressure 75 to 90 % all
session; at least 2 guest tests failed `[load-starved]` in my runs). I could not measure (c) in isolation; I would
expect the load-starved failures and timeouts to drop, not the totals. Large-model wall on a free machine: unmeasured.

## 5. Build side: kernel, QEMU, images, initramfs, caches

* Kernel (`@alpine` linux-virt), QEMU, busybox, mkfs tools are fetched Alpine packages and Debian `.deb` extractions,
  not compiled; the Debian rootfs genrule is 60 s, xfstests rootfs 39 s, `:initramfs` 32 s, the other 17
  initramfs variants 51 s together (fast3 profile, load 26 to 60). They re-run when `dcfs` or the guest scripts
  change; no sign of them re-running with nothing changed.
* Rerun check: fast2 (clean server, nothing changed) executed 28 processes: 6 sandboxed (4 failed
  load-starved tests + their re-runs) and 8 local, 1 `BazelWorkspaceStatusAction`, 7 `PyWriteBuildData`
  (0.3 s each). **The only real "reruns with nothing changed" are the three tests tagged `external`**:
  `//tools:format_test` (48 s at load), `repo_shape_test` (23 s), `totw_citations_test` (9 s), which Bazel never caches. Every
  `bazel test --config=fast` pays about 80 s for them at that load (they take a few seconds on a quiet machine).
  Failed tests are not cached either; at load 40+ that is how a "clean" fast run keeps re-running 4 guest tests.
* A fresh output base needs the @dcfs_llvm extraction: 277 to 292 s on a CI runner, 31.6 min at load 18 to 37 locally
  (docs/plan/notes/ci-profile-2026-10-09.md); lane-8 avoided it. Eight output bases exist (one per lane), each with its own copy
  of the extracted toolchain (2.1 GB each).
* Cache config: no remote cache or executor anywhere (`grep -rn remote_cache .bazelrc .github` is empty). Local:
  `build --disk_cache=~/.cache/bazel-disk-cache`, gc max 50G (the directory is 52 GB, so GC works at idle),
  shared by all lanes. CI: `prepare.sh` sets disk cache, `--repository_cache`, `--repo_contents_cache` (unsaved),
  gc max 4G (ineffective, section 1.3). `.bazelrc` already has `--lockfile_mode=error`.

## 6. Per-guest overhead

* Trivial guest `//test/qemu:boot_test`: Bazel's own test-action wall in CI 1.2 s (fast), 1.7 s (asan), 1.6 s (ubsan),
  2.8 s (coverage). `run-qemu.sh`'s own line, from 6 lane logs: guest wall 1.4 s, host CPU 1.2 s at CPU pressure 1 to
  3 %, kernel uptime 0.7 s at the end; 2.6 s with 29 % pressure and 9.1 s at 81 %. (`cache_permissions_test`: 1.3 s CI, 1.5 s local.)
  A guest that starts a daemon and mounts costs 2.5 to 6 s ("fixed cost" 2.5 to 6 s in
  docs/plan/notes/test-speed-scrub-2026-10-08.md).
* QEMU test executions in the last CI run: fast 46, presubmit 58, full 49, asan 143, ubsan 143, coverage 94,
  xfstests 18 = 551. At about 1.2 to 2.8 s of fixed cost each that is **11 to 25 runner-minutes of 234**, i.e. 5 to 10 % of
  the test steps. Boot is not where the time goes; the slow guests are slow because of the work in them
  (idle windows, pjdfstest, dm-delay waits, stress).
* Median QEMU test action: fast 2.7 s, presubmit 4.4 s, asan 17.5 s, ubsan 13.4 s, coverage 6.4 s, full 41.6 s, xfstests 178 s.

## 7. The agent loop (secondary)

* `docs/plan/dispatch-ledger.tsv` has durations for only 2 of 85 rows; `~/.cache/dcfs-dashboard/{running,finished}.json`
  have no timestamps. So I measured the subagent transcripts (`~/.claude/projects/-home-russ-Sources-dircachefs/4cb5bd53-.../subagents/*.jsonl`, 207
  agents with more than 10 active minutes, read-only): "active" time = sum of gaps between transcript events
  at most 30 min (longer gaps are dropped), Bash tool-call duration from tool_use to tool_result.
* Result: **active 480 h; `bazel test|build|coverage|run|query` calls 191 h = 40 %**; other shell commands 184 h
  (38 %; includes `git`, `grep`, and calls that wait on backgrounded Bazel), model thinking and the rest 22 %.
  Per agent the Bazel share has quartiles 3 % / 20 % / 54 %; the most test-heavy steps (15.6, 25.28b, 23.10, 11.5)
  spend 37 to 78 % there (up to 590 minutes of one agent's 757).
  Median agent active time 55 min. Lower bound, since waits that outlast the tool timeout continue as background tasks.
* Those Bazel waits were measured at load 30 to 120 (`docs/plan/execution.md` records load 120), so a good part is
  contention rather than work (fast tier: CI test step 2 to 7 min; locally 4.4 to 9 min with everything cached, at load 40).

## 8. Ranked: where the time goes, end to end

(runner-minutes are for the last green run, wall for the critical path; agent hours are for the whole session.)

1. **Agent waiting on Bazel on an oversubscribed host**: 191 of 480 agent hours (40 %); the host is at CPU pressure
   75 to 90 % all day, 8 cores for 7 lanes x 2 test jobs x guests/TLC.
2. **CI cache movement**: 83 runner-min per run (restore 49, save 34), 14 min of the 51-min wall chain, from an unbounded
   10.6 GB cache against a 10 GB quota; plus its secondary effect, repo/LLVM cache eviction: 292 s of LLVM extraction in
   `fast`, 175 to 328 s `cquery` windows in 12 shard jobs (51 runner-min) and 2 x 277 s in `reproducible`.
3. **CI serial chain fast -> presubmit -> wave 3** (11.7 + 10.2 + 28.9 min = the 51-min wall; waves could overlap).
4. **Wave-3 test time**: 5 to 25 busy minutes per shard; unbalanced shards (wall 499 to 1117 s within a family); the
   critical tests are idle_long (10 min x 3), nolock_test (14 min), pjdfstest (22 % of all test busy time).
5. **Style actions** (clang-tidy/query): 43 % of the uncached fast tier (12,177 of 28,618 busy s); 1618 busy s in every CI
   coverage run (70 % of that job's build).
6. **TLC**: 27.9 M states, about 1 h of 8 free cores per full re-run, 4 to 8 times that on today's host;
   5.7 to 6.2 GB per JVM.
7. Instrumented rebuilds (coverage 714 s, ubsan/asan 63 to 253 s per shard).
8. Weekly mutation workflow (560 to 620 runner-min, wall 97 to 148 min, one TLA+ shard 3x longer than its peers).
9. Per-guest fixed cost: 11 to 25 runner-min per run.
10. Setup, uploads, badges: under 3 runner-min.

## 9. Top 10 opportunities (savings are estimates; cost; rule check)

1. **Make the CI disk cache bounded and single-writer.** Only one job saves the disk cache (the presubmit-like
   canonical one), capped at about 4 GB by an effective GC (the 4G flag never fires; either
   `--experimental_disk_cache_gc_idle_delay=0` if Bazel honours it at exit, which I did not verify, or a size-bounding step
   before the save), all other jobs restore read-only. Repo cache (2.8 GB) and LLVM cache (0.57 GB) then
   stay inside the 10 GB quota (about 7.5 GB in all). Saves: about 55 runner-min per run (14 jobs x 2 min saves, restores
   from 10.6 GB to 4 GB), 8 to 10 min of the 51-min wall (fast 241 s of restore/save, presubmit 326, wave-3
   restore 120 to 240 + save 120 to 170), plus the 292 s LLVM extraction in the fast job and 2 to 4 min of refetch in
   half the shard jobs. Cost: a small change to ci.yml/prepare.sh; risk: lower hit rate for ASan/UBSan/coverage objects (see 7). Respects all rules.
2. **Start wave 3 after `fast`, not after `presubmit`** (`needs: fast` for full, asan, ubsan, xfstests; keep
   `presubmit` and wave 3 parallel). Saves about 10 min wall per run (presubmit's 10.2 min), 0 runner-min.
   Concurrent jobs would rise from 13 to about 17 (limit 20 for public repositories). Cost: wave 3 runs even when
   presubmit is red (wasted runner time on the 80 % red runs; "red CI is fine until ship" is the standing rule).
3. **Balance the wave-3 shards by measured time** (`.github/ci/shard.sh` hashes names): per family wall is 790/523/868 s
   (full), 686/953/499 (asan), 690/1117/618 (ubsan). A longest-processing-time partition from a committed durations table
   makes each about 700 to 780 s: saves about 5 to 6 min of wall (ubsan-1 1117 -> about 780 s), 0 runner-min. Cost: a
   table to refresh (can be generated from the uploaded profiles by `tools/ci_profile.py`); the idle_long tests
   (zero CPU, 10 min) should sit in a shard with CPU-heavy tests rather than alone. Same trick for the
   weekly mutation TLA+ shards (94 to 146 min vs 31 to 48 min): saves about 50 min of that workflow's wall.
4. **TLC resource classes** (section 4.1): fixed `-workers`, matching `cpu:N`, `-XX:ParallelGCThreads`, `-Xmx` and
   `resources:memory:` tags. Saves: about 80 % of TLC memory (20+ GB when four run), true CPU accounting; TLC throughput itself is unchanged (+/- 5 %).
   CI wall unchanged (large keeps 4 workers = 4 vCPU). Cost: edit `tlc.bzl`, the template and 168 call sites' classes (one `workers`
   attribute plus a derived tag); needs a model-owner look at the large class's `-Xmx` (3g evidence from 10-07, not re-measured today).
5. **Drop `//tools:style_checks_test` (and its 1618 busy s of clang-tidy/query) from the coverage run** (`coverage.sh` target list or
   `target_compatible_with` for coverage): about 5 to 7 min off a job that ends at 21 min and 27 runner-min per run;
   no coverage is lost (style actions are not instrumented code). Cost: one line; the style gate still runs in fast/presubmit.
6. **Split `mount_dcfs_test` into 3 to 4 targets** (its 5 slow-write scenarios look independent): it is the whole critical path of
   `fast`'s test portion (76 of 79 s), so that tier drops by 40 to 50 s per run, and the same in ASan (172 s)
   and the dev loop; the 4 boots cost 4 x 1.2 s CPU. Cost: a test refactor of an 888-line guest script (I did not check that the scenarios are really independent).
7. **Per-shard warm caches for the instrumented builds**: with the cache bounded (1), one more entry each for
   `asan`/`ubsan` (shared by their three shards) and `coverage` fits the quota; saves about 4 min on ubsan-1's critical path, 10 min of
   coverage (not critical) and 3 x (63 to 253 s) of recompiling per sanitizer; lowers wave-3 wall by 3 to 4 min if
   (3) is not done first. Cost: two more cache entries (4 GB each; check the quota).
8. **Shard listing with `bazel query` or a committed shard manifest instead of `cquery` over 951 targets**: cold-server
   locally `query` 24 s vs `cquery` 55 s (load 27); on the runner the listing window is 173 s of cquery
   (cache-hot) to 330 s (cache-cold). Saves 1 to 3 min per shard job (12 jobs: 12 to 30 runner-min, 1 to 3 min wall). Cost: `test.sh`
   must still drop incompatible targets (`IncompatiblePlatformProvider`); a committed manifest checked by a test avoids
   the problem entirely.
9. **Decide the duplicated expensive sanitizer runs** (needs russ; this would weaken checking, so I do not recommend it
   without a ruling): `idle_long_test` x3 (31 runner-min), pjdfstest x3 (97 busy min), `bench_full_test` x3 (17 busy
   min) in ASan/UBSan. Saves up to about 150 busy minutes (about 50 wall minutes spread over six shard jobs, 10 min of wall on the critical path).
10. **Local lanes: cap each lane's CPU and TLC memory** (`build --local_resources=cpu=4` plus (4)): the host at
    CPU pressure 80 to 90 % makes `[load-starved]` failures and 4x-inflated agent waits (agents spend 40 % of their active time in Bazel). Saves: not
    measurable here, but a lane's two 4-worker TLCs cannot then displace guests; the `external` tests could also
    be tagged cacheable (their inputs are declared files; 80 s of every fast run at load, a few seconds quiet).

Not an opportunity: guest boot (6 %), the formal tests in CI (cached unless the model changes), CI setup (< 3 runner-min).

## 10. What I could not measure, and why

* Presubmit and plain `//...` locally (hours at this load, would starve the other lanes); CI numbers used.
* TLC scaling on an idle machine, and any run of the 6 to 10 M-state targets (`large_test`, `nolock_test`): the machine is
  never idle and these need 2 h of CPU at today's rates; speedup beyond share-of-CPU is therefore
  known only from liveness (2.3x at 8 workers, 2.0x at 4 with 1/3 core per thread).
* The effect of Opportunity 1's GC setting on CI (I cannot run CI; `--experimental_disk_cache_gc_idle_delay`
  semantics are from Bazel's documentation, not tested).
* Per-test durations inside CI logs: successful jobs' test.logs are not uploaded; I used the profiles' `TestRunner`
  events instead (the "PASSED in" lines only appear in failed jobs).
* Mutation workflow per-mutant cost (no Bazel profile by design) and the `noisy` schedule (one run, 10-12: six
  jobs 15 to 55 min).
* `gh run view --log` returned nothing here; I used the jobs API logs.
* Agent-loop hours are a lower bound and include load-inflated waits (section 7).
