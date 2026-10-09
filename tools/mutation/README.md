# Mutation testing (steps 26.5, 26.5d, 12.13)

Limited mutation testing of the protocol code: a mutant that no test kills is
a missing test. It is a tool, not a test or a gate: running it takes hours and
starts Bazel itself. (Its own tests are host tests: `//tools/mutation:all`.)

Exceptions to "everything goes through Bazel", on purpose: the tool drives
Bazel, so `generate` runs the pinned clang on the file's own compile command
(from `bazel aquery`) outside the action graph for the AST dump, and the
workflows run it with the runner's system `python3`, not a Bazel-built one
(`bazel run //tools/mutation:mutate` works too). Its unit tests run the same
pinned clang as a Bazel data dependency.

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
line's function and text, find `nth` with `generate --all` (always give it: an
entry without `nth` suppresses every identical mutant, also ones added
later), and write the reason. `nth` counts in source order, so editing the
function can re-target an entry: `generate` warns about every entry that
matches no mutant (its code went: delete or re-key it) or several (add `nth`),
so re-key from `generate --all` when it does. Equivalents are split off
before sampling, so they take no line or function slot from live mutants.

## Running it

```
bazel run //tools/mutation:mutate -- generate --out /tmp/mutants.json
bazel run //tools/mutation:mutate -- run --mutants /tmp/mutants.json \
    --result /tmp/result.json [--sample 25 --seed 1] [--only 3,7,12]
bazel run //tools/mutation:mutate -- report /tmp/result.json
```

`generate` takes about two minutes (three AST dumps of 150, 60 and 110 MB).
`run` copies the tree (`git ls-files`, plus `user.bazelrc`) to a scratch
directory with its own Bazel output base inside it (`--output_base`); the
shared disk cache means only the mutated file's compile, the links and the
tests cost anything. The output base is `bazel clean --expunge`d and the
scratch directory removed when `run` ends, also on an error or SIGTERM (which
becomes an exit); only `kill -9` leaves them behind (`/tmp/dcfs-mutate.*`).
It first runs the killers on the unmutated tree (the baseline: a failing tree
is a tooling error, and the cold build is paid there, not by the first mutant).
For each mutant it edits the file, runs

```
bazel test --notest_keep_going --test_output=errors --config=fast //dcfs/...
bazel test --notest_keep_going --test_output=errors //dcfs:dir_cache_fs_trace_test
```

(two phases, the second only for a mutant the first did not kill: a size
filter such as `--config=fast` applies to every target of an invocation and
would drop the medium trace test; `--killers "phase;phase"` replaces them) and
restores the file. Exit status 3 (a test failed or timed out) is `killed`, but
the killing target is rerun with `--nocache_test_results` and a pass makes it
`flaky` (listed as `FLAKY`, not a kill); 0 is `survived`; 1 is `invalid` (the
mutant does not compile; not counted) only when the compiler's error is in the
mutated file, otherwise `error`, a tooling failure (exit 2).
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
  --range BASE..TIP` mutates only the functions of
  `scope.txt` whose lines the range touches (the `git diff -U0` hunks, mapped
  to the AST's function ranges), one mutant per line, minus the suppressed
  ones, and runs at most `--max-mutants` of them, **30**: the budget. The
  selection is deterministic (the seeded hash order with the operators taking
  turns), and the surplus is reported as "not run" in the output; `--time-budget`
  (120 minutes) does the same for the wall clock. A mutant that survives the
  small tier and trace validation is a finding, not a failure (the change to
  that function needs a test): the job summary shows the table and the
  survivors grouped by function (`mutate.py report`), the `mutation-changed`
  artifact holds the results, and only a tooling error (exit status 2) fails
  the job (`--fail-on-survivor` is there for local use). A range that touches nothing in scope
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

## First sweep (2026-10-08, step 26.5d)

`generate` (seed 1, `--per-function 2`) gave 354 mutants (343 to run and 11
suppressed) from 1582 candidates over `dcfs/metadata_cache.cc`,
`dir_cache_fs.cc` and `backing.cc`. A full run would take about 30 hours at
this machine's load, so `run --sample 30 --seed 1` ran 30 of the live ones
(9%, the operators taking turns), after the baseline run (which passed). The
11 suppressed (equivalent) mutants are counted but not run:

| operator | killed | survived | invalid | suppressed | error | flaky | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| constant | 3 | 0 | 0 | 0 | 0 | 1 | 4 |
| delete-statement | 2 | 2 | 0 | 11 | 0 | 0 | 15 |
| enum-swap | 2 | 1 | 0 | 0 | 0 | 0 | 3 |
| logical | 1 | 0 | 3 | 0 | 0 | 0 | 4 |
| negate | 3 | 1 | 0 | 0 | 0 | 0 | 4 |
| relational | 3 | 1 | 0 | 0 | 0 | 0 | 4 |
| status-return | 3 | 1 | 0 | 0 | 0 | 0 | 4 |
| swap-args | 1 | 2 | 0 | 0 | 0 | 0 | 3 |
| **all** | 18 | 8 | 3 | 11 | 0 | 1 | 41 |

30 mutants ran in 10003 s: **10.8 mutants per hour** (333 s each) under load
(load average 16; a kill is also rerun once to rule out a flaky test). The
8 survivors, the next 8.2x list (not fixed in this step), by function; one
mutant was `flaky` (`Tmpfile`, `dir_cache_fs_test` failed once and passed on a
rerun):

- `dcfs/backing.cc:1831` `FsyncDirFd`: error->ok `FsyncFd(*dir, datasync)` -> `absl::OkStatus()`
- `dcfs/backing.cc:672` `InitRoot`: negate-if `!inserted.ok() && !absl::IsAlreadyExists(inserted)` -> `!(!inserted.ok() && !absl::IsAlreadyExists(inserte`
- `dcfs/backing.cc:1974` `ProbeRecoveredRows`: delete-statement `gone = true` -> `(void)0`
- `dcfs/backing.cc:1999` `ProbeRecoveredRows`: relational > `>` -> `>=`
- `dcfs/backing.cc:1586` `RecordNewChild`: swap kAbsent/kPresent `events::Probe::Kind::kAbsent` -> `events::Probe::Kind::kPresent`
- `dcfs/backing.cc:510` `RefuseReservedIno`: swap-args `"Backing inode number ", stx.stx_ino` -> `stx.stx_ino, "Backing inode number "`
- `dcfs/backing.cc:1750` `UnlinkAt`: delete-call BackingCall `BackingCall(ctx, "unlinkat")` -> `(void)0`
- `dcfs/metadata_cache.cc:312` `WithStatx`: swap-args `stx.stx_rdev_major, stx.stx_rdev_minor` -> `stx.stx_rdev_minor, stx.stx_rdev_major`

The `BackingCall` deletion is a trace hook; others may be equivalent or arid
(triage them into `equivalent.txt` or `arid.txt`).

## TLA+ mode (step 12.13)

`mutate.py --lang tla` mutates the TLA+ model (`formal/dcfs.tla` and the
modules of `tla_scope.txt`: `ident`, `lifetime`, `reval` and the `Trace*`
modules; not the `MC*` harnesses, whose mutants would change a test, nor the
`known_bugs/` and `limitations/` variants) and asks which mutants no `//formal`
test tells from the real model. A survivor is an under-specified property:
the model, with that step changed, still passes every test, including that
every known-bug and limitation test still finds its counterexample (a mutant
that silences a known bug is killed by that test). It is one tool and one
report: the same `generate`, `run`, `changed` and `report`, the same sampling
(one mutant per line, `--per-function`, the seeded hash order), the same
`equivalent.txt` (an entry for a `.tla` file is matched only in this mode),
the same table and `SURVIVOR` lines.

```
bazel run //tools/mutation:mutate -- generate --lang tla \
    --module formal/dcfs.tla --seed 1 --max-mutants 20 --out /tmp/tla.json
bazel run //tools/mutation:mutate -- run --mutants /tmp/tla.json \
    --result /tmp/tla-result.json --tier small [--time-budget 90]
bazel run //tools/mutation:mutate -- changed --lang tla --tier small \
    --range BASE..TIP --max-mutants 10 --result /tmp/tla-changed.json
```

`generate --lang tla` needs neither Bazel nor clang (about half a second for
`dcfs.tla`: 1,350 candidates). `run` takes the language from the mutants file.
`--max-mutants N` keeps N of the sample (seeded, operators in turn);
`--time-budget MIN` stops `run` from starting a mutant after MIN minutes (the
remaining ones are "not run"); `--module PATH` (repeatable) replaces the scope
file. `run` copies the tree as before, writes the mutated module into the
copy and runs, with one Bazel invocation per phase,

```
bazel test --notest_keep_going --test_output=errors --config=fast //formal/...
bazel test --notest_keep_going --test_output=errors --config=presubmit //formal/...
```

(the small tier, then the small and medium tiers: the small results are
cached, so the second phase costs only the medium tests; `--tier small` stops
after the first, as the per-push job does; a medium `MC_nolock_small` joins the
second phase when 12.12a adds it, with no change here). The `//formal/...`
targets include the TLC tests and the trace validation of hand-written logs;
the guest traces (`tla_trace_test` in `//dcfs` and `//test/qemu`) are not
in the set. A baseline run of the same phases on the unmutated tree comes first.

### Outcomes

- **killed**: a test failed (Bazel exit 3), which includes a known-bug test
  that no longer finds its counterexample and a test that timed out (a mutant
  can blow the state space up). TLC is deterministic, so a kill is rerun
  (`--nocache_test_results`) only after a timeout (load can cause one): a rerun
  that passes makes it `flaky`.
- **survived**: every test passed. A finding.
- **invalid**: TLC itself failed rather than finding a violation: its exit
  status is not 0, 11 (deadlock), 12 or 13 (a violation) in the `tlc_test`
  runner's status line, or, for a test without that line (trace validation),
  the output holds a parse error (`Fatal errors while parsing`, `Semantic
  errors`) or an `Error:` line that is not a violation. Not counted as a kill
  or as a survivor; listed as `INVALID file:line in def: operator: first
  error` after the survivors, and in the table's `invalid` column.

### Operators

There is no parser. A conjunct-aware token scanner (`tla_operators.py`)
masks the comments and strings (offsets kept), tokenizes with line, column and
bracket depth, finds the definitions (a token at column 0 followed by `==`),
the bulleted `/\` and `\/` lists (the first bullet at the start of a line or
after `IN`, `THEN`, `ELSE`, `:`, `==` ...; the others at the same column and
depth; nested lists, `LET`/`IN`, `\A`/`\E` and `IF` inside items work), the
inline chains `a /\ b /\ c`, and which definitions and items are *effectful*
(a primed variable, `UNCHANGED`, or a definition that has one, transitively
and through the modules a module extends). The unit tests
(`tla_operators_test`) run on `testdata/snippets.tla`, constructs copied from
`dcfs.tla` (bulleted lists, nested indentation, `LET`, `\A`, `\E`, `EXCEPT`,
primes, `UNCHANGED`, tuples, `Commit` calls), and on `dcfs.tla` itself
(brackets stay balanced, no site in a comment, ids stable under a move).

| operator | what it does |
|---|---|
| `negate` | a pure conjunct or disjunct `X` becomes `~(X)` (`negate-conjunct`, `negate-disjunct`); the condition of an `IF` (`negate-if`) |
| `drop-guard` | a pure conjunct of an action becomes `TRUE` (`drop-conjunct` in a predicate); an effect or a frame (`x' = e`, `UNCHANGED`, `Unchanged...`) is never dropped |
| `drop-disjunct` | a disjunct becomes `FALSE`: an item of the next-state relation (each name of `Next`, or a nested list) or of a predicate |
| `swap-junction` | `/\` and `\/` exchanged: all the bullets of a pure list, or all the operators of a pure inline chain at once (TLA+ has no precedence between them) |
| `relational` | `<` and `<=`, `>` and `>=`, `=` and `#`; not an assignment (`x' = e`) or an `EXCEPT` path (`!.f[k] = v`) |
| `constant` | a number to N+1 and N-1 (not a range bound or an index), `TRUE` and `FALSE`, a member of a set in `tla_sets.txt` (the regimes `"seq"`/`"metaprefix"`/`"ext4"`, a row's `Unknown`/`Absent`/`NoRow`, F's `"no"`/`"atime"`/`"mut"`) to its neighbours |
| `durability` | the second argument of a `Commit(row, sync)` call (the model's commit kinds: a normal commit, a synced one) goes to the other level, an expression to `FALSE` |
| `drop-step` | a `!.pc = "X"` becomes the label step X itself moves to (the step is skipped); an element of a tuple or sequence literal (none outside `UNCHANGED` in `dcfs.tla` today) |
| `swap-step` | two adjacent elements of a tuple or sequence literal |

What it cannot reach: operands of an infix chain mixed with a looser operator
(`a /\ b \/ c`, `\E x : P /\ Q`: where an operand ends needs the grammar, so
the chain is left alone), definitions not at column 0 and `a (+) b ==`
definitions, the order of two conjuncts (TLA+ conjunction commutes, so swapping
"two steps" means the elements of a sequence and the `pc` chain, which is where
this model sequences), operators or constants only the cfg decides (the
`Bug*` flags, the bounds), and the `Trace*.tla` event guards beyond what the
same operators find in them. A mutant TLC rejects is `invalid`, not a bug of the
tool: the scanner does not type-check.

### Arid

Comments (masked, so never mutated); a definition named by a `VIEW` line of any
`.cfg` beside the module (a mutated view hides or splits states; it is not an
under-specified property); the `def-regex` rules of `tla_arid.txt` (each with
its reason: the `View*` and `CrashImage*` definitions); and source marked in
the module itself:

```
\* mutation: arid begin the reason
...
\* mutation: arid end
x' = y   \* mutation: arid the reason (this line only)
```

A marker without a reason, an `end` without a `begin` or a `begin` never
closed is a tooling error. `tla_operators_test` (`AridTest`) covers each.

### Survivor workflow

Same as the C++ one: a `SURVIVOR` is a missing property or configuration. Look
at the definition and ask which behavior the mutant changes and which test
should have seen it: a guard no configuration exercises (`Requests` without
the request kind, a bound too small to reach it) needs a configuration; a
conjunct of an invariant that nothing falsifies needs a known-bug variant that
violates exactly it. An equivalent mutant (a guard implied by another, a state
the model cannot reach) goes in `equivalent.txt` with the reason, keyed by the
`.tla` file, definition, operator and text (`nth` as for C++); an arid place
in `tla_arid.txt` or with a marker. Do not add to `equivalent.txt` to make a
sweep pass: a survivor is the next step's finding.

### On a schedule and per push

- **Per push** (`ci.yml`, `mutation-changed`, a second step): `changed --lang
  tla --tier small --max-mutants 10 --time-budget 45` mutates only the
  definitions of the scope's modules whose lines the range touches (finer
  than "the modules": a push that touches a comment or a cfg runs nothing, and
  with no Bazel). Survivors are findings in the job summary and the
  `mutation-changed` artifact; only a tooling error (exit 2) fails the job.
- **Weekly** (`mutation.yml`, `mutation-tla` and `report-tla`): the sample of
  90 (seeded with the run number), three shards, the small and medium tiers
  per mutant, 300 minutes of time budget each.

### First TLA+ run (2026-10-09, step 12.13)

A proving run of the tool, not a sweep: `generate --lang tla --module
formal/dcfs.tla --seed 1 --per-function 2 --max-mutants 20` (1,351 candidates,
20 sampled, the operators in turn), `run --tier small`: the killers are the
small tier of `//formal/...` only (the medium tier, with the known bugs, would
turn most survivors into kills or into the same survivors at ten times the
cost). Run on a loaded 4-core machine in five invocations (a tool call lasts
ten minutes) with the baseline run once; an earlier sample of the same seed
contained a `swap-junction` of one inline operator of a three-operand chain,
which TLA+ rejects (`a /\ b \/ c` has no precedence: SANY's parse error,
INVALID: the tool now swaps all the operators of a chain at once), and the
seven mutants that sample shared with the final one were not rerun (same
text, same module). Nothing was added to `equivalent.txt`: the survivors are
the next step's findings.

| operator | killed | survived | invalid | suppressed | error | flaky | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| constant | 2 | 0 | 0 | 0 | 0 | 0 | 2 |
| drop-conjunct | 0 | 2 | 0 | 0 | 0 | 0 | 2 |
| drop-disjunct | 0 | 2 | 0 | 0 | 0 | 0 | 2 |
| drop-guard | 2 | 0 | 0 | 0 | 0 | 0 | 2 |
| drop-step | 1 | 1 | 0 | 0 | 0 | 0 | 2 |
| durability | 1 | 2 | 0 | 0 | 0 | 0 | 3 |
| negate | 2 | 0 | 0 | 0 | 0 | 0 | 2 |
| relational | 3 | 0 | 0 | 0 | 0 | 0 | 3 |
| swap-junction | 0 | 2 | 0 | 0 | 0 | 0 | 2 |
| **all** | 11 | 9 | 0 | 0 | 0 | 0 | 20 |

20 mutants, 3,657 s of mutant time: 19.7 per hour (a kill costs 11 to 313 s,
a survivor 123 to 560 s, which is the whole small tier). The 9 survivors (small
tier only; the medium tier may kill some):

- `dcfs.tla:241` `TypeOK`: swap-junction `/\` -> `\/` (the list of type conjuncts)
- `dcfs.tla:517` `Serve`: swap-junction `\/` -> `/\`
- `dcfs.tla:651` `RDFromCode`: drop-step `"PD_read"` -> `"PD_commit"` (the retry skips a populate)
- `dcfs.tla:1060` `S2`: durability `FALSE` -> `TRUE` (the sync point's commit made synced)
- `dcfs.tla:1503` `FBehind`: drop-conjunct `d.fAttr # b.f` -> `TRUE`
- `dcfs.tla:1568` `Recover`: durability `FALSE` -> `TRUE`
- `dcfs.tla:1707` `Next`: drop-disjunct `UnlinkSyscall(p)` -> `FALSE`
- `dcfs.tla:1712` `Next`: drop-disjunct `AttrChangeSyscall(p)` -> `FALSE`
- `dcfs.tla:1826` `FileExactStrict`: drop-conjunct `mode = "up"` -> `TRUE`

The 11 killed ones were killed by `trace_*`, `litmus_*` and `limitation_*`
tests of the small tier. The `Next` survivors say that no small-tier
configuration needs the unlink or attribute-change syscall step (the medium
tier's `MC_small` does); `TypeOK` is checked only by the medium and large
configurations.

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
