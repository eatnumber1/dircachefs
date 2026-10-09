# The 4294967295 branch count (step 26.14c, 2026-10-09)

Cause: lost updates of non-atomic profile counters shared by the four dcfs
daemons that `dcfs_bench` mounts at once (one profile file per binary, one
set of counters for all of them). Fixed by `-fprofile-update=atomic` in the
coverage config; `tools/coverage_gate.sh` now fails on such a count.

## Mechanism

`bench_smoke_test_btrfs` (two vCPUs, `dcfs_bench` starting four dcfs
processes that run at the same time) wrote one profile for dcfs
(`2908397626586896120_0.profraw`, `%m%c`, continuous mode) that all four
processes increment in place. `llvm-profdata show --counts
--function=DropLookups` of the bad run:

    Function count: 12217        Block counts: [0, 12216, 12217, 0, ...]

A good run: `Function count: 12220  Block counts: [0, 12220, 12220, 0, ...]`.
`DropLookups` ran 12220 times (the clean runs agree on that); in the bad run
the entry counter lost 3 increments and the "right operand evaluated" counter
lost 4, so `it->second < n` read as `12216 - 12217 = -1` taken, which lcov
prints as 4294967295 (`BRDA:863,1,2,4294967295`, `BRDA:863,1,3,12217`; the
JSON export of the same branch is `[863,30,863,44,9223372036854775807,12217]`).
The same run had negative branch counts in 17 functions (the abseil hash-set
templates dcfs instantiates, sqlite3's `btreeParseCellPtr` and
`sqlite3VdbeExec`, and `DropLookups`; only `DropLookups` is in a file the
report keeps).

## Runs (`bazel coverage --config=presubmit --nocache_test_results`, `bench_smoke_test_btrfs`, host load average 12-20)

| run | change | `DropLookups` entry count (actual 12220) | functions with a negative branch count | 4294967295 in lcov |
|---|---|---|---|---|
| 1 | none (2 vCPU) | 12217 | 17 | yes, line 863 branch 1.2 |
| 2 | none | 12220 | 6 | no |
| 3 | none | 12220 | 2 | no |
| 4 | 1 vCPU (`DCFS_FORCE_CPUS=1`) | 12220 | 1 | no |
| 5 | 1 vCPU | 12220 | 1 | no |
| 6-8 | `-fprofile-update=atomic` | 12220 x3 | 0, 0, 0 | no |
| 9-10 | `-fprofile-update=atomic` | 12220 x2 | 0, 0 | no |

One vCPU lowers the rate but does not remove it (the compiler's plain
increment can be a load, add and store, which a preemption splits); atomic
increments removed it in 5 of 5 runs where the plain ones showed 1 to 17
functions in each of 5 runs. The rare value in the lcov (1 of the 3 two-vCPU
runs, and 1 of the 2 earlier full-suite runs) is the visible tip: the
functions that are negative in the other runs are in external code the
report filters out.

## What was not distinguished

The other candidates were not separately tested, only found unnecessary: each
binary writes one profile (two files in the guest's `/cov`), `dump_profraw`
waits until every instrumented process is gone and `/cov` stops changing
before the tar, so a tear at the snapshot or a second write is not in the
evidence; a startup race of a later process with the first one's counters
(continuous mode merges the file at start) cannot be excluded as a second
source, but atomic increments alone removed every negative count in five runs.
