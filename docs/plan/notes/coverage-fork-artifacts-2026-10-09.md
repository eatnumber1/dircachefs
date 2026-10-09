# Branch counts of 2^32 and more in the daemonisation code (step 26.14f, 2026-10-09)

Cause: `ForkDaemon` returned in both processes (the wrapper and the daemon).
Under continuous-mode profiling the two processes share the profile's
counters, a function entered once and left twice breaks the flow conservation
llvm-cov derives region counts from, and a branch after the fork gets a
negative count. Not a lost update, not the capture helper, not a truncated
profile. Fixed by `dcfs/fork_split.h` (`ForkSplit`: only one side returns) and
a guard in `scripts/cov-lcov.sh`.

## The numbers of CI run 37973594236

`main.cc:674 branch 0.1 4294967562`, `:685 4294967430`, `:686 4294967421`,
`:690 4294967546`, `startup_channel.cc:110 8589934575`, `:111 8589934574`.
These are not one test's counts: Bazel's lcov merger sums the tests' lcov
files. Each test that runs the wrapper (the guests of `mount_dcfs_test`, the
systemd test's, the daemon-starting tests) has its own 2^32-n (a negative
count, n = the number of forks, printed unsigned) and the real counts of the
others add up: 4294967562 = 4294967275 + 287, and 8589934575 = 2 x 4294967275
+ ... (two tests with the artifact on the line). That is why the values looked
like "2^32 + a few hundred".

## Reproduction (run 1)

`bazel coverage --config=presubmit //test/qemu:mount_dcfs_test
--nocache_test_results --test_env=DCFS_KEEP_PROFRAW=1`, one test, 22 calls of
`ForkDaemon` (`FNDA:22`). The test's `coverage.dat`, branches of
2^31 and above:

    dcfs/main.cc:674 0.1 4294967275        (-21)
    dcfs/main.cc:685 0.1 4294967281        (-15)
    dcfs/main.cc:686 0.1 4294967275        (-21)
    dcfs/main.cc:690 0.1 4294967254        (-42)
    dcfs/startup_channel.cc:110 0.1 4294967280    (-16)
    dcfs/startup_channel.cc:111 0.1 4294967280    (-16)
    dcfs/startup_channel.cc:120 0.1 4294967264    (-32)

The two raw profiles the guest wrote (harness profile name `%m%c`, continuous
mode): `15799749488584968325_0.profraw` (384200 bytes) and
`2908397626586896120_0.profraw` (2339912 bytes; the dcfs binary, the same
name as in 26.14c's note: the wrapper and all daemons write the one file,
`%m` is one file per binary and a daemon is a fork of the wrapper). Both read cleanly, so a truncated or
half-merged profile is excluded. In the merged profile
(`llvm-profdata show --all-functions --counts`):

    _ZN4dcfs10ForkDaemonEv  Function count: 22
      Block counts: [0, 22, 22, 0, 0, 0, 44, 44, 0, 0, 22, 0, 0, 16, 0, 0, 6, 6, 0, 0]

entered 22 times, and the blocks after the fork 44 times: one pass per
process, on the same counters. llvm-cov's derived counts (`false = condition
- true`, with `condition` taken from the function's entry) then go negative.
The same shape shows in a 25-line program (`DA` of the parent-only line
2^64-1, `BRDA:15,0,1,4294967295` after one fork).

Which process wrote the bad counter: neither, and that is the point. The
counters are right (the code after the fork did run 44 times); what is wrong
is the model llvm-cov applies to them. `%p` in the profile name could not
separate the processes in continuous mode (the file is mapped at start, in
the wrapper; a fork child keeps the mapping, so it never opens a file of its
own), and the attempt to use `/cov/%p%c.profraw` filled the guest's initramfs
(`LLVM Profile Error: Failed to write file ... No space left on device`: 60
processes x 2.3 MB) so mount_dcfs_test failed. Run 2, discarded.

## The candidates

(a) parent and child both own the mapped profile and one re-initialises it:
the child does not re-run the runtime's initialisation after a fork (the
mapping is inherited and the file name was fixed at the wrapper's start);
the counts (22 entries, 44 passes after the fork) fit counters shared by the
two processes with nothing reset, not a reset or a second mapping (inferred;
a reset would lose counts, not add them). Excluded as a cause; the sharing
itself is the mechanism.
(b) the capture helper's exec of /bin/mount writing into our pattern:
excluded by the evidence: `/cov` holds two profiles and no third (an
instrumented foreign binary would have added or merged one), and the artifact
is on the fork's own lines. The tmpfs over /proc/sys/vm was not tested apart.
(c) a process that `_exit`s without the runtime's flush leaves the file
half-written: excluded. Continuous mode needs no flush; every file read
cleanly and the counts were wrong in a way a missing flush could not make
(a flush that never ran lowers counts, it does not make a difference negative).
(d) a SIGKILLed daemon writing a partial profile: excluded, same evidence
(every profile readable, errors come from a function entered once and left
twice, not from a partial one).
(e) `-runtime-counter-relocation` plus `%m` merging across processes with
different biases: excluded. Run 3 below keeps `%m` and relocation and
removes the artifact by dropping `%c` alone.

Experiment that pinned it (run 3, `LLVM_PROFILE_FILE=/cov/%m.profraw`, no
continuous mode, same test): no branch of 2^31 or more. Without `%c` each
process works on a private copy of the counters (the child's starts as the
parent's at the fork) and writes its own profile at exit, merged by `%m`:
each process's history is consistent, the merge sums consistent histories,
and the price is that everything before the fork is counted twice
(run 3: `ForkDaemon` Function count 43 with 43 passes after the fork, where
run 1 had 22 and 44; the number of forks differs between the runs). So the artifact comes from continuous mode + a function that
returns in both processes. Continuous mode cannot go (a daemon SIGTERMed while
it writes its profile at exit leaves a truncated one: `test/qemu/README.md`).

## The fix

`dcfs/fork_split.h`: `ForkSplit(child, parent)` forks, runs `child()` in the
child and returns its value there, runs `parent(pid)` in the parent, which
must not return (it exits). It is the one function that returns twice and
carries `__attribute__((no_profile_instrument_function))`, so it has no
counters to violate. `ForkDaemon` now returns the daemon's `StartupReporter`
in the child only, the parent waits for the report and calls `exit()` inside
`AwaitReportAndExit`; `main.cc` no longer has the `parent_exit_status`
branch. Every function with counters is entered as many times as it is left.
Behavior is unchanged: the wrapper exits with the same status after the same
report, through `exit()` (atexit handlers and the profile dump run) instead
of returning from `main`.

The other candidate fixes: re-exec of the daemon (a different design: the
daemon would parse the arguments again), `__llvm_profile_set_filename` after
the fork (continuous mode maps the file once; the runtime does not remap),
a count-only exclusion in the gate (not allowed).

## Before and after

`mount_dcfs_test` (`bazel coverage --config=presubmit`, same command):

| | branches of 2^31 and above in the test's lcov | `ForkDaemon` count / blocks after the fork |
|---|---|---|
| before (run 1, `%m%c`) | 7 (above) | 22 / 44 |
| `%m` without continuous mode (run 3) | 0 | 43 / 43 (pre-fork code counted twice) |
| after the fix (run 4, `%m%c`) | 0 | 22 / 22 (`FNDA:22,..ForkDaemon`, `DA:38-46,22`) |

After the fix main.cc 660-691 and startup_channel.cc 38-130 have counts that
match what ran (`FNDA:22` for `ForkDaemon`, both of its lambdas,
`AwaitReportAndExit` and `BecomeDaemon`; `Ready` 17 and `Fail` 6 as reported by
the daemons). `tools/coverage_gate.sh` on the fixed
run's `coverage.dat` passes the artifact check (it then fails on the baseline,
as it must for one test's report: 49.33% lines). The same coverage run of
`//dcfs:startup_channel_fault_test` (the four ForkDaemon failure tests, each
now running the wrapper in a process of its own) has no such count either.

## Guards

- `scripts/cov-lcov.sh` fails a test whose lcov has a branch or line count of
  2^31 or more, naming the lines (before: it wrote the report and the gate
  summed it into the combined one, where 4294967275 + 287 hid the 2^32).
- `//test/qemu:coverage_pipeline_test` (host, 0.6 s) runs
  `test/qemu/testdata/cov_fork_fixture.cc` in continuous mode two ways:
  `twice` (a function that returns in both processes) must be refused by
  `cov-lcov.sh` with the fixture's source line named (this failed first:
  "FAIL: a report with a counter-underflow artifact was accepted"), and
  `split` (through `ForkSplit`) must be accepted with `Parent()` and
  `Child()` each at exactly 1.
- `//dcfs:startup_channel_fault_test` runs the real exit path of the wrapper.

## Sizes and time (for 26.16)

`mount_dcfs_test` (22 forks): `2908397626586896120_0.profraw`
2339912 bytes before, 2340264 after (+352: the new functions' names and
counters), testutil's 384200 unchanged, `merged.profdata` 1391184 before and
1389928 after. `llvm-profdata merge` of the raw profiles 0.26-0.37 s before,
0.27-0.31 s after. No cost.

## Not fixed

`backing_capture.cc` lines 81 and 313 (`if (child == 0)` after
`syscalls::fork()`, whose child execs or exits) and `bench/process.cc`:
the `fork()` wrapper returns twice, the caller's parent-leg branch reads
0 (`BRDA:81,0,1,0`, `BRDA:313,0,1,0`) although it ran 15 times. An undercount
of two branches, no negative. The same primitive fixes it (the side that
returns is the parent here: `ForkSplit` would take a `child` that never
returns); a follow-up, not done here because it is outside the six counts.

## Commands

    bazel coverage --config=presubmit //test/qemu:mount_dcfs_test --nocache_test_results --test_env=DCFS_KEEP_PROFRAW=1   (runs 1, 3 with a temporary init edit, 4 with the fix)
    bazel test //test/qemu:coverage_pipeline_test --nocache_test_results --test_output=errors
    bazel test --config=presubmit //dcfs:startup_channel_test //dcfs:startup_channel_fault_test //test/qemu:mount_dcfs_test //test/qemu:coverage_pipeline_test //tools:coverage_gate_self_check_test --nocache_test_results
