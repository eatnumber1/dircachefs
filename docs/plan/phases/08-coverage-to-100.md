# Phase 8 — Coverage close to 100%

**Decision (russ, 2026-10-05).** Coverage is a priority: get close to
100% as soon as possible, then keep it there.
- Measurement is 7.2 (clang source-based coverage, unit and QEMU tests
  merged). It moves to the front of the toolchain phase: switch compiler,
  then coverage, then warnings and UBSan.
- Scope: production code (`dcfs/`, the wrapper, helpers); tests and
  `third_party/` excluded. Line and branch coverage both reported; the
  goal is ~100% of lines and every branch, with each remaining gap either
  covered or listed in `docs/coverage.md` with the reason and agreed with
  russ (no silent exclusions; unreachable code is removed or turned into
  a CHECK).
- Error paths are most of what is uncovered in code like this. Reach them
  with fault injection, not one-off tests:
  - **Syscall failure sweeps:** a test-only interposer (the existing
    link-time `-Wl,--wrap` fakes) that fails the Nth call of a given
    syscall with a given errno; a test runs a request once to count calls,
    then reruns it failing each call in turn, asserting the request
    returns an error, the cache stays consistent (the cache checker,
    below), and
    nothing leaks. The approach SQLite uses for its own I/O error tests.
  - **SQLite failures:** a test VFS shim (SQLite's documented VFS
    interface) that fails the Nth read, write, sync or lock with
    SQLITE_IOERR, SQLITE_FULL or SQLITE_BUSY, swept the same way.
- **Cache checker, run after every test** (built here because the
  failure sweeps need it; Phase 11 relies on it too). A test-only tool
  compares every cached entry (present/absent dentries, attributes, xattrs, symlink
  targets) with the backing filesystem. Every QEMU test runs it at teardown
  against the cache it used, new or reused: a cache filled during the test
  is exactly what drift would corrupt. A mismatch fails the test.
- Ratchet: CI fails if coverage drops below the last recorded value
  (stored in the repo). Every later phase's "done" includes full coverage
  of the code it adds; AGENTS.md states the rule.
Owner: Sonnet for the sweeps' infrastructure (Opus review); gap-closing
by area owners. Order: right after the toolchain phase, before
everything after it.

## Decisions from the first report (russ, 2026-10-08; notes/coverage-baseline-2026-10-08.md)

- The gate: `dcfs/*.cc` only, lines AND branches, fails on any drop
  below a committed baseline file that starts at today's 92.4 / 73.3 and
  only moves up (bumped in the commit that raised it); bench/ and tools/
  reported, not gated.
- The non-passthrough data path is NOT dead: the kernel grants
  passthrough per mount but refuses it per file past the backing stack
  depth (a source on a FUSE mount), so READ/WRITE fall back to dcfs; it
  gets an e2e variant with the source on a FUSE mount (dcfs over dcfs
  in the guest) rather than deletion.
- Dead for real, delete: the FUSE `Access` handler (never sent under
  `default_permissions`, which stays: the kernel's checks from cached
  attributes cost no backing I/O; dropping it would mean an ACCESS
  request per permission decision, either no more faithful or a backing
  syscall per check), `FuseRequest`'s move-assignment, the `openat2`
  wrapper.
- `FileHandle::ToString` gets a test; style rule to add to docs/style.md
  (tests): a debug/log string is tested for the important fields it must
  contain (substrings or a regex per field), never compared to a
  hard-coded whole.
- Cheap gaps, one mechanical step: status.cc errno-name edges, device_id
  parse failures, mountinfo parsing edges, utimensat/removexattr on a
  closed file, main.cc startup failure modes (a guest misuse test).
- Error branches after syscalls and SQLite steps: not hand-written; 26.6
  and Phase 11's cache-disk error injection cover them.
