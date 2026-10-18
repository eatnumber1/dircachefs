# Index of protocol rules

Read this before you touch the write-through protocol, the dirty set, recovery,
cancellation, concurrency or `formal/`: the tri-state rule, the TLA+ model and
the rules that keep coroutines possible. Each line is a ratified rule, in russ's
words with the date where there is one, and links to the section that holds its
reasoning. `AGENTS.md` says which indexes to read for which task.

- **Tri-state records.** Every database record that mirrors something on the
  backing filesystem is present, absent or unknown, and a mutation sets it
  unknown before the backing syscall and present/absent after.
  [style 1.8](../style.md#rule-tri-state-records)
- **The protocol has a model.** A change to the write-through protocol (mutation
  phases, fills and their guards, completeness, the dirty set, sync points,
  recovery) updates the TLA+ model in `formal/` in the same change, and `bazel
  test //formal/...` passes (see `formal/README.md`, "Changing the model"). A
  protocol bug the model can express gets a `formal/known_bugs/` variant whose
  test expects its counterexample. Fixing a gap listed in `formal/findings/`
  moves its configuration into the real model. From phase 12.2, trace validation
  fails if the code does something the model does not allow.
  [style 1.8](../style.md#rule-protocol-change-updates-model),
  [design](../design.md#rule-design-protocol-model),
  [style 6](../style.md#rule-tla-conventions)
- **Requests are cancellable** (from phase 22): any operation that can take more
  than about 100 ms on a slow backing checks for interruption at interruption
  points (`dcfs/interruption_point.h`), each just before a backing syscall and
  never between a mutation's syscall and its phase 3, and has a cancellation
  test; cancellation never breaks the tri-state rule.
  [style 1.8](../style.md#rule-requests-cancellable),
  [design](../design.md#rule-design-cancellation)
- **No interim compromises toward the coroutine design** (russ, 2026-09-27):
  simple, synchronous, single-threaded, reviewable in small chunks; the rules
  that hold now are listed in [design](../design.md#rule-coroutine-ready-rules);
  [design](../design.md#rule-no-interim-compromises)
- **Runtime invariant checks**: the rules the design states in prose are also
  checked while tests run. [design](../design.md#rule-runtime-invariant-checks)
