# Index of protocol rules

Read this before you touch the write-through protocol, identity, the dirty set,
recovery, cancellation, concurrency or `formal/`. Each line is a ratified rule,
in russ's words, or marked as an orchestrator ruling, with the date where there
is one and a link to the section that holds its reasoning. `AGENTS.md` says
which indexes to read for which task.

- Tri-state records: present, absent or unknown, set unknown before the backing
  syscall. [style 1.8](/docs/style.md#rule-tri-state-records)
- A protocol change updates the TLA+ model in the same change.
  [style 1.8](/docs/style.md#rule-protocol-change-updates-model),
  [design](/docs/design.md#rule-design-protocol-model),
  [style 6](/docs/style.md#rule-tla-conventions)
- Requests are cancellable at interruption points (from phase 22).
  [style 1.8](/docs/style.md#rule-requests-cancellable),
  [design](/docs/design.md#rule-design-cancellation)
- Rules that hold now so that coroutines need no redesign.
  [design](/docs/design.md#rule-coroutine-ready-rules)
- Runtime invariant checks run while tests run.
  [design](/docs/design.md#rule-runtime-invariant-checks)
- Identity comes from the backing filesystem, and a row matches a backing object
  only if device, inode, generation, handle and birth time agree.
  [design](/docs/design.md#rule-identity-model),
  [design](/docs/design.md#rule-identity-match)
- Every mutation is three phases: mark unknown durably, make the backing
  syscall, record the result.
  [design](/docs/design.md#rule-write-through-phases)
- A daemon crash leaves the database at its last commit; a power loss leaves a
  prefix, and recovery re-probes what the dirty set names.
  [design](/docs/design.md#rule-daemon-crash-safe),
  [design](/docs/design.md#rule-power-loss-recovery)
- FSYNC and FSYNCDIR are a file's sync points and run no syncfs; the periodic
  sync point runs syncfs (russ, 2026-10-18).
  [design](/docs/design.md#rule-sync-points)
