# Phase 19 — Soak test (manual, last automated phase)

A long-running test, slower than the slow tier and never run by CI
(GitHub's job time limit is too short): fsstress plus a mixed workload for
hours on each backing filesystem, sampling dcfs's memory and the cache
checker periodically; fails on a crash, a checker mismatch or memory
growth beyond a bound. Bazel `size = "enormous"`, `timeout = "eternal"`
plus the `manual` tag, so `//...` never includes it; run explicitly
(`bazel test //test/qemu:soak_test --test_timeout=<hours>`).
Order: the last automated phase (before the final real-hardware check,
Phase 21.2).
