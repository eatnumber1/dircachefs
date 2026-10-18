# Index of test rules

Read this before you write, change or run a test, or review one: what runs in
QEMU guests, test tiers, load-agnostic verdicts, coverage and fakes. Each line
is a ratified rule, in russ's words with the date where there is one, and links
to the section that holds its reasoning. `AGENTS.md` says which indexes to read
for which task.

- **Test first.** Every bug fix and every behavior change starts with a test
  that fails on the current code. Show the failure (quote it in the commit
  message or the review), then fix. [style 2](../style.md#rule-test-first),
  [design](../design.md#rule-design-test-first)
- **Tests that need root or control over the kernel run in QEMU guests**, and
  that is every test of dcfs itself, unit tests included: no code paths exist
  only so such a test can run outside the guest. The guests boot a minimal
  kernel in about a second. Checks that need neither (clang-tidy, formatting,
  model checking a specification) may run on the host.
  [style 2](../style.md#rule-tests-in-qemu),
  [design](../design.md#rule-design-everything-in-qemu)
- **No new shell for guest tests** (from step 25.34): a new test that mounts
  dcfs, crashes it or compares trees is a `qemu_cc_test` in `test/qemu/guest/`
  using the helpers of `guest.h`; shell under `test/qemu/` is thin glue only,
  and `tools/repo_shape.py` refuses a new `*.sh` there that
  `tools/repo_shape_guest_shell.txt` does not list.
  [style 4](../style.md#rule-new-guest-script-thin-glue)
- **Fakes, not mocks.** Inject dependencies and use small fake implementations.
  Test-only code never lives in production files: it goes in `*_test.cc`, or in
  a `testonly/` directory whose BUILD targets set `testonly = 1`. Syscall fault
  injection uses link-time `-Wl,--wrap` fakes.
  [style 2](../style.md#rule-fakes-not-mocks)
- **No test-only things in production code** (russ, 2026-10-09): "If a test
  needs a knob in the prod code, make that knob a real feature of the class /
  method. There should be zero `// only use in tests` style comments." A seam a
  test needs is designed and documented as a feature with its production
  default. [style 8](../style.md#rule-no-test-only-things); no test peers (russ,
  2026-10-09): [style 8](../style.md#rule-no-test-peers)
- **Coverage** (from phase 7/8): new code arrives fully covered by tests, error
  paths included. CI fails if total coverage drops. Gaps that cannot be covered
  are listed with a reason in `docs/coverage.md`. You must not lower the
  coverage baseline. That's why it exists. Only ever increase it (russ,
  2026-10-11). [style 2](../style.md#rule-never-lower-coverage),
  [process](../plan/process.md#rule-coverage-baseline)
- **Test sizes are tiers**: Bazel `size` is the tier, and every test declares
  `size` and `timeout` explicitly (the QEMU test macros refuse a missing one).
  `small` tests run constantly during development (`bazel test --config=fast
  //...`), `medium` before submitting (`--config=presubmit`), `large`/`enormous`
  in CI (plain `bazel test //...`). The soak test is tagged `manual` and only
  runs when asked for by name. See `test/qemu/README.md` "Test tiers".
  [style 2](../style.md#rule-test-tiers)
- **A test's verdict does not depend on host load or speed** (russ, 2026-10-17:
  "in general for all our tests I'm interested in them being load-agnostic"):
  count events and wait on events; time is only a hang guard sized by the
  harness. [style 2](../style.md#rule-load-agnostic-tests)
- **Shard with Bazel's `shard_count`** (russ, 2026-10-18, step 5.12).
  [style 2](../style.md#rule-shard-count)
- **The kernel-facts pins test the kernel, not dcfs** (russ, 2026-10-18): they
  run only in the weekly workflow.
  [style 2](../style.md#rule-kernel-facts-weekly)
- Never weaken a test to make it faster or to make it pass.
  [style 2](../style.md#rule-test-first)
- **Test conventions**: googletest fixtures ([style
  2](../style.md#rule-googletest)), extend an existing test or write a new one
  by the cost model ([style 2](../style.md#rule-extend-or-new-test)), matchers
  over booleans ([style 2](../style.md#rule-matchers-over-booleans)), a test
  helper does not reuse a production struct ([style
  2](../style.md#rule-test-helper-structs)), a test comment does not start with
  its step number (russ, 2026-10-17: [style 2](../style.md#rule-test-comments)),
  and a test ported from a script is named by a behavior sentence ([style
  2](../style.md#rule-ported-test-names)).
- **Tests may crash on what is not under test** (russ, 2026-10-09/11): setup
  failures use Abseil `CHECK`/`CHECK_OK`; googletest `ASSERT`/`EXPECT` are for
  the property under test. [style 1.6b](../style.md#rule-tests-may-crash)
- **Sanitizer runs do not block a step** (russ, 2026-10-06): a step's agent runs
  its own tests and the plain suite; the full ASan suite runs on the merged
  commit. [process](../plan/process.md#rule-sanitizers-do-not-block)
