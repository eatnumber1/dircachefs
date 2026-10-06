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
