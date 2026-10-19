# Working on dcfs

Rules for anyone changing this repository, human or agent. `README.md`
explains what dcfs is; `docs/design.md` explains how it works; the plan
is `docs/plan/README.md` (one file per phase in `docs/plan/phases/`).
Rules marked "(from phase N)" take effect when that plan phase lands.
 Every ratified rule has one line, with russ's words, the date and a link to its
reasoning, in one of the topic indexes under `docs/rules/` (russ, 2026-10-18:
"make AGENTS.md the complete index", then "multiple 'roles'": AGENTS.md says
which index to read in which case). This file holds only what every task needs.
`tools/repo_shape.py` fails when a rule marked in `docs/style.md`,
`docs/design.md`, `docs/plan/process.md` or `CLAUDE.md` is in no index, or an
index links to a marker that is gone. To add a rule: write it and its reasoning
where it belongs, put an anchor `<a id="rule-NAME"></a>` on it, and add a line
to the one index it belongs to. `.claude/agents/*.md` name the indexes each role
always reads and restate no rule.

## Always

- **Test first.** Every bug fix and every behavior change starts with a test
  that fails on the current code. Show the failure, then fix. Never weaken a
  test to make it faster or to make it pass.
  ([style 2](/docs/style.md#rule-test-first))
- **Tests run in QEMU guests**, unit tests included; only checks that need
  neither root nor the kernel run on the host.
  ([style 2](/docs/style.md#rule-tests-in-qemu))
- **Everything goes through Bazel**, run with access to `/dev/kvm`.
  ([style 3](/docs/style.md#rule-everything-through-bazel),
  [process](/docs/plan/process.md#rule-bazel-kvm))
- **Root is required.** dcfs runs as root; there are no unprivileged
  fallbacks. ([design](/docs/design.md#rule-root-required))
- **Warnings are errors**, and clang-tidy's findings are errors.
  ([style 1.1](/docs/style.md#rule-warnings-are-errors))
- **Never lower the coverage baseline.** Only ever increase it (russ,
  2026-10-11). ([style 2](/docs/style.md#rule-never-lower-coverage))
- **Follow `docs/style.md`** and run `bazel test --config=fast //tools/...` on
  your own diff before you report.
  ([process](/docs/plan/process.md#rule-agent-runs-style-checks))
- **Never push, publish or send anything outside the repository** unless the
  maintainer asks. ([process](/docs/plan/process.md#rule-never-push))
- **Commit subjects start with the plan step**, e.g. `3.2: third_party/linux:
  build the pinned kernel with Bazel`, and end with a Co-Authored-By trailer
  naming the model that wrote the commit.
  ([style 7](/docs/style.md#rule-commit-subject))
- **Delegate menial chunks to Gemini through foreman**, by the
  `dcfs-foreman` skill (`.claude/skills/dcfs-foreman/SKILL.md`), never a bare
  `agy -p`; your report has a `Delegated:` line.
  ([process](/docs/plan/process.md#rule-delegate-down))
- **Stop and report on a permission denial**; do not work around it.
  ([process](/docs/plan/process.md#rule-stop-on-denial))

## Read before you start

Read the indexes for your kind of task.

| Your task | Read |
|---|---|
| A change under `dcfs/`, `bench/` or `tools/` | [code](docs/rules/index-code.md), [style](docs/rules/index-style.md), [tests](docs/rules/index-tests.md) |
| Writing, changing or running a test | [tests](docs/rules/index-tests.md), [style](docs/rules/index-style.md) |
| The write-through protocol, identity, recovery, cancellation or `formal/` | [protocol](docs/rules/index-protocol.md), [code](docs/rules/index-code.md), [tests](docs/rules/index-tests.md) |
| Docs, Bazel files, shell, Python or a commit message | [style](docs/rules/index-style.md) |
| Running commands on russ's machine (kvm, `bazel shutdown`, polling loops, `pkill -f`, zsh noclobber, `cd` into the lane, Docker is production) | [process](docs/rules/index-process.md) |
| Dispatching, reviewing, merging, plan or bookkeeping work | [process](docs/rules/index-process.md) |
| A style review (`dcfs-style-reviewer`) | [style](docs/rules/index-style.md), [code](docs/rules/index-code.md), [tests](docs/rules/index-tests.md) |
| A review by `dcfs-reviewer` | all five |

The indexes: [code](docs/rules/index-code.md),
[style](docs/rules/index-style.md), [tests](docs/rules/index-tests.md),
[protocol](docs/rules/index-protocol.md),
[process](docs/rules/index-process.md).
