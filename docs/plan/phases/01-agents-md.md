# Phase 1 — AGENTS.md

**Decision (russ, 2026-10-05).** A checked-in `AGENTS.md` (not
Claude-specific) holding the project's working rules for any agent or
contributor: test first (a failing test before every fix); fakes, not
mocks (`*_test.cc`, `testonly/` with `testonly = 1`); all tests in QEMU, no
host-side tests; root required, no unprivileged fallbacks; test sizes as
tiers and which to run when; `-Weverything -Werror`; `third_party/`
convention; pinned tools and how to update them; running Bazel under
`sg kvm` (and `bazel shutdown` first) so KVM is used; never pipe Bazel
through `tail` in `&&` chains (it hides the exit code); where the plan
lives (`docs/plan/README.md`, one file per phase in `docs/plan/phases/`) and that design decisions go in
`docs/design.md`; commit message conventions.
Status: done 2026-10-05: drafted by the orchestrator (`/AGENTS.md`),
reviewed and approved by russ. Order: first, with Phase 2 (Fix: world-readable cache database) (agents
working on later phases read it).
