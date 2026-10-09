# Coverage

`AGENTS.md` says new code arrives fully covered and that the gaps that cannot
be covered are listed here with a reason. `test/qemu/README.md` ("Coverage")
describes how the guests' profiles become the report and `dcfs/coverage_baseline.txt`
holds the gated numbers. No uncoverable gap is listed yet.

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
- **Line and branch status that depends on scheduling** (FORGET vs
  BATCH_FORGET, the session loop's read-ahead and `-EINTR` paths, the daemon's
  shutdown path): not artifacts of the tooling but of the tests, listed in
  `notes/coverage-determinism-2026-10-08.md` (steps 26.14b and later).

Debugging knobs of the harness (`test/qemu/scripts/run-qemu.sh`, for use
with `bazel coverage --test_env=...`): `DCFS_KEEP_PROFRAW=1` keeps each test's
raw and merged profiles as undeclared outputs (`test.outputs/profraw/`, for
`llvm-profdata show --counts` and `llvm-cov export`); `DCFS_FORCE_CPUS=<n>`
overrides the guest's vCPU count.
