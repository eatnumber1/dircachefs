# Index of code rules

Read this before you change code under `dcfs/`, `bench/` or `tools/`: the design
principles (behave like the backing, bounded memory, root), syscalls, names as
bytes, crashes, timers, retries and branches. Each line is a ratified rule, in
russ's words with the date where there is one, and links to the section that
holds its reasoning. `AGENTS.md` says which indexes to read for which task.

- **Root is required.** dcfs runs as root; there are no unprivileged fallbacks.
  [design, Assumptions](../design.md#rule-root-required)
- **Behave like the backing, only faster.** With dcfs out of the loop, what does
  the backing do for this operation, over time? dcfs does that
  (`docs/design.md`, "Behave like the backing, only faster"): the same results,
  incremental where it is, interruptible where it is, no stronger and no weaker
  guarantees. A claim about what the backing, the kernel or POSIX promises cites
  its source (POSIX text, a man page, or the kernel code path) where it is
  written, and where it can be measured, a native differential test (the same
  operations with dcfs out of the loop) backs it. russ, 2026-10-17.
  [design](../design.md#rule-behave-like-backing)
- **Where the backing's contract allows several answers, give the most useful
  one** (russ, 2026-10-17: "When there's ambiguity, you should be choosing the
  most _useful_ result."). [style 1.12](../style.md#rule-most-useful-result)
- **Each FUSE operation keeps the contract the backing syscall gives its
  caller** (russ, 2026-10-10), races included.
  [style 1.12](../style.md#rule-fuse-op-keeps-backing-contract)
- **Bounded memory.** A request's working memory is O(1) in the size of the
  files and directories it touches; the cache database grows with the
  filesystem, the working memory does not (`docs/design.md`, "Bounded memory").
  Any exception is a recorded decision with its reason.
  [design](../design.md#rule-bounded-memory)
- **File names are bytes.** Never treat a name, symlink target or xattr name as
  text; escape it whenever it is printed (from phase 9).
  [style 1.8](../style.md#rule-names-are-bytes),
  [design](../design.md#rule-design-names-are-bytes)
- **Descriptors and handles, never paths**, after startup.
  [style 1.8](../style.md#rule-no-paths-after-startup),
  [design](../design.md#rule-fds-never-paths)
- **`Context &ctx` first** in every function that reads or writes the cache or
  backing; **no globals, singletons or `thread_local`**; **no database
  transaction spans a backing syscall**.
  [style 1.8](../style.md#rule-context-first),
  [style 1.8](../style.md#rule-no-globals),
  [style 1.8](../style.md#rule-no-txn-across-syscall)
- **No raw syscalls anywhere** (russ, 2026-10-07: a firm rule): syscalls go
  through `dcfs/syscalls*.h`, tests included.
  [style 1.5](../style.md#rule-no-raw-syscalls); one thin wrapper per syscall,
  named for its manpage: [style 1.5](../style.md#rule-syscall-wrappers);
  **prefer pidfds over plain pids** (russ, 2026-10-18):
  [style 1.5](../style.md#rule-pidfds-over-pids)
- **Syscall fault injection stays link-time** (russ, 2026-10-16: "I agree.
  Revert it."). [design](../design.md#rule-link-time-fault-injection)
- **No intentional crashes** (russ, 2026-10-09): no `CHECK`, `LOG(FATAL)`,
  `abort()` in production; `RET_CHECK` is fine; a crash needs russ's
  case-by-case approval. [style 1.6b](../style.md#rule-no-intentional-crashes)
- **No timers in the codebase** (russ, 2026-10-09: "Timers are inherently
  brittle. If things are slow, the system breaks."): wait on the event; a
  timeout only where the awaited thing cannot signal.
  [style 1.11](../style.md#rule-no-timers); while working, anything goes, but
  what is submitted obeys the rules (russ, 2026-10-11):
  [style 1.11](../style.md#rule-anything-goes-while-working); host-side tools
  get no blanket exemption: [style 1.11](../style.md#rule-timers-host-tools)
- **No retry loops** (russ, 2026-10-10: "if you think one is necessary, come to
  me"). [style 1.12](../style.md#rule-no-retry-loops)
- **Every branch names its reachable cause** (russ, 2026-10-10): "writing
  unnecessary code causes unnecessary cognitive overhead"; no code for the
  impossible, a surviving mutant blocks the merge.
  [style 1.10a](../style.md#rule-reachable-cause)
- **One abstraction over variants; branching is discouraged, not forbidden**
  (russ, 2026-10-08).
  [style 1.10](../style.md#rule-one-abstraction-over-variants),
  [style 1.10](../style.md#rule-branching-discouraged)
- **No backward compatibility until the first deployment** (russ, 2026-10-10/11:
  "we've never shipped dcfs"): a removed option, format or behaviour is simply
  gone; no migration of the cache schema.
  [process](../plan/process.md#rule-no-backcompat)
- **FUSE-only machinery is a fallback with an expiry** (russ, 2026-10-11):
  workaround first, kernel patch after everything else, the workaround written
  to be deleted. [design](../design.md#rule-fuse-only-fallback)
