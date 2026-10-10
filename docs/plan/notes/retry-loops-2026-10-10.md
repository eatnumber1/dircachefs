# Retry loops in the tree (2026-10-10)

russ, 2026-10-10: "no retry loops. I understand they may sometimes be
necessary, but if you think one is necessary, come to me." Style guide
section 1.12. This is the inventory at the time the rule was made, for
russ's ruling on each; the list only shrinks. Found by grepping for
`retr`, `attempt` and `kAttempts` in `dcfs/` (production files),
`test/qemu/guest/` and `tools/`.

## Production (daemon)

| # | Site | Loop | Bound | What the event would be | Proposed |
|---|---|---|---|---|---|
| 1 | `dcfs/dir_cache_fs.cc:1125` (unlink/rmdir) | resolve the name again when phase 1 finds the parent or child changed since the resolve (`kAborted`) | 3 attempts, then `EAGAIN` "it kept changing" | the overlapping mutation's end (`TODO(coroutines)`); today nothing can run in between, so the loop never iterates | keep until coroutines, with the approval comment; or make it one attempt and `EAGAIN` at once, since today it cannot iterate |
| 2 | `dcfs/dir_cache_fs.cc:1255` (rename) | the same for both names | 3 attempts, then `EAGAIN` | same | same as 1 |
| 3 | `dcfs/dir_cache_fs.cc:2239` (readdir listing) | populate the directory again when the completeness check finds it incomplete after a populate | 3 attempts, then `EAGAIN` "kept changing while being listed" | the in-flight mutation's end (`TODO(coroutines)`); the populate is guarded by `cache::CanFill`, so a concurrent mutation makes it a no-op | same as 1 |
| 4 | `dcfs/backing.cc:160` (`ListXattrOPath`) | size the xattr list, read it, `ERANGE` means it grew in between | 4 attempts, then the `ERANGE` status | none: `listxattr` has no "size and read atomically" form; a writer racing the reader is the only cause | ask: keep (approved comment) or one size-then-read and `ERANGE` as the answer |
| 5 | `dcfs/fuse_request.cc:175` (`fuse_req_getgroups`) | one re-call at the size the first call reported | exactly one second call, not a loop | none needed: the first call reports the true count | not a retry loop (a two-step read); listed for completeness |
| 6 | `dcfs/umount_helper.cc:56` (`BlockingLock`) | restart `flock` after `EINTR` when `retry_signals` | unbounded while non-fatal signals arrive | n/a: this is the kernel asking for a restart, section 1.12's first "not a retry loop" | keep |
| 6a | `dcfs/backing.cc:2092` (`GetGroups`) | `getgroups(0)` for the count, then `getgroups(n)`; `EINVAL` (the list grew) asks again | unbounded `while (true)` | none; the comment itself says another thread cannot change our list, so the loop cannot iterate | one count-then-read, `EINVAL` reported (found 2026-10-10 after the inventory above) |
| 6b | `dcfs/backing.cc:178` (`GetXattrOPath`) | size the value, read it, `ERANGE` means it grew | same shape as 4 | same as 4 | same ruling as 4 |

russ, 2026-10-10, on items 1-3: no bounded retries and no EAGAIN fallback;
contention between our own threads is a mutex or an unbounded optimistic
retry with a progress argument checked in the model (style guide 1.12).
Items 1-3 therefore lose `kAttempts` and the EAGAIN return. The progress
argument: what invalidates a resolve is another mutation's phase 1 or 3
(or a fill), each of which completes in a bounded number of steps without
waiting on the retrying request, so a retry sees a settled state; the model
gets a liveness property (every begun unlink/rename/readdir eventually
begins its mutation or serves its listing, under weak fairness of the
other requests' steps) and the retry-or-EAGAIN branch becomes retry only.
Today the loops cannot iterate at all (one request at a time), so the code
change is small; the model change is the substance. Items 4 and 6b (russ, 2026-10-10: "Do whatever the underlying filesystem
does in this case"): a filesystem asked for a list or value into a buffer
that became too small answers ERANGE and leaves the retry to the program.
So one size-then-read, and ERANGE as the answer; where a client's own
buffer size is at hand (FUSE GETXATTR/LISTXATTR with a size), pass that
size through once and let the client see what the backing would have
answered. The cache fill (XattrsOf) sizes once and reports ERANGE.
Item 6a cannot iterate (the comment says so): one call pair, EINVAL
reported, which is not a retry.

Items 1-3 share one shape: a model branch (dcfs.tla's "retry or EAGAIN")
that exists for the coroutine future and cannot be exercised today. If
russ rules "one attempt, then EAGAIN", the model's retry branch goes too
(a `formal/` change in the same step) and the `kAttempts` constants and
loops become straight-line code.

## Guest test scripts and tools

| # | Site | Loop | Note |
|---|---|---|---|
| 7 | `test/qemu/guest/xfstests.sh:354` `retry_missing` | re-runs, alone, each test whose `check` batch was interrupted before reaching it, once | not a retry of a failure: a batch interrupted by the watchdog ran nothing for its later tests; they run once each. Rename to avoid the word |
| 8 | `test/qemu/guest/fault_dcfs_lib.sh:136` | "three looks 0.2 s apart" at a daemon's state | a sleep-and-poll already on `tools/repo_shape_sleeps.txt` (6.5 removes it) |
| 9 | the `kill -0` / READY-line polls on `tools/repo_shape_sleeps.txt` | sleep-and-poll waits | already listed under the no-timers rule; 6.5 removes them |

Nothing in `tools/` retries. The 17.1 image's mount retry (0.2 s while the
kernel dropped the old namespace) went before the merge (5ea0719).

## Enforcement

Mechanical enforcement is wanted (russ prefers build-graph gates). What
is checkable: `tools/repo_shape.py` can refuse `kAttempts`, `attempt`,
`retries` and `retry` as identifiers in `dcfs/*.cc` and `dcfs/*.h` outside
an allowlist that only shrinks, the same shape as the sleep list. The
word in a comment is fine. Queue as 25.10 after russ rules on 1-6.
