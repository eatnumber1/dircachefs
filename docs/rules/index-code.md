# Index of code rules

Read this before you change code under `dcfs/`, `bench/` or `tools/`: the design
principles (behave like the backing, bounded memory, root), syscalls, names as
bytes, crashes, timers, retries and branches. Each line is a ratified rule, in
russ's words, or marked as an orchestrator ruling, with the date where there is
one and a link to the section that holds its reasoning. `AGENTS.md` says which
indexes to read for which task.

- Behave like the backing, only faster: the same results, incremental and
  interruptible where the backing is (russ, 2026-10-17).
  [design](/docs/design.md#rule-behave-like-backing)
- Each FUSE operation keeps the contract the backing syscall gives its caller
  (russ, 2026-10-10).
  [design](/docs/design.md#rule-fuse-op-keeps-backing-contract)
- Where the contract allows several answers, give the most useful one (russ,
  2026-10-17: "When there's ambiguity, you should be choosing the most _useful_
  result."). [design](/docs/design.md#rule-most-useful-result)
- Bounded memory: a request's working memory is O(1) in the size of what it
  touches. [design](/docs/design.md#rule-bounded-memory)
- Root is required; there are no unprivileged fallbacks.
  [design](/docs/design.md#rule-root-required)
- Test against the minimum supported kernel, and keep our kernel headers
  matching the tested kernel (russ, 2026-10-18).
  [design](/docs/design.md#rule-test-minimum-kernel)
- Pin the test kernel by major.minor and let the patch version float; no
  from-source kernel build (russ, 2026-10-18).
  [design](/docs/design.md#rule-kernel-pin-major-minor)
- No backward compatibility until the first deployment (russ, 2026-10-10/11:
  "we've never shipped dcfs"). [design](/docs/design.md#rule-no-backcompat)
- File names are bytes; escape them whenever printed (from phase 9).
  [style 1.8](/docs/style.md#rule-names-are-bytes),
  [design](/docs/design.md#rule-design-names-are-bytes)
- Descriptors and handles, never paths, after startup.
  [style 1.8](/docs/style.md#rule-no-paths-after-startup),
  [design](/docs/design.md#rule-fds-never-paths)
- `Context &ctx` first, no globals or `thread_local`, no database transaction
  across a backing syscall. [style 1.8](/docs/style.md#rule-context-first),
  [style 1.8](/docs/style.md#rule-no-globals),
  [style 1.8](/docs/style.md#rule-no-txn-across-syscall)
- No raw syscalls anywhere (russ, 2026-10-07: a firm rule); one thin wrapper per
  syscall, named for its manpage.
  [style 1.5](/docs/style.md#rule-no-raw-syscalls),
  [style 1.5](/docs/style.md#rule-syscall-wrappers)
- Prefer pidfds over plain pids (russ, 2026-10-18).
  [style 1.5](/docs/style.md#rule-pidfds-over-pids)
- Syscall fault injection stays link-time (russ, 2026-10-16: "I agree. Revert
  it."). [design](/docs/design.md#rule-link-time-fault-injection)
- No intentional crashes: no `CHECK`, `LOG(FATAL)` or `abort()` in production
  (russ, 2026-10-09). [style 1.6b](/docs/style.md#rule-no-intentional-crashes)
- No timers in the codebase (russ, 2026-10-09: "Timers are inherently
  brittle."): wait on the event. [style 1.11](/docs/style.md#rule-no-timers)
- While working anything goes, but what is submitted obeys the rules (russ,
  2026-10-11). [style 1.11](/docs/style.md#rule-anything-goes-while-working)
- Host-side tools get no blanket exemption from the timer rule (orchestrator
  ruling, 2026-10-16). [style 1.11](/docs/style.md#rule-timers-host-tools)
- No retry loops (russ, 2026-10-10: "if you think one is necessary, come to
  me"). [style 1.12](/docs/style.md#rule-no-retry-loops)
- Every branch names its reachable cause (russ, 2026-10-10).
  [style 1.10a](/docs/style.md#rule-reachable-cause)
- One way to do each thing (russ, 2026-10-10).
  [style 1.10](/docs/style.md#rule-one-way-to-do-each-thing)
- One abstraction over variants; branching is discouraged, not forbidden (russ,
  2026-10-08). [style 1.10](/docs/style.md#rule-one-abstraction-over-variants),
  [style 1.10](/docs/style.md#rule-branching-discouraged)
- Rely on library defaults and test the ones we rely on (russ, 2026-10-18).
  [style 1.13](/docs/style.md#rule-rely-on-defaults)
- FUSE-only machinery is a fallback with an expiry (russ, 2026-10-11).
  [design](/docs/design.md#rule-fuse-only-fallback)
- No interim compromises toward the coroutine design (russ, 2026-09-27).
  [design](/docs/design.md#rule-no-interim-compromises)
- Mechanize every rule you can, and move logic to compile time (russ, 2026-10-09
  and 2026-10-17). [style 1.1](/docs/style.md#rule-mechanize-rules),
  [style 1.1](/docs/style.md#rule-compile-time-logic)
