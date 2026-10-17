# Phase 22 — Request cancellation (Ctrl+C, EINTR)

**Decision (russ, 2026-10-06).** dcfs supports cancelling requests: a
process interrupted while waiting on dcfs (Ctrl+C, a signal, a timeout)
gets EINTR promptly, and any slow operation dcfs was doing for it stops.
Cancellation is a stated goal even if today's operations turn out to be
fast: future slow operations must support it, and the design must carry
over to the coroutine + io_uring future.

**Mechanism.** The kernel sends FUSE_INTERRUPT for a request whose caller
got a signal. libfuse exposes it as `fuse_req_interrupt_func()` (a callback
when the request is interrupted) and `fuse_req_interrupted()` (poll). A
request answered after interruption should reply EINTR (or complete, if it
already changed state that cannot be undone).

## 22.1 Inventory (measure, decide)

- Measure the worst case of every request path on a slow backing
  (dm-delay from Phase 10 plus a spun-down-disk-like latency, cold cache,
  large directories): lookup/getattr on a miss, population of a 100k-entry
  directory (readdir + statx per entry + SQLite), readdirplus of a huge
  cached directory, create/unlink/rename (three phases, backing syscall,
  durable commits), fsync/fsyncdir and the sync point's syncfs after large
  writes, startup recovery of a large dirty set, open of a large file
  (passthrough setup). Reads and writes themselves are kernel passthrough
  (cancellable by the kernel, not dcfs).
- Classify: anything that can exceed about 100 ms on a slow disk is
  "slow" and must be cancellable at safe points; the rest is "fast" and
  needs no checks. Expected slow set: directory population, large
  readdirplus fills, recovery; and syscalls that block in the kernel
  (syncfs, fsync, a cold open), which dcfs cannot interrupt but should not
  start for an already-interrupted request.
- If the measurement shows everything is fast, say so with numbers; the
  rest of the phase still delivers the policy, the tests of the
  interruption path, and the docs.

## 22.2 Semantics (dcfs-protocol decides, with the model)

- Safe points: between entries of a population loop, between SQLite
  batches, before each backing syscall. At a safe point an interrupted
  request stops and replies EINTR.
- The tri-state rule holds through cancellation: an aborted population
  leaves the directory incomplete with the entries probed so far present
  (sound); an aborted fill must release its fill guard; a mutation is
  NOT cancellable between its backing syscall and phase 3 (the backing
  change exists; finish and reply success), but is cancellable before the
  syscall (undo phase 1: the name returns to unknown, which is sound).
- Single-threaded today: libfuse delivers FUSE_INTERRUPT as its own
  request, read by the loop only after the current request's handler
  returns, so a long handler never sees the interrupt unless the loop
  processes incoming requests at safe points. Decide: (a) call
  `fuse_session_process_buf`-style draining at safe points, or (b) accept
  that today only the "already interrupted on arrival" case is handled
  and make the safe points real with coroutines. The decision and its
  reason go in docs/design.md; the model gets an `Interrupt` action for
  whichever is chosen (cancellation never violates CacheNeverWrong or
  TriState).
- Coroutines + io_uring: a per-request cancellation token checked at
  every await; `IORING_OP_ASYNC_CANCEL` for in-flight backing I/O; the
  same safe-point rules. Written down now so the rewrite inherits it.

## 22.3 Tests first

- Unit (`dir_cache_fs_test`, forged requests): a request marked
  interrupted before dispatch replies EINTR without touching the backing
  (fake backing records calls); a population interrupted at a safe point
  (the `--wrap` hook raises the interrupt mid-loop) stops, leaves the
  directory incomplete and consistent (cache checker), releases the fill
  guard, and the next request completes the population; a mutation
  interrupted after its syscall still completes and replies success; one
  interrupted before its syscall leaves the name unknown and the backing
  untouched.
- Guest: on a dm-delay backing with a 100k-entry cold directory,
  `timeout -s INT 1 ls /mnt/huge` exits promptly (within 2 s) with EINTR
  visible to `ls`, dcfs keeps serving other requests during and after,
  and the checker passes; the same with Ctrl+C semantics via `kill -INT`
  on a blocked `find`. Failing first on today's dcfs (it should hang until
  the population finishes).
- Benchmarks: the inventory's numbers become a `bench_full` table so a
  later slow operation is noticed.

## 22.4 Docs and rules

- docs/design.md: "Cancellation" section (goal, semantics, safe points,
  the coroutine plan); README: EINTR behaviour.
- AGENTS.md: any operation that can take more than ~100 ms on a slow
  backing checks for interruption at safe points and has a cancellation
  test.

Owner: dcfs-protocol (22.2, 22.3's unit tests), dcfs-investigator (22.1),
dcfs-implementer (guest tests, docs). Order: after R4 merges (it touches
the same request paths), before Phase 13.

## Note from the readdir step (2026-10-08)

Since the readdir reductions, children's attributes are read in the
listing statement (`ListDir`'s join), not per entry when it is served.
Under checkpoints this changes nothing (no interleaving inside a request
today); under coroutines, a mutation interleaving during an earlier
entry's refresh no longer makes a later entry's attributes unknown before
they are served; the kernel discards readdirplus attributes older than a
completed modifying op on the same mount (attr_version), so this is safe,
but the coroutine design should re-check it. See docs/design.md "Readdir".

## As built (2026-10-08, step-22, pending its review fixes)

Option (b): synchronous checkpoints, single-threaded. Deviations from
the text above: an interrupted population commits nothing (22.2 said
"probed entries present"): the directory stays incomplete and the next
readdir completes it; the guest test uses a 20,000-entry directory (not
100k); the inventory (notes/cancellation-inventory-2026-10-08.md) is its
own enormous test, not a bench_full table. Review (merge after fixes):
a refused INIT must exit non-zero (SessionLoop cannot see libfuse's
private se->error: detect FUSE_INIT + fuse_session_exited); rename's
double re-resolve stops at the first EINTR; Drain stops after the first
non-INTERRUPT message so queued requests stay kernel-cancellable;
FUSE_CAP_SPLICE_READ unset; a parameterized cancellation test over every
checkpoint; cancel_test's bound scaled by the accelerator; AGENTS.md
wording: "checks for interruption at checkpoints (dcfs/checkpoint.h),
each just before a backing syscall and never between a mutation's
syscall and its phase 3, and has a cancellation test".

Merged 2026-10-08 (87b35a0, 7 commits, +2070/-32) after the review fixes:
a refused INIT exits -EPROTO; re-resolution stops at the first EINTR;
Drain reads one message ahead at most, FUSE_CAP_SPLICE_READ unset; a
parameterized cancellation test over all twelve checkpoints under the
invariant checker; cancel_test bounds 2 s KVM / 8 s TCG (0.8 s measured;
12.1 s with checkpoints off); large_test 7,238,097 states and nolock_test
6,036,816 unchanged (1508 s / 1272 s alone under load 20). AGENTS.md's
rule now reads "at checkpoints (dcfs/checkpoint.h), each just before a
backing syscall and never between a mutation's syscall and its phase 3".

## 22.5 Timely interrupts, in the model (russ, 2026-10-17)

russ: "Let's use the formal model to additionally provide some guarantees
around 'interrupt in a timely manner'. Not necessarily wall clock, but
maybe 'interrupt between every step' or something like that."

Today `Interrupt(p)` is an action that may happen at a checkpoint pc, so
the model proves an interrupt is safe but not that it is acted on: a dcfs
that ignored interrupts forever passes. This step makes acting on one an
obligation and bounds how much work follows it.

- **Arrival, separate from acting on it.** `InterruptArrives(p)` marks a
  request (in flight, or queued before dispatch) interrupted at any step.
  At a checkpoint pc an interrupted request's only enabled step is
  `Interrupt(p)`: the checkpoint checks, as `Checkpoint` in
  `dcfs/checkpoint.h` does. Between a syscall and its phase 3 it
  proceeds, as now.
- **The bound, a safety property counted in work, not time.** After the
  interrupt arrives, a request takes at most K more backing syscalls before
  it replies, where K is the longest run of backing syscalls with no
  checkpoint between them (the syscall already past its checkpoint, plus
  the post-syscall reads before phase 3, e.g. a create's probe). Find K
  from the model, name every pc in that run, and state the property with K
  written out. If the honest answer is "unbounded somewhere" (a loop of
  backing calls with no checkpoint), that is a finding: report it rather
  than bending the property.
- **Liveness.** An interrupted request eventually replies (`~>`), under
  the weak fairness of the request's own steps that the code provides.
- **What the bound does not count.** Work with no backing syscall: cache
  queries and commits, the kernel-blocked syscall itself (README future
  work, coroutines over io_uring). Say so in the model's header and in
  `docs/design.md` "Cancellation".
- **Known bugs.** A `known_bugs/` variant with one checkpoint removed (say,
  in readdir's probe loop, or a rename's second name) must violate the
  bound, and one where the checkpoint ignores the arrival must violate
  liveness.
- **The code keeps it.** Trace validation (Phase 12) must refuse a trace
  in which the code takes a backing syscall at a checkpoint after the
  interrupt was visible there. Propose, and if cheap build, a mechanical
  check that every backing call reachable from a FUSE handler is preceded
  by a checkpoint or sits in the named post-syscall run (the model's K),
  in the style of 25.9's backing-door matcher.
- Keep the state space affordable: only the `MC_interrupt*` configs turn
  arrival on; `MC_large` and `MC_nolock` state counts stay unchanged.

Owner: dcfs-protocol. Docs: `formal/README.md` (actions and properties
tables), `docs/design.md` "Cancellation", README's EINTR paragraph if
the guarantee is user-visible.
