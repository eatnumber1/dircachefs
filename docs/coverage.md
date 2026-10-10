# Coverage

`AGENTS.md` says new code arrives fully covered and that the gaps that cannot
be covered are listed here with a reason. `test/qemu/README.md` ("Coverage")
describes how the guests' profiles become the report and `dcfs/coverage_baseline.txt`
holds the gated numbers. The gaps that cannot be covered are listed below,
each with its reason.

## Gaps that cannot be covered

Lines that no test reaches, and why none can (step 15.6b; measured as the
union of `//test/qemu:mount_dcfs_test` and the two unit tests of the helper
under `bazel coverage`).

- **`dcfs/umount_helper.cc`, `BlockingLock`'s `EINTR` handling** (the retry
  with `retry_signals`, and the error for a signal when it is off): it needs a
  signal to land while the process is blocked in `flock(2)`. A test can know
  the process has not yet reached the call, or that it has returned, but there
  is no event for "blocked in it": the only way to land the signal there is a
  delay, and the project does not wait on timers (`docs/style.md`, "No
  timers").
- **`HoldDaemonLock`, a `flock` failure other than `EWOULDBLOCK`, and an
  `fstatat` failure other than `ENOENT` on the lock's path** (a lock the
  kernel refuses, a path that changes under the daemon): the first needs
  `ENOLCK` or `EBADF` from a valid descriptor, the second a directory that is
  swapped for a file between the `openat` and the `fstatat`. Neither has a
  syscall fault fake in the tests (`--wrap` fakes exist for the backing
  syscalls, not for `flock`/`fstatat` of the helper's own files), and the swap
  has no event to key on. Both return the error with the path in it; nothing
  else runs after them.
- **`UmountAndWait`, a `waitpid` failure other than `EINTR`**: the child is
  ours and is never reaped by anything else, so `ECHILD` cannot happen; a
  failure would be an invalid argument of our own call.
- **A fork whose child never returns**: `UmountAndWait` forks `umount` and its
  child execs or exits, so the profile artifact of `docs/coverage.md`'s
  "returns twice" entry cannot occur; like `backing_capture.cc`'s forks, the
  parent-leg branch of `child == 0` may read 0 although it ran.

## Known coverage artifacts

Numbers in the report that are not counts of what ran.

- **A branch count of 4294967295 (2^32-1)** (found in step 26.14c:
  `DropLookups`, `dcfs/dir_cache_fs.cc` branch 1.2, in one run of
  `bench_smoke_test_btrfs`). llvm-cov derives a branch's count as a
  difference of region counters (`true = condition evaluated - false`); when
  the function's counters disagree the difference is negative and the lcov
  prints it as 4294967295 (the JSON export clamps it to INT64_MAX). The branch
  then counts as taken although it never was. The counters disagree because a
  plain (non-atomic) counter increment loses an update when two processes or
  threads run the same function at once, and every dcfs daemon of one test
  writes the same profile file (`%m`: one file per binary, continuous mode),
  so the daemons share their counters: `dcfs_bench` mounts four at once.
  Measured in `notes/coverage-branch-underflow-2026-10-09.md`: 17, 6 and 2
  functions with a negative branch count in three two-vCPU runs, 1 and 1 on
  one vCPU, none in five runs with atomic increments.
  Fix: `coverage --copt=-fprofile-update=atomic` in `.bazelrc`.
  Guard: `tools/coverage_gate.sh` fails on any branch count of 2^31 or more,
  naming the file, line and branch (rerun the job; the report is not
  trustworthy until the artifact is gone). A count that is merely too low
  (a lost update that does not make a difference negative) cannot be seen
  this way; the atomic increments are what prevent it.
- **A branch count of 2^32 or more, or of 2^32-1 summed with real counts**
  (found in step 26.14f: `dcfs/main.cc` 674, 685, 686 and 690 and
  `dcfs/startup_channel.cc` 110 and 111 in CI run 37973594236, counts such as
  4294967562 and 8589934575). The same artifact as the one above, one per test
  that ran the wrapper: each test's lcov holds 2^32-1 or 2^32-n, and Bazel's
  combined report adds the tests' counts (2^32-21 + 21 + 266...). The cause
  is a function that returns in two processes: `ForkDaemon` forked, and the
  parent and the child both returned through it and through `MountHelperMain`.
  In continuous mode (`%c`, what the guests need so that a daemon killed
  while it exits still leaves a complete profile) the two processes share the
  profile's counters. A function entered once and left twice breaks the flow
  conservation llvm-cov assumes when it derives a region's count from the
  others (`false = condition - true`): a branch after the fork gets a negative
  count, a line `2^64-1`. Measured in `notes/coverage-fork-artifacts-2026-10-09.md`:
  22 forks in `mount_dcfs_test` gave -21 to -27 on seven branches; the same
  test with `%m.profraw` (no continuous mode: the child works on a private
  copy of the counters) gave none; a 25-line program with this shape shows it
  with one fork. Not a lost update (atomic counters do not help), not the
  helper that execs mount(8) (it is not instrumented), not a truncated profile.
  Fix: no function returns in both processes. `dcfs/fork_split.h`'s
  `ForkSplit(child, parent)` runs the child's side in the child and returns
  its value there, and runs the parent's side, which exits, in the parent;
  `ForkDaemon` returns the daemon's reporter in the child only and the wrapper
  exits inside it. `ForkSplit` is not instrumented (it is the one function
  that returns twice). Guards: `scripts/cov-lcov.sh` fails a test whose lcov
  has a count of 2^31 or more and names the lines (so `bazel coverage` of the
  one test shows it, not only the combined report), and
  `//test/qemu:coverage_pipeline_test` runs a fixture that forks both ways
  (it must be refused when a function returns twice, and accepted, with every
  count 1, through `ForkSplit`). A new fork that returns in both processes
  needs the same shape. Not fixed: the forks of `backing_capture.cc` (lines
  81 and 313) and `bench/process.cc`, whose child never returns (it execs or
  exits) but whose `fork()` wrapper does: they cannot go negative, and their
  parent-leg branch (`child == 0` false) reads 0 although it ran (an undercount
  of two branches).
- **Line and branch status that depends on scheduling** (FORGET vs
  BATCH_FORGET, the session loop's read-ahead and `-EINTR` paths, the daemon's
  shutdown path): not artifacts of the tooling but of the tests, listed in
  `notes/coverage-determinism-2026-10-08.md` (steps 26.14b and later).

Debugging knobs of the harness (`test/qemu/scripts/run-qemu.sh`, for use
with `bazel coverage --test_env=...`): `DCFS_KEEP_PROFRAW=1` keeps each test's
raw and merged profiles as undeclared outputs (`test.outputs/profraw/`, for
`llvm-profdata show --counts` and `llvm-cov export`); `DCFS_FORCE_CPUS=<n>`
overrides the guest's vCPU count.
