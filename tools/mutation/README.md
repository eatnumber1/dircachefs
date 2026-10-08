# Mutation testing (steps 26.5, 26.5d)

Limited mutation testing of the protocol code: a mutant that no test kills is
a missing test. It is a tool, not a test or a gate: running it takes hours and
starts Bazel itself. (Its own tests are host tests: `//tools/mutation:all`.)

## What is mutated

`scope.txt` names the files and, per file, the functions:
`dcfs/metadata_cache.cc` and `dcfs/backing.cc` (every function) and the
protocol paths of `dcfs/dir_cache_fs.cc` (every function that calls a
`Begin*`/`End*`/`Mark*` function, plus Forget, ForgetRemoved, Release, the sync
and reconcile helpers). The sites come from clang's own AST (`-ast-dump=json`,
the pinned clang 22, the file's compile command from `bazel aquery`), so macros
and templates are not guessed at by a regex; a node of a header taken for one
of the main file is dropped (`sane()`).

## Operators

Each is a small class in `operators.py` (`visit(node, ctx)` returns the
`Site`s at one AST node) with a unit test in `operators_test.py` on the
committed C++ fixture `fixture.cc`. The test compiles the fixture with the
pinned clang (a Bazel data dependency) through `mutate.ast_dump_command`, the
function production uses on `dcfs/`, so the AST under test is clang's own and
nothing but the source is checked in (the dump takes 0.3 s; the fixture
includes no header, its `absl` and logging pieces are stubs with the real
names). The report's unit is the operator's name:

| operator | what it does |
|---|---|
| `relational` | `<` <-> `<=`, `>` <-> `>=`, `==` <-> `!=` (the operator token of a builtin binary operator) |
| `logical` | `&&` <-> `||` |
| `negate` | the condition `X` of an if, while, for or `?:` becomes `!(X)` (`negate-if`, ...) |
| `enum-swap` | `kFound`/`kNegative` and `kPresent`/`kAbsent` exchanged (the tri-state records) |
| `constant` | an integer literal in a comparison or in `+ - * / %` (and `+=`...) becomes N+1, N-1 or 0 |
| `delete-statement` | an expression statement (a call, an assignment, `++`/`--`) or a void call anywhere becomes `(void)0` (`delete-call End`, `delete-statement`); a `Begin*`/`End*`/`Mark*` call returning `absl::Status` and used as a value becomes `absl::OkStatus()` |
| `status-return` | in a function returning `absl::Status`: `return <error>` becomes OK and `return OkStatus()` an error; in one returning `absl::StatusOr<T>`: a returned value becomes an error (an error has no OK value to make up; returns inside lambdas are not the function's) |
| `swap-args` | two adjacent arguments of one call, of the same type and different text, exchanged |

## Arid nodes

`arid.txt` lists, with a reason per rule, where a mutant is noise and none is
generated (`arid.py` documents the rule kinds): `LOG`/`VLOG`/`PLOG`/`DLOG`/
`LOG_IF`... statements and `DCHECK*` (a text scan finds the statement), the
message streamed after a `CHECK(cond)` (the condition itself is mutated: it
aborts), functions named `AbslStringify`, `ToString` or `DebugString`, testonly
hooks (`...ForTesting`, `TestOnly...`), and the message streamed into an error
builder (`NotFoundErrorBuilder() << "..." << x`: an AST rule on the head of a
`<<` chain). A test (`operators_test.py`, `AridTest`) shows that a mutant in a
log stream is not generated and that every rule has a reason.

## Sampling

`generate` keeps the cost flat as operators are added:

- at most **one mutant per source line** and at most `--per-function N`
  (default 2) per function, the operators taking turns inside a function so a
  cut keeps each represented;
- the choice is the order of a hash of `--seed` (default 1; printed; the
  weekly job passes its run number so each week looks at different mutants)
  and the mutant's stable id, so a mutant keeps its place when code elsewhere
  changes;
- `--all` writes every candidate (to look at them, or to find equivalents).

`run --sample N --seed S` (development) takes N of the mutants, operators
taking turns. The per-push mode below has its own budget.

## Equivalent mutants

`equivalent.txt` lists mutants no test can kill, one JSON object per line,
keyed by a stable id: `file`, `function`, `op`, `before`, `after` (text, not
line numbers; whitespace-insensitive) and optionally `nth` (the n-th identical
mutant of the function, in source order), with a `reason` that
`mutate_test.py` insists on. `run` and `changed` do not run a listed mutant:
its result is `suppressed` (with the reason) and the summary counts it apart.
Today it holds the 11 `End()` deletions on a path that returns at once after
a refused Checkpoint (`if (interrupted) { mutation.End(); return interrupted;
}`): the destructor ends the mutation at the return and nothing runs between
the two. (The `End()` before phase-3 refreshes is not equivalent: see the
`...EndsItsMutationBeforeItsRefreshes` tests.) To add one: copy the `SURVIVOR`
line's function and text, find `nth` with `generate --all` if the same text
occurs twice in the function, and write the reason.

## Running it

```
bazel run //tools/mutation:mutate -- generate --out /tmp/mutants.json
bazel run //tools/mutation:mutate -- run --mutants /tmp/mutants.json \
    --result /tmp/result.json [--sample 25 --seed 1] [--only 3,7,12]
bazel run //tools/mutation:mutate -- report /tmp/result.json
```

`generate` takes about two minutes (three AST dumps of 150, 60 and 110 MB).
`run` copies the tree (`git ls-files`, plus `user.bazelrc`) to a scratch
directory, which gives it its own Bazel output base; the shared disk cache
means only the mutated file's compile, the links and the tests cost anything.
For each mutant it edits the file, runs

```
bazel test --notest_keep_going --test_output=errors --config=fast //dcfs/...
bazel test --notest_keep_going --test_output=errors //dcfs:dir_cache_fs_trace_test
```

(two phases, the second only for a mutant the first did not kill: a size
filter such as `--config=fast` applies to every target of an invocation and
would drop the medium trace test; `--killers "phase;phase"` replaces them) and
restores the file. Exit status 3 (a test failed or timed out) is `killed`, 0
is `survived`, 1 is `invalid` (the mutant does not compile; not counted).
`--notest_keep_going` ends the mutant at its first failing test, so killed
mutants are cheap and survivors cost the whole tier. The result file is
rewritten after every mutant, so a long run can be read while it goes.

## Reporting

`run` prints a table, per operator, of killed / survived / invalid /
suppressed / error and the mutants per hour (mutants that ran over the time
they took), then the survivors **grouped by function**:

```
== dcfs/dir_cache_fs.cc Release: 2 survivors
SURVIVOR dcfs/dir_cache_fs.cc:1738 in Release: relational: `<` -> `<=`
```

(the diff is the tokens that changed). `report RESULT.json...` merges result
files (the weekly job's `report` job puts its output in the job summary, so
the next sweep can be judged against the last). `--survivors-out` and
`--summary-out` write them to files.

## Per push and on a schedule (step 26.5b)

- **Per push** (`ci.yml`, the `mutation-changed` job): `mutate.py changed
  --range BASE..TIP --fail-on-survivor` mutates only the functions of
  `scope.txt` whose lines the range touches (the `git diff -U0` hunks, mapped
  to the AST's function ranges), one mutant per line, minus the suppressed
  ones, and runs at most `--max-mutants` of them, **30**: the budget. The
  selection is deterministic (the seeded hash order with the operators taking
  turns), and the surplus is reported as "not run" in the output. It fails
  the job when a mutant survives the small tier and trace validation: the
  change to that function needs a test. A range that touches nothing in scope
  runs nothing. A new branch's all-zero base means the tip against its parent.
- **On a schedule** (`.github/workflows/mutation.yml`: weekly, and by
  `workflow_dispatch`): the sampled scope (about 340 mutants), split into six
  contiguous id ranges (`run --shard K/6`), one runner each. Survivors are
  findings (the `mutation-survivors` artifact and the job summary), not
  failures; a job fails only on a tooling error (exit status 2).
- Estimate for a runner (4 idle vCPUs, from this machine's 91 s per mutant for
  the small tier, 311 s more for the quarter that survives it): about 57
  mutants and 2.7 hours per shard plus half an hour of cold build, under the
  6-hour limit. Per push: 30 mutants at most: 20 to 150 minutes, the job has a
  180-minute limit.

`mutate_test` covers the modes: sampling (one per line, the per-function
bound, seeds, the budget), equivalents, the report, the hunk-to-function
mapping, the shards, the exit codes (survivor: 0 on the schedule and 1 per
push, tooling error: 2), and `run` end to end over a throwaway git repository
with a fake bazel.

SWEEP

## Reading survivors

`SURVIVOR file:line in function: operator: diff` is one missing test: the
tests all passed with that behaviour changed. Look at the line, ask what
observable difference the mutation makes (a reply, a cache row, a trace line)
and whether a unit test (the `dir_cache_fs_test` harness, forged requests) or a
trace scenario can observe it. An equivalent mutant (a negated condition whose
both branches do the same, a `<` that only differs at an unreachable bound)
is noise: add it to `equivalent.txt` with the reason so it is skipped next
time; an arid place (a message, a log) goes in `arid.txt`.

## Why not mull

mull-project/mull has builds for LLVM 22 (0.34.1, `Mull-22-0.34.1-LLVM-22.1.2`,
a Ubuntu 26.04 `.deb`), but it does not fit: it instruments the program under
test and runs it on the host with the mutant switched on by an environment
variable, and our killers run dcfs and the tests inside a QEMU guest (the
instrumented binary would have to be mutated, and selected, inside the guest
from outside), its LLVM 22.1.2 plugin does not match the pinned clang 22.1.8
(a pass plugin must be built against the same LLVM), and it is a `.deb`, not
something Bazel fetches and verifies by sha256 on this host. A source-level
mutator that drives `bazel test` is the fit. (Dextool needs a D toolchain.)
The ideas are Google's (Petrovic and Ivankovic, "State of Mutation Testing at
Google" and "Does Mutation Testing Improve Testing Practices?"): arid nodes,
one mutant per line, sampling per function, and a per-change budget.
