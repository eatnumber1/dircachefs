# Index of test rules

Read this before you write, change or run a test, or review one: what runs in
QEMU guests, test tiers, load-agnostic verdicts, coverage and fakes. Each line
is a ratified rule, in russ's words, or marked as an orchestrator ruling, with
the date where there is one and a link to the section that holds its reasoning.
`AGENTS.md` says which indexes to read for which task.

- Test first: write the test, show it failing on the current code, then fix;
  never weaken a test to make it pass.
  [style 2](/docs/style.md#rule-test-first),
  [design](/docs/design.md#rule-design-test-first)
- Every test of dcfs runs in a QEMU guest as root; only checks that need neither
  run on the host. [style 2](/docs/style.md#rule-tests-in-qemu),
  [design](/docs/design.md#rule-design-everything-in-qemu)
- No new shell for guest tests (from step 25.34): a new guest test is a
  `qemu_cc_test`, and a new guest script is thin glue (orchestrator ruling,
  2026-10-18). [style 4](/docs/style.md#rule-new-guest-script-thin-glue)
- Fakes, not mocks; test-only code lives in `*_test.cc` or `testonly/`, and
  syscall fault injection uses link-time `--wrap` fakes.
  [style 2](/docs/style.md#rule-fakes-not-mocks)
- A test helper does not reuse a production struct (orchestrator ruling,
  2026-10-18). [style 2](/docs/style.md#rule-test-helper-structs)
- Tests may crash on what is not under test, but ASSERT/EXPECT only the property
  under test (russ, 2026-10-09/11).
  [style 1.6b](/docs/style.md#rule-tests-may-crash)
- Coverage (from phase 7/8): new code arrives covered, error paths included, and
  the baseline is never lowered (russ, 2026-10-11: "You must not lower the
  coverage baseline. That's why it exists. Only ever increase it.").
  [style 2](/docs/style.md#rule-never-lower-coverage),
  [process](/docs/plan/process.md#rule-coverage-baseline)
- Test sizes are tiers: every test declares `size` and `timeout`; `small` runs
  constantly, `medium` before submitting, `large` and `enormous` in CI.
  [style 2](/docs/style.md#rule-test-tiers)
- A test's verdict does not depend on host load or speed (russ, 2026-10-17: "in
  general for all our tests I'm interested in them being load-agnostic").
  [style 2](/docs/style.md#rule-load-agnostic-tests)
- Shard with Bazel's `shard_count` (russ, 2026-10-18, step 5.12).
  [style 2](/docs/style.md#rule-shard-count)
- The kernel-facts pins test the kernel, not dcfs, so they run only weekly
  (russ, 2026-10-18). [style 2](/docs/style.md#rule-kernel-facts-weekly)
- Every kernel invariant dcfs relies on is pinned by a native `kernel_facts_test`
  case citing `path:line` (russ, 2026-10-18: "add a test").
  [style 2](/docs/style.md#rule-kernel-invariants-pinned)
- Test conventions: googletest fixtures, extend an existing test or write a new
  one by cost, matchers over booleans.
  [style 2](/docs/style.md#rule-googletest),
  [style 2](/docs/style.md#rule-extend-or-new-test),
  [style 2](/docs/style.md#rule-matchers-over-booleans)
- A test comment does not start with its step number (russ, 2026-10-17), and a
  test ported from a script is named by a behavior sentence (orchestrator
  ruling, 2026-10-18). [style 2](/docs/style.md#rule-test-comments),
  [style 2](/docs/style.md#rule-ported-test-names)
- Until we ship a red CI is not a problem; large-tier failures are fixed forward
  (russ, 2026-10-17). [process](/docs/plan/process.md#rule-red-ci-until-ship)
- Review is the bottleneck, so tests are placed in CI waves by what they catch
  before review (russ, 2026-10-18).
  [process](/docs/plan/process.md#rule-review-is-bottleneck)
