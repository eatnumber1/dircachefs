# Working on dcfs

Rules for anyone changing this repository, human or agent. `README.md`
explains what dcfs is; `docs/design.md` explains how it works; the plan
is `docs/plan/README.md` (one file per phase in `docs/plan/phases/`).
Rules marked "(from phase N)" take effect when that plan phase lands.

Every ratified rule has one line, with russ's words, the date and a link to
its reasoning, in one of the topic indexes under `docs/rules/` (russ,
2026-10-18: "make AGENTS.md the complete index", then "multiple 'roles'":
AGENTS.md says which index to read in which case). This file holds only what
every task needs. `tools/repo_shape.py` fails when a rule marked in
`docs/style.md`, `docs/design.md`, `docs/plan/process.md` or `CLAUDE.md` is in
no index, or an index links to a marker that is gone. To add a rule: write it
and its reasoning where it belongs, put an anchor `<a id="rule-NAME"></a>` on
it, and add a line to the one index it belongs to.

## Always

- **Test first.** Every bug fix and every behavior change starts with a test
  that fails on the current code. Show the failure (quote it in the commit
  message or the review), then fix.
- Never weaken a test to make it faster or to make it pass.
- **Tests run in QEMU guests**, unit tests included; only checks that need
  neither root nor the kernel (clang-tidy, formatting, model checking) run on
  the host.
- **Everything goes through Bazel**: every build step, tool, image and test is
  a Bazel target with declared inputs. Run Bazel with access to `/dev/kvm`,
  never pipe it through `tail` or `head` in an `&&` chain, and shut down the
  servers you started in temporary worktrees.
- **Root is required.** dcfs runs as root; there are no unprivileged fallbacks.
- **Warnings are errors**, and clang-tidy's findings are errors.
- **You must not lower the coverage baseline.** That's why it exists. Only
  ever increase it (russ, 2026-10-11).
- **Follow `docs/style.md`** and run `bazel test --config=fast //tools/...` on
  your own diff before you report.
- **Never push, publish or send anything outside the repository** unless the
  maintainer asks.
- **Commit subjects start with the plan step**, e.g. `3.2: third_party/linux:
  build the pinned kernel with Bazel`, and end with a Co-Authored-By trailer
  naming the model that wrote the commit.
- Stop and report on a permission denial; do not work around it.

## Read before you start

Read the indexes for your kind of task. A review reads all of them.

| Your task | Read |
|---|---|
| A change under `dcfs/`, `bench/` or `tools/` | [code](docs/rules/index-code.md), [style](docs/rules/index-style.md) |
| Writing, changing or running a test | [tests](docs/rules/index-tests.md), [style](docs/rules/index-style.md) |
| The write-through protocol, recovery, cancellation or `formal/` | [protocol](docs/rules/index-protocol.md), [code](docs/rules/index-code.md) |
| Docs, Bazel files, shell or Python | [style](docs/rules/index-style.md) |
| Dispatching, reviewing, merging, plan or bookkeeping work | [process](docs/rules/index-process.md) |
| A review of any step | all five |

The indexes: [code](docs/rules/index-code.md),
[style](docs/rules/index-style.md), [tests](docs/rules/index-tests.md),
[protocol](docs/rules/index-protocol.md),
[process](docs/rules/index-process.md).
