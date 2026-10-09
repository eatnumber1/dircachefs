# Test speed scrub, 2026-10-08 (step 6.5)

Phase 6's rule applies throughout: same assertions, same coverage, one fresh
guest per test. Every change below is its own commit with its before and
after; nothing under `formal/` was touched.

## Summary

- The suite's time is mostly **not** in shell loops or polling. Of the fast
  tier's 1,700 s (loaded host), 810 s are host-only tests (TLC under
  `//formal`, tools checks) and 890 s are guests; of a guest's time, 2.5
  to 6 s each is fixed cost (QEMU, kernel, initramfs, modules: 4.5 to 6 s
  under load), and the rest is mostly dcfs doing real work (about 4.5 ms of
  daemon CPU per create, per the plan: its fsyncs), idle windows and timers
  the tests are about, and fsstress/fsx.
- What was cheap and safe: `start_daemon` looked for the mount once a second
  (every daemon start, restart and refused start cost a whole `sleep 1`:
  at least 60 starts in the fast tier's guests, 115 in the medium tier's,
  and two per ACE sequence: 3.3 s per sequence before, 1.9 s after, at load
  15); the READY/MAPPED
  lines of testutil's holders were polled once a second; `nfs_test` waited
  out nfsd's 90 s grace period twice (103 s each of its 221 s); a database
  lock was held 30 s where the event it waits for comes at 15 s; two mmap
  checks slept for 3 s and 2 s to order a stat before a store.
- Left for other steps or lanes: the create cost (23.10), TLC (formal lane),
  the initramfs format (needs a pinned zstd), by-design waits (below).
- `bench_full_test` under ASan (CI run 37853130640): not a leak; asan_mem
  3072 was a guess, 4352 is measured (its own commit).

## 1. Method, and the conditions (read before the numbers)

- Lane checkout `~/Sources/dircachefs-lanes/lane-3`, `--jobs=2
  --local_test_jobs=1 --cache_test_results=no`, one test at a time.
- **The host was never quiet.** Five other lanes ran Bazel, TLC and guests at
  the same time: load average 10 to 17 for the baselines (fast 17 to 13,
  medium 11 to 8 with peaks of 21, large 15 to 10), 25 to 35 at times during
  the final large run, 1.4 at the start of one presubmit run. CPU-bound
  work (the unit tests' gtests, TLC, fsstress) runs 3 to 10 times slower
  than on the CI runner: `dir_cache_fs_test` took 129 s here and 7.4 s on
  the runner; the formal trace tests 14 s here at load 15 and 4 s at load 11.
  Time spent asleep (a `sleep 1`, an idle window, nfsd's grace period) does
  not scale with load. So **wall times from different runs are not
  comparable**; the comparisons below use (a) the guest's own markers for the
  check that held a sleep, which are exact, (b) back-to-back runs of both
  trees, (c) the CI runner's numbers beside ours.
- Guest time is split with markers that exist only in scratch copies (never
  committed): `guest/init` prints `TS <uptime>` when it starts (boot is the
  kernel's uptime at that point), when the script starts and ends;
  `lib.sh`'s `pass`/`fail`/`skip` print one before each `TEST` line; and
  `run-qemu.sh` its own start before the mkfs of the disks. From them:
  host setup (mkfs of the scratch disks), boot, init to script (the modules
  and the sampler), the script, and the gaps between checks. The first
  baseline run of each tier ran with the markers, so three host
  self-checks of `lib.sh`'s output (`require_commands_test`,
  `pjdfstest_suite_sane_test`, `strace_lib_test`) failed in the fast
  baseline only; the clean runs pass them.
- CI numbers: run 37841442160's job logs (`gh api .../jobs/<id>/logs`), the
  `PASSED in` lines of `fast`, `presubmit` and `full (0..2)` (the testlogs
  artifacts were uploaded only for failures; `(cached)` lines carry the time
  of the run that produced the result).
- Tiers measured: fast = `--config=fast //...` (184 tests); medium = the
  `--test_size_filters=medium` tests (104; presubmit is fast plus these);
  large = `--test_size_filters=large //...` (31, formal included; the
  enormous tier is not in it: pjdfstest, bench_full, cancel_inventory,
  stress_random).

## 2. Baseline

| Tier | Tests | Sum of test wall here | Wall of the run | Load | CI runner (sum of the targets' times) |
|---|---|---|---|---|---|
| fast | 184 | 1,701 s (guests 890 s over 63 tests, host-only 810 s over 122) | 1,699 s | 17 to 13 | 658 s over 201 lines |
| medium only | 104 | 4,681 s (guests 2,872 s over 67, host-only 1,809 s over 37) | 4,687 s | 11 to 8 | presubmit (fast + medium) 2,686 s |
| large | 31 | 8,813 s (guests 4,904 s over 25, host-only 3,909 s over 7) | 2 h 24 min | 15 to 10 | `full`'s three shards (14 targets each, large and enormous): 2,074, 1,451 and 1,849 s |

### Fixed cost of a guest (before any check runs, per qemu test)

| | tests | host setup (mkfs) | boot (kernel to init) | init to script (modules, sampler) | QEMU outside the guest's clock (start, reboot) |
|---|---|---|---|---|---|
| fast | 63 | 0.26 s | 1.17 s | 0.98 s | 2.06 s |
| medium | 67 | 0.33 s | 1.19 s | 2.24 s (xfs and btrfs load more modules) | 1.91 s |
| large | 25 | 0.42 s | 1.17 s | 2.14 s | 1.70 s |

Per guest that is 4.5 to 6 s under load, 282 s of the fast tier's 890 s of
guest time (32 per cent) and 380 s of the medium tier's 2,872 s. Quiet-host
numbers (manual boots, 10 to 12 each, min / median, host load 11 to 17): a
unit guest reaches init in 0.17 / 0.35 s, an e2e guest (14 MB initramfs, 34 MB
unpacked) in 0.64 / 1.2 s; the six modules of an ext4 test load in 0.33 to 1.35 s.

### The ten biggest sinks per tier, and what dominates each

Fast tier (wall here / on the CI runner):

| Test | here | CI | What dominates |
|---|---|---|---|
| `dir_cache_fs_test` | 129 s | 7.4 s | 114 gtests in one guest vCPU (SQLite, death tests that fork): CPU, load-bound |
| `fault_shutdown_test_ext4` | 54 | 27 | 99 checks, 13 daemon starts, device-mapper switches and fsyncs; 3 s in `ro-forced-read-only` |
| `bench_smoke_test_ext4` | 43 | 6.3 | `dcfs_bench`, one iteration of each benchmark on small trees (34.7 s in the benchmark process: CPU) |
| `fault_power_test_ext4` | 40 | 18 | 47 checks, restarts after cuts; 4.3 s in `sync-held` |
| `write_test_ext4` | 36 | 15 | 77 checks; two mmap checks of 3.3 and 3.6 s (testutil's fixed store delay), `write-large-tool` 3.9 s (150 MiB `dd`) |
| `rename_test_ext4` | 33 | 18 | `phase3-failure-renamed-src` 9.3 s (an 8 s database lock held on purpose) |
| `dir_cache_fs_fault_sites_test` | 32 | 3.3 | the fault sweep: CPU, load-bound |
| `syscall_traces_test` | 27 | 13 | 26 operations each with strace attached and two `quiesce_daemon` waits (0.3 s each at least) |
| `formal:known_bug_reval_write_fd_last_writer_test` | 25 | 9.4 | one TLC run (about 4.4 s of CPU: parsing the specs and starting) |
| `formal:known_bug_reval_no_recheck_test` | 23 | 5.0 | the same |

Of the host-only tests, 42 `formal:trace_*`, 20 `known_bug_*` and 8
`limitation_*` tests (690 s here) each start one TLC JVM; see section 4.

Medium tier (the tests that presubmit adds to the fast tier):

| Test | here | CI | What dominates |
|---|---|---|---|
| `formal:small_test` | 508 s | 184 | TLC model checking (formal lane) |
| `dir_cache_fs_trace_{a,c,b}_test` | 405, 405, 390 | 64, 63, 64 | 48 TLC runs per shard (one JVM per trace, section 4); the guest is 20 to 60 s |
| `formal:known_bug_rename_stale_source_test` | 221 | 111 | TLC |
| `formal:interrupt_test` | 145 | 71 | TLC |
| `fault_power_kill_test_ext4` | 132 | 34 | a real QEMU kill and a second boot per scenario |
| `memory_test_ext4` | 96 | 26 | 100,000 entries: 55 s in `restart-unmount` (the clean shutdown), 18 s in `mount` and in `find-grows-rss`: dcfs work |
| `readdir_boundary_test` | 94 | 16 | 82.5 s in the `mkfiles` calls (6,000 and 1,500 creates through dcfs) |
| `formal:reval_oob_test` | 85 | 29 | TLC |
| `idle_short_test_ext4` | 84 | 73 | 66 s of idle window by design |
| `formal:ident_stubs_test` | 76 | 32 | TLC |

Large tier:

| Test | here | CI | What dominates |
|---|---|---|---|
| `formal:large_test` | 1,602 s | 626 | TLC (formal lane) |
| `formal:nolock_test` | 1,438 | 464 | TLC |
| `idle_long_test` | 706 | 623 | 685 s of idle window by design |
| `fault_ace_a_test` | 590 | 410 | 182 sequences at 3.3 s: two daemon starts of a second each, the cut, the remount |
| `stress_long_test_{xfs,ext4,btrfs}` | 518, 516, 320 | 103, 61, 70 | fsstress 71 to 137 s, fsx 89 to 126 s, two tree comparisons of 40 to 73 s: CPU and I/O |
| `fault_ace_b_test` | 299 | 194 | 90 sequences |
| `destroy_test` | 271 | 43 | 253 s in `mktree`: 20,000 creates through dcfs |
| `nfs_test` | 221 | 218 | 2 x 103 s of nfsd's NFSv4 grace period |
| `fault_ace_fs_test_{xfs,btrfs}` | 205, 191 | 140, 152 | ACE sequences |
| `formal:interrupt_muts2_test` | 197 | 91 | TLC |

## 3. Changes

Each is one commit (`6.5:` or `6.2:`), each with its numbers in the commit
message. The times of single checks come from the guest's markers (exact);
the test walls are one test at a time on the shared host.

| Commit | Change | Before | After |
|---|---|---|---|
| `6.2: bench_full_test: ASan guest memory ...` | `asan_mem` 3072 to 4352 | died at 181 s, out of memory (CI) | passes at 4096 and 8192, `reclaim_scans=0` |
| `start_daemon: look ... every 0.1 s`, then `start_daemon waits for the mount as an event` | 100 looks at 0.1 s instead of 10 at 1 s; then `testutil waitmount` (poll(2) on /proc/self/mounts and the daemon's pidfd: no interval, no timeout; russ's rule of 2026-10-09) | `mount` check 1.05 to 1.4 s | 0.12 to 0.44 s; `cache_permissions_test` 18.2 to 7.2 s, `passthrough_test_ext4` 15.6 to 11.0, `power_test_ext4` 15.2 to 12.0, `crash_test_ext4` 11.1 to 9.0, `write_test_ext4` 32.1 to 26.8, `fault_ace_b_test` 280 to 205 (two other runs: 357 and 311, load). Event version on a quiet host (load 5), against the 0.1 s poll: `mount` check 0.11 to 0.02 s, `fault_ace_b_test` 45.8 to 24.8 s, `cache_permissions_test` 3.0 to 1.7 s |
| `wait for a holder's READY/MAPPED/STORED`, then `waitline` | `wait_for_line` in `lib.sh`; then `testutil waitline FILE TEXT PID` (inotify on the file's directory and the holder's pidfd); copy, crash, idle, release_leak, rename, write, destroy | `writehold-ready` 1.03 s | 0.13 s with the poll, 0.05 s with the event |
| `release_leak.sh: let go of the database lock` | wait for the Release's warning, then kill the lock | `release-refresh-failed-as-forced` 32.0 s, test 36.9 s | 17.4 s, test 22.1 s |
| `nfs.sh: nfsd grace 10 s, lease 30 s` | `rpc.nfsd --grace-time 10 --lease-time 30` | the two checks 103.4 s and 103.3 s; test 221 s | 13.2 s and 13.5 s; test 47.6 s |
| `write.sh: signal the mmap holder` | `mmapwrite ... usr1` waits for SIGUSR1 instead of 3 s / 2 s | 3.15 s and 2.59 s | 0.31 s and 0.73 s; test 25.9 to 21.1 s |
| `pjdfstest: chmod/ in the rename shard` | balance by CI times | chown 185 / 211 / 188 s, rename 61 / 60 / 60 | about 150 / 170 / 150 and 95 (not measured: the host's I/O load) |

## Tiers, after

Clean tree (all commits), same flags:

- `--config=fast //...`: 202 of 204 pass, 2 skipped; 791 s of wall (load 11),
  against 1,699 s before (load 17 to 13). **Not a like-for-like pair**: the
  host-only tests, whose code did not change, also took 358 s of the 810 s
  they took in the baseline.
- `--config=presubmit //test/qemu/...`: 124 of 124 pass, 1,392 s (load 1.4 to
  7).
- `--test_size_filters=large,enormous //test/qemu/...` (the guests of the
  large tier, pjdfstest, bench_full, cancel_inventory; the four large `formal`
  tests do not use anything changed and were not rerun): 40 of 43 pass; the
  three failures are `stress_long_test` (alias), `_ext4` and `_btrfs`, killed
  by the guest's own 840 s timeout at load 25 to 35 (the guest ran fsx for
  195 s where it ran 89 s in the baseline). Rerun alone at lower load:
  `_ext4` passes in 668 s (load 16 to 27), `_btrfs` in 160 s (load 6 to 10;
  the baseline's 320 s was at load 15). Load, not a regression: the test's
  scripts did not change, and with the 1-second polling gone it can only
  have done less waiting.

After the rebase onto `dbcda27` (which brought 15.2's `mount.dcfs` start-up
and the 15.6 tests; the fast tier grew from 204 to 232 targets), on the final
tree: `--config=fast //...` 230 of 232 pass, 2 skipped (13 min, load 11);
`--config=presubmit //test/qemu/...` 126 of 126 pass; and the large tests
this step changed or that follow its changes (nfs_test, fault_ace_b_test, the
pjdfstest chown and rename shards on ext4, xfs and btrfs, pjdfstest rest on
ext4) 9 of 9 pass, at load 5 to 11.

**Back-to-back pair** (the fast tier, the `origin/main` tree and then this
branch's, the same session, load 23 to 34 and 34 to 14, so this is a
bound, not a measurement): 2,516 s against 2,144 s; guests 1,343 s against
1,174 s over 60 tests (e2e 852 s against 722 s over 30, unit tests 491 s
against 452 s); host-only 1,146 s against 943 s (the same code: a drift of
18 per cent from the load alone). The tier-level effect of this step is
smaller than that drift. One run of the first tree had a 60 s `formal`
TLC test time out at load 34 (`limitation_out_of_band_stale_test`). A second
pair was cancelled: at that load it would only add noise.

**What the changes are worth, from the guest's own markers** (exact, not
load-bound): at least 60 `start_daemon` waits of about 0.9 s in the fast
tier's guests (24 tests, 69 s of 890 s of guest time) and 115 in the medium
tier's (51 tests, 123 s of 2,872 s), plus two per ACE sequence; the
holders' waits and the two mmap delays are a few seconds in `write_test`
(three variants) and `release_leak`; `nfs_test` 174 s of 221 s and
`release_leak_test` 15 s. In all, about 5 per cent of the fast tier's guest
time on a loaded host, 4 per cent of the medium tier's, 10 to 20 per cent
of the ACE tests and 75 per cent of `nfs_test`.

**Large guests, before and after** (the large tier's baseline run at load 15
to 10; the after run at load 25 to 35 for most of it, so a faster "after" is
a lower bound of the change):

| Test | before | after |
|---|---|---|
| `idle_long_test` | 706 s | 694 s |
| `fault_ace_a_test` | 590 s | 540 s |
| `stress_long_test_xfs` | 518 s | 503 s |
| `stress_long_test_ext4` | 516 s | 841 s (timed out) |
| `stress_long_test_btrfs` | 320 s | 841 s (timed out) |
| `fault_ace_b_test` | 299 s | 231 s |
| `destroy_test` | 271 s | 392 s |
| `nfs_test` | 221 s | 45 s |
| `fault_ace_fs_test_xfs` | 205 s | 164 s |
| `fault_ace_fs_test_btrfs` | 191 s | 97 s |
| `fault_power_kill_test_xfs` | 191 s | 334 s |
| `bench_readdir_test` | 126 s | 121 s |
| `memory_test_btrfs` | 108 s | 74 s |
| `fault_power_kill_test_btrfs` | 106 s | 307 s |
| `memory_test_xfs` | 98 s | 179 s |
| `idle_short_test_xfs` | 91 s | 121 s |
| `idle_short_test_btrfs` | 90 s | 88 s |
| `fault_recover_btrfs_unpinned_test` | 76 s | 89 s |
| `fault_recover_test_xfs` | 75 s | 133 s |
| `fault_recover_test_btrfs` | 68 s | 99 s |
| `trace_rename_test` | 60 s | 305 s |
| `cancel_test` | 59 s | 59 s |
| `fault_freeze_test_xfs` | 57 s | 124 s |
| `fault_freeze_test_btrfs` | 49 s | 77 s |
| `trace_power_test` | 45 s | 111 s |
| `trace_crash_test` | 34 s | 39 s |
| `trace_create_test` | 29 s | 54 s |

(`trace_{rename,power,create,crash}_test`: the baseline run built their
traced initramfs after the first of this step's guest-script edits had been
made in the lane, so their "before" already has the faster `start_daemon`.)

Not in the large baseline (enormous, first measured in the after run, load 25
to 35): `bench_full_test` 1,439 s, `pjdfstest_rename` 797 s (ext4), 551 s (xfs)
and 768 s (btrfs), `pjdfstest_rest` 448 to 748 s, `pjdfstest_chown` 278 to 314 s,
`cancel_inventory_test` 185 s. All pass with the shard move.


## 4. Measured and not changed

- **Per-trace TLC startup.** One TLC run of a hand-written trace costs 3.9 to
  4.9 s of user CPU plus 0.5 s of system time here (9 to 14 s of wall at load
  15): parsing `Trace.tla`, `dcfs.tla` and the community modules, and
  initialising TLC. Neither the garbage collector (ParallelGC against
  SerialGC: 3.9 against 4.1 s of user CPU) nor an AppCDS archive of the JVM's
  classes (5.0 against 5.0 to 5.6 s) changes it, so it is not JVM class
  loading. A batch of
  traces per JVM would need `Trace.tla` to take several traces (the trace
  comes in through the `DCFS_TRACE` variable), which is the formal lane's;
  `trace_validate.sh` runs the traces one after the other. On the runner a
  shard of 48 traces takes 64 s, so batching would save about 40 s per
  shard there; running two JVMs at a time would not help a runner that is
  running two guests at a time already. Left. The 70 host-only formal tests of
  the fast tier are the same cost once each (690 s here, 3 to 5 s each on the
  runner).
- **TLC configurations** (reported, not changed): `formal:large_test` and
  `nolock_test` are the two longest tests of the suite on the runner (626 s,
  464 s, 7.2 and 8.2 million distinct states at about 80 to 90 us per state,
  per `formal/BUILD.bazel`); the model checks run with `-workers auto`
  (`third_party/tlaplus/tlc_test_runner.sh.tpl`), so the runner's four vCPUs
  are used (6.4c); the trace checks run `-workers 1` per JVM, one JVM at a
  time. `MC_large.cfg`, `MC_nolock.cfg` and the other model configurations
  already declare `VIEW View`; only `MC_reval.cfg` declares a `SYMMETRY`
  (`HandleSymmetry`). A symmetry set over the interchangeable names, inodes
  or handles of `MC_large`/`MC_nolock` would cut the state count (the work
  is proportional to it); whether those sets are symmetric under the
  specification's operations is the formal lane's to decide and validate.
- **Initramfs format and size.** The e2e initramfs is 14 MB gzipped (`gzip
  -1`), 34 MB unpacked: `dcfs` 10.6 MB and `testutil` 2.3 MB (both with
  symbols), `dcfs_bench` 6.0 MB, `strace`, `fsstress`, `fsx`, `pjdfstest`
  and its tests, and the shared libraries. Kernel time to init, min / median
  over 12 interleaved boots (host load 11 to 17): gzip -1 0.74 / 1.21 s;
  zstd -3 0.46 / 0.86; zstd -19 0.52 / 1.02 (7.5 MB); the same cpio
  uncompressed 0.32 / 0.59; zstd -19 of the image without `dcfs_bench`,
  `fsstress`, `fsx`, `pjdfstest` and `strace` 0.43 / 0.83. (xz and lz4
  archives did not boot: the kernel wants `--check=crc32` and `-l`.) The
  decompression and the page-faulting of the unpacked image are 0.3 to 0.6 s
  of every e2e boot (about 65 s of the 6,400 s of the fast and medium tiers
  here). Taking it needs a
  pinned `zstd` (an Alpine package pin: MODULE.bazel, README, SBOM entry) or
  an uncompressed cpio (400 MB more of cache for the 55 images); neither is a
  five-minute change and the gain is about 1 per cent of a tier. Stripping
  `dcfs` takes 10.6 MB to 7.0 MB but loses the symbols a crash's backtrace
  prints. Left, as a follow-up.
- **Guest memory** does not slow boot: unit guests at 192 MiB and one vCPU
  and at 256 MiB and two reach init in the same 0.17 to 0.18 s (min of 8);
  the `-smp` count (lane 2's) and the `-m` sizes are not where the time is.
- **The memory sampler** (`guest/init`, one `awk` that forks a `cat` of every
  `/proc/*/status` and a `sleep` every 0.1 s) costs two process creations
  every 0.1 s in every guest. Not measured separately and not touched
  (lane 2 edits `guest/init`; the peaks the allowance table rests on come
  from that resolution).
- **ACE sequences**: with the 1 s polling gone a sequence takes 1.9 s at load
  15 (3.3 s before): the remount (`fd_restore`: two umounts, two device-mapper
  switches, two mounts) 0.54 s, the two snapshots and their diff 0.29 s, the
  operation and the cold daemon's first listing 0.28 + 0.23 s, the cut 0.17 s,
  the restart 0.13 s. Each is the work the sequence is about; nothing in
  the per-sequence setup can be shared without sharing a filesystem
  between sequences, which the cut and the durable check forbid.
- **Shell loops that create thousands of files**: there are none left. The
  guests already use `testutil` (`mkfiles`, `names-random`, `opath-hold-tree`,
  `mktree`); the 100,000-entry and 20,000-entry loops spend their time in
  dcfs's creates, not in forks.
- **pjdfstest shards on this host** showed rename at three times chown (381 s
  against 132 s per run, and 302 s for the same tests directly on ext4, no
  dcfs): on the runner rename takes 61 s in all against chown's 185 s. The
  difference is the host (a shared disk and load 20 to 35: not investigated);
  the shards were balanced by the runner's times (the later CI run 37853130640
  agrees: chown 187 / 211 / 184 s, rename 62 / 112 / 54 s, rest 103 / 105 /
  151 s on ext4 / xfs / btrfs). After the move, on this host at load 5 to 11:
  chown 208 s (ext4) and 172 s (xfs), rename with chmod 270 / 113 / 282 s
  (ext4 / xfs / btrfs), rest 197 s (ext4): here the rename shard is the
  longest, which the runner's times say it is not. If the runner's next run
  shows it is, move `chmod/` back (one line in `shard_of()`).

## 5. What was left, and why

| Item | Why left |
|---|---|
| dcfs's create cost (`readdir_boundary` 82 s, `destroy_test` 253 s, `names_random`, `memory_test`'s 100,000 entries, ACE operations) | 2 durable fsyncs per create, a protocol cost: 23.10 (and 26.4b's slope test decides); measured, not pre-empted |
| TLC startup, `formal:*` | formal lane (above) |
| `idle_short` (60 s) and `idle_long` (600 s) windows | the quiet window is what they measure |
| sync-interval waits (`enospc_cache` 3 s, `fault_cache` 3 s, `fault_freeze` 3 s and 2 s, `power` 2 s, `memory` 2 s x 3, `cancel_inventory` 2 s) | they let a 1 to 2 s `--sync_interval_sec` elapse: time is the event; shortening the interval changes what the test runs |
| `copy.sh`/`setattr.sh`/`atime.sh` `sleep 1` | the next operation must land in a later second than a cached ctime or mtime (atime.sh is lane 4's) |
| `fault_selftest` / `fault_recover` windows (`sleep_until` the flakey window's end, 20 s) | the window is the thing under test |
| `rename.sh` phase-3 8 s lock | tried: the mv returns only after the lock (9.24 s to 9.22 s) |
| `quiesce_daemon` (0.3 s of silence, twice per strace op and per `drop_caches_quiesced`) | the criterion is the check's; a shorter silence would weaken it |
| `finish`'s `sleep 0.2` and `mem_report`'s wait for a sample | drains the serial console (a lost verdict was observed); lane 2's file |
| `idle_long`, `stress_long` under load: the 840 s guest timeout | `stress_long_test_{ext4,btrfs}` timed out at 840 s in the final large run at load 25 to 35 (baseline 516 s and 320 s at load 15); rerun at load 6 to 18, ext4 passed in 668 s and btrfs in 160 s. Load, not a regression |
| initramfs compression and trimming | needs a pinned zstd or 400 MB more cache; follow-up |
| xfs/btrfs-only medium tests that are just the ext4 test again | one fresh guest per test; the matrix is the coverage |

## 6. `bench_full_test` under ASan (CI run 37853130640, asan shard 1)

The guest died of the OOM killer at `-m 3072` after 181 s. Reproduced with
`--config=asan --test_env=DCFS_MEM=8192` (passes, 1,720 s) and 4096 (passes,
1,114 s): `peak_used` 2,592 and 2,697 MiB, `min_avail` 1,229 MiB at 4096,
`reclaim_scans=0`. A sampler in a scratch copy of `bench.sh` (VmHWM and VmRSS
of every `dcfs` and `dcfs_bench` every 10 s) shows what fills it: the four
fixtures' daemons (`dcfs`, `dcfs0`, `slow_dcfs`, `slow_dcfs0`) live the
whole run, and the Startup, Recovery and Memory benchmarks start one more,
each 410 to 625 MiB of VmHWM under ASan against 10 to 21 MiB plain (the same
process structure in the plain run; about 28 times, the README's "40 or more"
for ASan), plus `dcfs_bench` at 170 MiB. A daemon's VmHWM: 409 MiB at 168 s,
472 MiB at 332 s, 625 MiB at 664 s, 625 MiB at the end (1,111 s): a plateau,
not growth. The plain run's daemons: 14.5, 14.7 and 15.8 MiB at 108 s, 213 s
and 1,157 s. Not a dcfs finding. `asan_mem` = (2,697 + 170) x 1.5 up to 64 MiB
= 4,352 (README's rule).
