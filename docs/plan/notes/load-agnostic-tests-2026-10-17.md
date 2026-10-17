# Load-agnostic tests: the audit (step 26.36, 2026-10-17)

russ, 2026-10-17: "in general for all our tests I'm interested in them being
load-agnostic." The rule is `docs/style.md` section 2 ("A test's verdict does
not depend on host load or speed"). This is the census of the places that
still break it, and what was done about each.

## Method

Every place in `test/qemu/` (guest scripts, `guest/init`, `run-qemu.sh`, the
budget files), `dcfs/` (the unit tests and `testonly/`), `bench/`, `tools/`
and `formal/` where a verdict, or a wait that decides one, depends on wall
time or host speed: `sleep`, `timeout`, `usleep`/`nanosleep`, time read from
`/proc/uptime` or `date` and compared, `SimulatedClock`, bounded poll loops
(`kill -0`, `grep` in a `while`), budgets and allowances. Found by `grep` over
those trees (patterns in the last section), then by reading each site; the
63 bare `sleep`s of `tools/repo_shape_sleeps.txt` were each read in context.
Counts are of call sites, not of tests.

Classes (the brief's three, and a fourth the audit needed):

- **(a) hang guard.** Time bounds a wait for something that cannot hang
  forever when the test is right: generous, and a firing guard says "hung".
  Acceptable. Where the guard is a fixed number rather than sized by
  `run-qemu.sh`'s allowance, that is noted.
- **(b) verdict on time.** A duration, a CPU time or a fixed window is
  compared with a bound, or a state is judged by whether it was reached in a
  number of seconds. Must become a count of events, or move behind (a).
- **(c) a sleep or a poll standing in for an event.** Violates
  `docs/style.md` 1.11.
- **(d) time is not the verdict.** The wait is a lower bound that load can
  only lengthen (a second that must pass so two timestamps differ, an idle
  window whose subject is "nothing happens", a process that exists to be
  killed, a sampler's cadence, a simulated clock). The verdict is the same on
  any host.

## Counts

The 63 bare sleeps of `tools/repo_shape_sleeps.txt` (now 50):

| class | sites | fixed here |
|---|---:|---:|
| (a) hang guard | 1 | 0 |
| (b) verdict on time | 5 | 0 |
| (c) stands in for an event | 46 | 13 |
| (d) time is not the verdict | 11 | 0 |

and, outside that list, the other places a verdict depends on time or speed:

| class | sites |
|---|---|
| (a) hang guards | 9 groups: `timeout N` in the guest scripts (`wrapper` 60 s, `lifecycle.sh` 10 s, `fault_recover.sh` 30 s), `run-qemu.sh`'s guest limit and kill-mode watchdog, the wall allowance `cost_wall_allowance` derives from the budgets (russ, 2026-10-17: "Keep it until we replace with non-load-sensitive tests."), `xfstests.sh`'s per-test `LIMIT` (200 s, 1500 s slow: a fixed number, not scaled by the allowance), `destroy.sh` `BOUND_SECS` (60 s), `cancel_inventory.sh`'s printed times (no verdict), Bazel `timeout` on every test (135 declarations), `mutate.py --timeout` (a mutant that times out is reported as one, not as killed) |
| (b) verdicts on time | 6: `cancel.sh` (4), `readdir_boundary.sh` (1), `fault_recover.sh`'s window check (1) |
| (b) a count that moves with load | 2: the I/O budgets of `write_test_ext4` and `fault_ace_b_test`, see below |
| (a) that fired on load | 1: `mount_dcfs_test` timed out in 1 of 4 runs at 90% host pressure, see below |
| (d) | 3 groups: `backing_test.cc`'s `WaitForNextTimestamp` (30 ms, a lower bound), `syscalls_test.cc`'s nanosleep wrapper test (a lower bound), every `SimulatedClock` test of `dir_cache_fs_test.cc` (no wall time at all) |

The `dcfs/` unit tests are otherwise clean: no `SleepFor`, no
`WaitForNotificationWithTimeout`, no elapsed-time assertion; `bench/` prints
times and decides nothing; `formal/` has none.

## (b) verdicts on time, with the fix proposed

1. `cancel.sh`: four verdicts. `timeout -s INT 1 ls` sends the signal at a
   fixed second, so a faster host finishes the listing first and
   `ls-interrupted` fails (a verdict that load cannot break but speed can);
   `within ls-sigint` and `within find-sigkill` compare the time from the
   signal to the exit with `BOUND_MS` (2 s KVM, 8 s TCG); `find finished within
   1 s` judges a state by whether it was reached in a second. *Proposed:* the
   signal goes at the event (the first backing request of the population is in
   flight: `testutil waitline` on the `--v=2` log), and the verdict counts the
   backing requests dcfs issues after the signal until it replies (a handful
   with checkpoints, thousands without: the 12.1 s population of the existing
   comment), by `strace_op` or the testonly request counters.
2. `readdir_boundary.sh` `warm-listing-is-not-quadratic`: the daemon's CPU
   ticks of a 4x listing against the small one's, at most 8x. *Proposed:* the
   same ratio of instructions retired (follow-up 1), or of backing `getdents`
   calls, which are counts.
3. `fault_recover.sh` `window` mode: the operations must finish before a
   dm-flakey down interval (20 s) ends, and the script sleeps until the window
   opens. *Proposed:* 11.3c as already planned: switch the dm table at events
   (before the operations, after them), not by the clock.
4. `fault_recover.sh` `sleep_until` and `fault_selftest.sh`
   `sleep_until_uptime`: the same window.
5. `fault_freeze.sh` `probe()` and `expect_answered`: a request is "answered"
   if its process ends within `PROBE_WAIT` tenths of a second, else "blocked".
   Blocked-by-a-freeze is structural, so that verdict holds on any host;
   "answered" fails on a host slow enough to take 5 s for a `stat`.
   *Proposed:* answered = the process ended before the thaw, waited on with
   `wait_exit` in a `timeout`-less race against the thaw: take the thaw as the
   event, i.e. thaw after N requests are known blocked, and judge by which
   processes ended.
6. `lib.sh` `quiesce_daemon` and `fault_dcfs_lib.sh` `fd_blocked`: "quiet" is
   0.3 s without a wakeup and "blocked" is three looks 0.2 s apart in D state.
   A loaded host can pause a busy daemon for 0.3 s. *Proposed:* count: wait
   until the daemon's request counters (testonly cost counters,
   `dcfs/testonly/cost_counter.h`) have served as many FORGETs as the kernel
   sent, and for "blocked" read the daemon's in-flight-request counter.
7. I/O budgets that are not load-stable, found while verifying the fixes.
   `write_test_ext4` at host pressure of 87 to 91% (four runs, and four more
   with the unchanged `write.sh`, so it is not the change): `writes` 329 to 336
   against a budget of 325 (observed in 20 quiet runs: 323 to 324),
   `sectors_written` 133136 to 133184 against 133096 (exactly 133096 in every
   quiet run), `flushes` 144 to 150 against 145. `fault_ace_b_test` at 57 to 62%
   (with a PMU counter beside it): `reads` 17930 to 17936 against 17898,
   `writes`, `sectors_read` and `sectors_written` 0.2 to 0.6% over. The
   likely mechanism (not measured) is guest time, not host work: the guest's clock runs while its
   vCPU waits for a host CPU, so a slow host gives ext4's periodic journal
   commit (`commit=5`) more guest seconds to fire in, and every commit is
   writes and a flush. 26.34's "counts no longer move with load" holds for the
   counts of requests dcfs causes (`dcfs_*` did not move) and not for the disks'
   own journal. *Proposed:* mount the test disks with a commit interval no test
   reaches (the quiet kernel's other half: no periodic journal commit, only
   sync, fsync and dcfs's sync points commit), then re-measure; or hold
   `writes`, `sectors_written` and `flushes` only on runs that are not
   load-starved, as the CPU line now is (russ decides). Follow-up 11.
8. `mount_dcfs_test` (a hang guard that fired): in 1 of 4 runs at 90% host
   pressure QEMU was killed by the guest limit after 240 s (the test is
   `moderate`: 300 s; it passes in 97 s on a quiet host, and the other three
   runs took 132 to 242 s). The guard is sized from Bazel's `timeout`, not from
   the load, so it says "hung" for a host that was only slow. *Proposed:* the
   guard follows the harness's load: `run-qemu.sh` has the pressure only after
   the run, so size it from the test's measured wall time (the plan's
   non-load-sensitive replacement of the allowance is the real fix; russ,
   2026-10-17: "Keep it until we replace with non-load-sensitive tests.").
   Also in that run set: `daemon-logs-to-syslog` failed once because
   `sleep 0.5 # the socket /dev/log appears` is shorter than a loaded host's
   syslogd start; fixed here (`testutil waitpath /dev/log`).

## (c) sleeps and polls standing in for events

Fixed here, each by an event that already existed or one new one
(`testutil waitexit` and `testutil waitpath`):

- `write.sh` (1): `wait_for_file` polled for a file; now `testutil waitline`
  on the writer's `ready` line, and the second wait was redundant with
  `wait "$WRITER_PID"`.
- `mount_dcfs.sh` (5): `wait_gone` (a process's exit) and `nothing_left` (no
  daemon left) are `wait_exit`; the lock holder's `READY` and the foreground
  mount's appearance are `testutil waitline` and `testutil waitmount present
  PID`; syslogd's socket is `testutil waitpath /dev/log` (new: `waitline`
  cannot open a socket). This one failed under load before the fix (below). `daemons()` no longer lists a zombie: `wait_exit` returns at the
  exit, and the old 5 s poll had hidden that the parent reaps the process a
  moment later (the first run of the change failed 7 checks on it).
- `fault_recover.sh` (2): the writer's and the pin holder's `READY` lines,
  `testutil waitline` with the holder's pid.
- `fault_shutdown.sh` (2): `stop` and `wait_bg` waited on children with `kill
  -0` loops and then called `wait`; `wait` is the event.
- `fault_freeze.sh` (3): `thaw_and_wait`, and two waits for the daemon's exit
  after SIGTERM, are `wait_exit`/`wait`. What were checks that the process
  ended within 5 or 10 s are now behind the test's own timeout (class (a)): a
  daemon that stays held hangs the guest and Bazel's timeout says so. No check
  that could pass or fail on its merits was dropped; `TW_ALIVE`, which could no
  longer be set, is gone.

One mechanism, not two: `wait_exit` had a python pidfd version in
`mount_dcfs_systemd.sh` only; it is now `lib.sh`'s, on `testutil waitexit`,
and the python one is removed.

Left (33, which is the 46 less the 13 fixed), with the event that would replace each (follow-ups 2 and 6 to 9):

| sites | what the sleep waits for | event |
|---|---|---|
| 8: `enospc_cache.sh`, `fault_cache.sh`, `fault_freeze.sh`, `fault_power.sh`, `fault_shutdown.sh` (2), `idle.sh`, `power.sh` | the sync interval to pass, so the next request runs the sync point | a sync point on demand: `fsync` or `fsyncdir` through the mount already run `SyncBackingNow`; the periodic trigger itself is tested with a `SimulatedClock` in `dir_cache_fs_test.cc`. Not a drop-in where the test is about the request that runs the point while a filesystem is frozen. Also a latent race: the premise "the mutation was made within the interval" (2 s) is an upper bound on elapsed time |
| 14: `cancel.sh`, `cancel_inventory.sh`, `fault_backing.sh`, `fault_power.sh` (2), `fault_recover.sh`, `fault_selftest.sh`, `fault_shutdown.sh`, `idle.sh`, `memory.sh` (3), `release_leak.sh`, `rename.sh` | FORGETs or RELEASEs served, a backgrounded process to reach its backing syscall, a journal to abort | a counter of served requests, or `fd_blocked`'s event (follow-up 6) |
| 3: `nfs.sh` | `rpc.idmapd` and `rpc.mountd` to listen, and a `mount -t nfs4` retried every second for 10 s (a 1.12 retry loop) | the listening sockets; one mount after them |
| 6: `guest/init` | coverage runs only: processes to finish writing profiles, `/cov` to stop changing, the console to drain | pidfds of the instrumented processes; a drain has no event |
| 2: `lifecycle.sh`, `strace_lib.sh` | a mount to appear over an existing mount; the tracer to attach | `waitmount` cannot see a second mount at a path; ptrace attach has no readiness line |

## (d) time is the subject or load cannot change the verdict

`atime.sh`, `copy.sh`, `setattr.sh` (2) sleep a second so two timestamps
differ; `quiet_kernel.sh` (10 s: nothing happens by itself) and `idle.sh`'s
idle window; `boot.sh` (2) and `fault_recover.sh`'s `sleep 100000` and
`fault_freeze.sh`'s `sleep 0.2` are processes that exist to be killed or to hold
a file; `fault_freeze.sh`'s `sleep 2` (a SIGTERM that must not take effect
while a mutation is held; load cannot make it take effect sooner); the
sampler's 0.1 s cadence in `init`; `mount_dcfs.sh`'s "waited at least 1000 ms"
(a lower bound). `justified_sleep` in `lib.sh` is the sanctioned form.

## Follow-ups proposed

1. **Instructions instead of CPU time** for the budgets of `cpu_budgets.txt`
   (README, "Instructions instead of CPU time": the measurements and the design).
2. **Sync points on demand** for the eight sync-interval sleeps.
3. **`cancel.sh` by counts**, as proposed above.
4. **`readdir_boundary.sh` by instructions or `getdents` counts.**
5. **The fault windows by events** (11.3c).
6. **`probe`, `expect_answered`, `fd_blocked`, `quiesce_daemon` by counters.**
7. **The settles after drop_caches, RELEASE and backgrounded processes** (the
   fourteen of the second table), by the same counters.
8. **`nfs.sh`**: readiness of the rpc daemons, and no mount retry.
9. **`init`'s coverage-run waits** by pidfds.
10. **`xfstests.sh`'s per-test `LIMIT`** sized from the harness's allowance, not
    a fixed 200 s: a hang guard, but one that a 3 to 5 times slower host can
    trip on a test that takes 60 s.
11. **I/O budgets that move with load** (item 7 of the (b) list): journal commits
    by guest time. Mount the test disks with a long commit interval, or hold the
    journal-dependent counts only on runs that are not load-starved; then
    re-measure per russ's rule (all of a target's lines together, one run count
    in the header).
12. **`xfstests_1_test_ext4`'s budget lines** (26.34's open item) re-measured as
    a whole in the same step as 11.

## As built

- `testutil waitexit`, `testutil waitpath` and `testutil pmu` (new; the last is the probe), `lib.sh`
  `wait_exit`, `guest/pmu.sh` and `//test/qemu:pmu_test`, `.github/ci/pmu.sh`
  and `//tools:pmu_report_test`, a CI step "Report the PMU" in the `fast` job.
- `tools/repo_shape_sleeps.txt`: 63 to 50; the lines of the five files changed
  carry their new reasons.
- `docs/style.md` section 2: the load-agnostic rule, with russ's words, the
  hang-guard ruling and the re-measure ruling.

## How the sites were found

    grep -rn -E "\bsleep\b" test/qemu/guest/*.sh test/qemu/guest/init test/qemu/scripts bench tools
    grep -rn -E "\btimeout\b" test/qemu/guest
    grep -n -E "date \+%s|/proc/uptime|SECONDS|elapsed" test/qemu/guest/*.sh
    grep -rn -E "SleepFor|nanosleep|WaitForNotification|absl::Now|steady_clock" dcfs bench
    grep -rn -E "time\.sleep|timeout=" tools test formal bench --include='*.py'
