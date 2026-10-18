# Test Tier and Pre-Review Bug-Catching Analysis

Drafted by Gemini Flash (2026-10-18, test-placement analysis); orchestrator-reviewed.

## Summary Answers
- **(a) Wave 3 timing and bug-catching**: Starting Wave 3 after `fast` instead of `presubmit` in CI loses **zero** bug-catching before review, because reviews occur *before* code is pushed to CI. Furthermore, across 28 CI runs, Wave 3 has caught **zero real dcfs production bugs** (only test harness bugs, VM memory exhaustion, and load flakes). However, [`mount_dcfs_systemd_test`](file:///home/russ/Sources/dircachefs/test/qemu/BUILD.bazel) (measured at ~31 s) should become `medium` so implementers run it before review.
- **(b) Sanitizer runs of idle / pjdfstest / bench**: Sanitizer runs (ASan/UBSan) of `idle_long_test`, `pjdfstest`, and `bench_full_test` caught **zero real bugs** across the entire project history. They consume ~150 busy minutes per CI run and have only ever triggered guest OOMs or timeouts.
- **(c) Tier movements**: Move [`mount_dcfs_systemd_test`](file:///home/russ/Sources/dircachefs/test/qemu/BUILD.bazel) earlier (`large` $\to$ `medium`); move or split [`mount_dcfs_test`](file:///home/russ/Sources/dircachefs/test/qemu/BUILD.bazel) later (`small` $\to$ `medium`); move sanitizer runs of `idle_long`, `pjdfstest`, and `bench_full` to the weekly workflow.

---

## Evidence

### 1. Review Findings Classification ([review-findings.tsv](file:///home/russ/Sources/dircachefs/docs/plan/review-findings.tsv))
Analysis of all 62 `blocker` and `should` review findings:
- **37 (60%) Human/AI reader findings**: Style, docs, naming, dead code, comment accuracy, and contract claims (e.g. L11, L19, L30, L33, L40, L43, L74, L119, L121). No test could catch these without dedicated linters.
- **10 (16%) Test gaps / missing assertions**: Implementer tests passing vacuously or missing error paths (L2, L3, L4, L6, L7, L16, L20, L42, L71, L118). None were caught by late tests; they represent gaps that branch coverage gates (L16, L20, L71) or mutation testing (L42) catch before review.
- **7 (11%) Correctness edge cases**: Found via code analysis (L8, L9, L56, L59, L60, L101, L102).
- **6 (10%) TLA+ model discrepancies**: Specification vs implementation alignment (L21, L23, L25, L48, L57, L58).
- **2 (3%) Status handling**: Ignored statuses (L5, L75).
- **0 findings** were caught by late CI tests that `fast`/`presubmit` missed.

### 2. CI History & Wave 3 Failures ([log.md](file:///home/russ/Sources/dircachefs/docs/plan/log.md), [ci.yml](file:///home/russ/Sources/dircachefs/.github/workflows/ci.yml))
Across 28 CI runs (23 red), Wave 3 failures were entirely test harness defects or VM environment limits:
- **Run 39046531637** (commit [3441f89b](file:///home/russ/Sources/dircachefs/)): ASan guest initramfs unpacking ENOSPC due to 384 MiB floor after adding SibylFS. Fixed by commit [233baa34](file:///home/russ/Sources/dircachefs/) (`E2E_ASAN_MEM = 512`). Zero dcfs bugs.
- **Run 39000396656** (commit [58a1f172](file:///home/russ/Sources/dircachefs/)): `mount_dcfs_systemd_test` and `xfstests_*` failed because commit [95dafad](file:///home/russ/Sources/dircachefs/) removed native capture and made SOURCE a directory, breaking guest test setup. Fixed by commits [8184e510](file:///home/russ/Sources/dircachefs/) and [2abffe7a](file:///home/russ/Sources/dircachefs/) (fixture fixes).
- **Run 38018425705** (commit [f6161819](file:///home/russ/Sources/dircachefs/)): `nfs_test` (ESTALE), `destroy_test` (syntax error), and `fault_ace_fs_test_xfs` (cut race). Step 26.20 confirmed all 3 were test harness bugs; fixed in commit [9fd3ff2](file:///home/russ/Sources/dircachefs/).
- **Run 37582181373** (commit [b41cd175](file:///home/russ/Sources/dircachefs/)): `destroy_test` failed in plain tier due to guest kernel slab reclaim under low memory. Fixed in commit [a5f3227](file:///home/russ/Sources/dircachefs/) (`mem = 1536`).
- **Run 37853130640** (commit [9a675522](file:///home/russ/Sources/dircachefs/)): `bench_full_test` ASan guest OOM at 3072 MiB. Fixed in commit [68bab9be](file:///home/russ/Sources/dircachefs/) (`asan_mem = 4352`).

### 3. Sanitizer Execution Waste ([time-analysis-2026-10-18.md](file:///home/russ/Sources/dircachefs/docs/plan/notes/time-analysis-2026-10-18.md))
- **`idle_long_test`**: Sleeps for 600 s. Plain 617 s, ASan 621 s, UBSan 646 s. Zero CPU activity; sanitizers check nothing during sleep. Wastes 31 runner-minutes per CI run.
- **`pjdfstest_*`**: POSIX conformance suite. Plain 126–422 s, ASan 163–464 s, UBSan 102–230 s per shard. Consumes 97 busy minutes across sanitizers per CI run without ever detecting a memory safety bug.
- **`bench_full_test`**: Microbenchmarks. Plain 303 s, ASan 469 s, UBSan 266 s. Consumes 17 busy minutes per CI run; ASan only provoked guest OOMs.

---

## Ranked Recommendations

1. **Move [`mount_dcfs_systemd_test`](file:///home/russ/Sources/dircachefs/test/qemu/BUILD.bazel) from `large` to `medium`**
   - *Duration*: 31 s (measured in [26-bumpers.md:968](file:///home/russ/Sources/dircachefs/docs/plan/phases/26-bumpers.md#L968)).
   - *Evidence*: Failed in Run 39000396656 after commit [95dafad](file:///home/russ/Sources/dircachefs/) broke systemd mount options unnoticed. Moving it to `medium` makes it run during `--config=presubmit`, catching systemd regressions before review.
2. **Move ASan and UBSan runs of `idle_long`, `pjdfstest`, and `bench_full` to weekly** (or drop sanitizer coverage on them)
   - *Savings*: ~150 busy minutes (and ~10 min wall on critical shards) per CI run.
   - *Evidence*: Zero bugs caught in project history; only artificial guest OOMs ([68bab9be](file:///home/russ/Sources/dircachefs/)).
3. **Move or split [`mount_dcfs_test`](file:///home/russ/Sources/dircachefs/test/qemu/BUILD.bazel) out of `small`**
   - *Duration*: 76 s plain, 172 s ASan ([time-analysis row 29](file:///home/russ/Sources/dircachefs/docs/plan/notes/time-analysis-2026-10-18.md#L196)).
   - *Evidence*: Accounts for 76 s of the 79 s critical path in `fast`. Split into separate unit targets or move slow dm-delay scenarios (`SLOW_WRITE_MS=2000`) to `medium`.
4. **Enforce diff coverage locally before code review (Phase 25.39)**
   - *Evidence*: 10 blocker/should review findings (e.g. L16, L20, L42, L71) were uncovered branches or lines that implementers missed. Running diff-coverage pre-review catches them without Opus tokens.
5. **Start CI Wave 3 after `fast` (`needs: fast`)**
   - *Savings*: ~10.2 min of CI wall time per run ([time-analysis Section 1.1](file:///home/russ/Sources/dircachefs/docs/plan/notes/time-analysis-2026-10-18.md#L38)). Zero risk to pre-review bug-catching.

---

## What the Data Could Not Show
1. **Defects on code paths governed by unit tests**: Because AGENTS.md enforces "test first" with unit tests in `fast`/`presubmit`, the data cannot demonstrate whether Wave 3 would have caught bugs on untested features.
2. **Fine-grained review token breakdown**: `review-findings.tsv` records findings and severity, but not the exact token spend per finding category.
3. **Clean TLC execution under zero load**: All TLC benchmark data in `time-analysis-2026-10-18.md` was gathered under local host contention (load 23–72).
