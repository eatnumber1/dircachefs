# Cancellation: inventory and semantics (Phase 22.1/22.2, 2026-10-08)

From the dcfs-protocol agent's design round (branch step-22 in lane-2 holds
the measurement test `//test/qemu:cancel_inventory_test`, enormous tier:
ext4 on dm-delay at 10 ms per backing I/O, cache DB on a fast disk,
fastbuild, loaded host, two runs, +-40%).

## Inventory

| Request path | Wall time | Phase while blocked | Cache state | An INTERRUPT then should |
|---|---|---|---|---|
| Populate a 20,000-entry directory (first READDIR/LOOKUP) | 13.6-19.5 s (backing `ls -l` alone 3.4 s; ~0.7-1 ms/entry: 100k ~ 70-100 s) | fill: PopulateRead (snapshot, probe per entry) then PopulateCommit | dir incomplete, guard raised, nothing written until commit | stop at the next safe point between probe batches, no commit (or commit incomplete), release the guard, EINTR |
| Cold lookup 3 dirs deep | 210-310 ms | fill per dir | as above | safe point before each population |
| Cached READDIRPLUS + stat of 20k | 3.3-4.1 s over many ms-sized requests | none | complete | nothing: each request is fast |
| Cold open + read | 340-460 ms | open_by_handle_at + identity statx | row unchanged | before the open: EINTR; during: blocked in the kernel; after: finish |
| Create / rename / unlink | 210 / 50-120 / 50-110 ms | phase 1 (durable), phase 2 syscall, phase 3 | names unknown, dirty | before phase 2: end without the syscall (names stay unknown), EINTR; during/after: finish phase 3, reply success |
| FSYNC (+ sync point syncfs) after 32 MiB | 330-450 ms, scales with dirty data | sync point then ClearDirty | dirty set intact | before: EINTR; during: blocked in the kernel; after: finish |
| DESTROY reconcile + FinishRun (500 written) | 370-440 ms (100k: 4.8 s) | no caller | n/a | n/a (SIGTERM path) |
| Start-up after a crash, 500 dirty | 270-580 ms (clean 140-350) | before mount | n/a | n/a |
| First I/O after a spin-down | +5-10 s (reasoned; dm-delay cannot simulate) | wherever it lands | | blocked in the kernel: only a worker or async I/O returns early |

## Kernel and libfuse facts (fs/fuse/dev.c request_wait_answer)

- After the daemon has READ a request, a signal only sets FR_INTERRUPTED
  and queues FUSE_INTERRUPT; the caller keeps waiting, killable or not:
  even SIGKILL does not free it until the daemon replies. A 100 s
  population therefore makes `ls` unkillable today.
- FUSE_INTERRUPT is queued after the request was read and is read ahead
  of later requests; single-threaded dcfs reads it only after the handler
  returned, so libfuse finds no live request and answers it EAGAIN. dcfs
  effectively has option (a) today, "interrupted before dispatch" included.

## Options

- (a) Ignore INTERRUPT (today). Safe; long populations leave callers unkillable.
- (b) Honour it at safe points before backing syscalls, single-threaded:
  dcfs owns its loop (fuse_session_receive_buf/process_buf); at each safe
  point it polls /dev/fuse without blocking; an INTERRUPT goes to
  fuse_session_process_buf (libfuse's do_interrupt marks the running
  request; fuse_req_interrupted() then true; reentrant-safe: se->lock is
  not held during a handler); other messages are queued for after the
  handler. Cost: one poll() per safe point, no thread, no idle wakeups.
  Cannot cut a syscall already blocked (spin-up, syncfs, a hung network
  mount).
- (c) A worker thread (or io_uring) per blocking call: breaks the
  single-thread rule (26.8); io_uring has no op for getdents64,
  open_by_handle_at, name_to_handle_at, syncfs or most ioctls, so a pool
  is needed anyway; for mutations an early EINTR means "EINTR but done"
  (NFS intr semantics); the model's no-lock configuration already covers
  the early-reply interleaving.
- (d) Coroutines + io_uring + a fallback pool: every await a safe point,
  IORING_OP_ASYNC_CANCEL for in-flight I/O; the planned rewrite.

All four keep the tri-state rule given: an interrupted fill commits
nothing unguarded and releases its guard; a populate stops without commit
or commits incomplete; a mutation interrupted before its syscall ends with
its names unknown (the model's Failed paths, backing unchanged); a
mutation past its syscall always runs phase 3.

## Recommendation: (b) now, safe points written so (d) inherits them

~1-2 days: the loop; a per-request interrupted check through Context as
an injected interface with a fake; safe points in PopulateDirectory and
ResolveName and one before each phase-2 syscall; the model; the tests.
Population is the only in-dcfs path measurably slow (> 100 ms/request on
a slow backing) and (b) bounds its wait to one probe batch. Blocking
inside the kernel (spin-up, syncfs, network) needs (c)/(d): defer to the
coroutine rewrite, state the limit in design.md.

## 22.2 model changes (dcfs.tla)

Interrupt(p) enabled at every pc before a backing syscall: drops the
request, releases inflight, and for a mutation past phase 1 ends it
without phase 3 with the backing unchanged (names unknown, completeness
not restored); disabled from the syscall through phase 3. Invariants
unchanged plus "no guard leaks after an interrupt". Known bugs: interrupt
after the syscall skipping phase 3 (CacheNeverWrong); an interrupted
mutation restoring completeness (TriState). Trace: RequestEnd with EINTR
maps to T_Interrupt. lifetime.tla: an interrupted LOOKUP hands out nothing
(already holds: lookups_ counted after the reply).

## 22.3 test plan

Harness with a fake interruption source: interrupted before dispatch
(EINTR, no backing call); a population interrupted mid-loop (incomplete,
guard released, next readdir completes it); a mutation interrupted before
its syscall (EINTR, name unknown, backing untouched) and after (success);
one end-to-end harness test feeding a forged FUSE_INTERRUPT through the
custom io.read. Guest on the dm-delay 20k directory: `timeout -s INT 1 ls
/mnt/big` returns within 2 s (today 14-20 s: fails first); `kill -9` of a
blocked `find` returns promptly (today unkillable); a `stat` elsewhere is
served afterwards; a full `ls` completes; the checker passes. The
inventory numbers become a bench_full table.
