# Mutation testing (step 26.5)

Limited mutation testing of the write-through protocol code: a mutant that no
test kills is a missing test. It is a tool, not a test or a gate: running it
takes hours and starts Bazel itself.

## What is mutated

`scope.txt` names the files and, per file, the functions:
`dcfs/metadata_cache.cc` (every function) and the protocol paths of
`dcfs/dir_cache_fs.cc` (every function that calls a `Begin*`/`End*`/`Mark*`
function, plus Forget, ForgetRemoved, Release, the sync and reconcile helpers).
Operators, inside those functions only:

- `negate-if`, `negate-while`, `negate-for`, `negate-?:`: the condition `X`
  becomes `!(X)`;
- `delete-call <name>`: a call to a `Begin*`/`End*`/`Mark*` function that
  returns `absl::Status` or `void` becomes `absl::OkStatus()` or `(void)0`
  (calls returning `StatusOr` are not deleted: the value is used);
- `swap`: `kFound`/`kNegative` and `kPresent`/`kAbsent` exchanged.

The sites come from clang's own AST (`-ast-dump=json`, the pinned clang 22,
the file's compile command from `bazel aquery`), so macros and templates are
not guessed at by a regex; a node of a header taken for one of the main file
is dropped (`sane()`).

## Running it

```
bazel run //tools/mutation:mutate -- generate --out /tmp/mutants.json
bazel run //tools/mutation:mutate -- run --mutants /tmp/mutants.json \
    --result /tmp/result.json [--sample 25 --seed 1] [--only 3,7,12]
```

`generate` takes about a minute (two AST dumps of 150 and 60 MB) and writes the
mutant list (277 today). `run` copies the tree (`git ls-files`, plus
`user.bazelrc`) to a scratch directory, which gives it its own Bazel output
base; the shared disk cache means only the mutated file's compile, the links
and the tests cost anything. For each mutant it edits the file, runs

```
bazel test --notest_keep_going --test_output=errors --config=fast //dcfs/...
bazel test --notest_keep_going --test_output=errors //dcfs:dir_cache_fs_trace_test
```

(two phases, the second only for a mutant the first did not kill: a size filter
such as `--config=fast` applies to every target of an invocation and would drop
the medium trace test; `--killers "phase;phase"` replaces them) and restores the
file. Exit status 3 (a test
failed or timed out) is `killed`, 0 is `survived`, 1 is `invalid` (the mutant
does not compile; not counted). `--notest_keep_going` ends the mutant at its
first failing test, so killed mutants are cheap and survivors cost the whole
tier. The result file is rewritten after every mutant, so a long run can be
read while it goes.

## Per push and on a schedule (step 26.5b)

- **Per push** (`ci.yml`, the `mutation-changed` job): `mutate.py changed
  --range BASE..TIP --fail-on-survivor` mutates only the functions of
  `scope.txt` whose lines the range touches (the `git diff -U0` hunks, mapped
  to the AST's function ranges), at most `--max-mutants` (30; a sample with the
  fixed seed 1 beyond that), and fails the job when one survives the small tier
  and trace validation: the change to that function needs a test. A range that
  touches nothing in scope runs nothing. A new branch's all-zero base means the
  tip against its parent.
- **On a schedule** (`.github/workflows/mutation.yml`: weekly, and by
  `workflow_dispatch`): the whole scope, split into six contiguous id ranges
  (`run --shard K/6`), one runner each. Survivors are findings (the
  `mutation-survivors` artifact and the job summary), not failures; a job fails
  only on a tooling error (exit status 2).
- Estimate for a runner (4 idle vCPUs, from this machine's 91 s per mutant for
  the small tier, 311 s more for the quarter that survives it): about 13 hours
  in all, so 46 mutants and about 2.1 hours per shard plus half an hour of cold
  build: about 2.7 hours each, under the 6-hour limit. Per push: a typical
  commit touches a few functions (a dozen mutants, 30 at most): 20 to 150
  minutes, the `full` job's step has a 120-minute limit.

`mutate_test` covers both modes: the hunk-to-function mapping, the shards, the
exit codes (survivor: 0 on the schedule and 1 per push, tooling error: 2), and
`run` end to end over a throwaway git repository with a fake bazel.

## How long

On this machine (4 cores shared with other lanes, load average about 18): a
killed mutant takes 40 to 220 s (91 s on average for the small tier), a
survivor the whole small tier and then the trace test, 400 to 700 s. 277
mutants: about 7 hours for the small tier plus about 6 hours of trace runs
for the roughly one in four mutants it does not kill, so 13 hours; on a
quiet machine less. Run it on a schedule, a sample (`--sample N --seed S`)
while developing.

## First run (2026-10-08, a seeded sample)

277 mutants generated (206 negate-if, 3 negate-for, 1 negate-while, 15
negate-?:, 41 delete-call, 11 swap). A 25-mutant sample (`--sample 25 --seed
1`): 19 killed by the small tier, 6 survived it; the trace test (a second run
over those 6) killed 2 of them. Result: **25 mutants, 21 killed, 4 survived,
0 invalid**; 2265 s for the small tier (91 s each) and 2404 s for the 6
survivors' two phases (401 s each). The four survivors, as missing-test
findings:

1. `dir_cache_fs.cc:1257`, `ForgetRemoved`: `if (absl::IsNotFound(status))
   return OkStatus()` negated. No test makes `DeleteInode` report NotFound (a
   row already gone) and checks that FORGET of a removed inode still
   succeeds, nor the reverse (a real error propagated).
2. `dir_cache_fs.cc:1619`, `RecordWrittenAttrs`: `!marked.ok() &&
   !IsNotFound(marked)` negated. The path where refreshing the attributes of a
   written file fails and the best-effort `MarkAttrsUnknown` fails too is not
   reached (it needs the cache-disk error injection of Phase 11; the log line
   is the only effect).
3. `dir_cache_fs.cc:1779`, `Release`: `!retired.ok()` negated. A failing
   `RetireRemoved` (the row of an unlinked inode cannot be deleted at last
   release) is not tested; the effect is a warning and a leaked row.
4. `dir_cache_fs.cc:2406`, `CopyFileRange`: `mutation->End()` deleted. No test
   notices that the copy's mutation is never ended (it stays in flight: the
   directory cannot be completed or synced, the trace misses its `end`). The
   other `End` deletions of the sample (`dir_cache_fs.cc:824`, `1681`)
   were killed; this one needs a copy_file_range followed by a listing or a
   sync point that depends on the mutation having ended. The most valuable of
   the four.

(`dir_cache_fs.cc:1683` and `1728`, `Release`, survived the small tier and
were killed only by `//dcfs:dir_cache_fs_trace_test`: trace validation earns
its place.)

## Reading survivors

`SURVIVOR file:line in function: operator: before -> after` is one missing
test: the tests all passed with that behaviour changed. Look at the line, ask
what observable difference the mutation makes (a reply, a cache row, a trace
line) and whether a unit test (the `dir_cache_fs_test` harness, forged requests)
or a trace scenario can observe it. Equivalent mutants (a negated condition
whose both branches do the same, a swap in a log line) are noise: list them
here when found, with the reason, so they are skipped next time.

## Why not mull

mull-project/mull has builds for LLVM 22 (0.34.1, `Mull-22-0.34.1-LLVM-22.1.2`,
a Ubuntu 26.04 `.deb`), but it does not fit: it instruments the program under
test and runs it on the host with the mutant switched on by an environment
variable, and our killers run dcfs and the tests inside a QEMU guest (the
instrumented binary would have to be mutated, and selected, inside the guest
from outside), its LLVM 22.1.2 plugin does not match the pinned clang 22.1.8
(a pass plugin must be built against the same LLVM), and it is a `.deb`, not
something Bazel fetches and verifies by sha256 on this host. A source-level
mutator that drives `bazel test` is the fit.
