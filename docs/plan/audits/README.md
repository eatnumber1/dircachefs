# Audits (2026-09-27 to 2026-09-28)

Read-only audits of dcfs made during the original plan, kept for their
reasoning. Each names the commit it audited. They are snapshots: every
finding was later fixed test-first, assigned to a step, or decided against;
`../log.md` records which (search for the finding ids, e.g. "F3").

- `crash.md`: crash and power-loss robustness (F1-F10).
- `races.md`: concurrent-request races.
- `tristate.md`: present/absent/unknown state of every cached record.
- `style.md`: style, readability, dead code.
- `test-coverage.md`: whether every earlier fix got a test.
- `review-2026-10-06-waves-1-2.md`: review of everything merged in waves
  1-2 (cache hardening, pinned kernel/QEMU/busybox/Debian/e2fsprogs,
  harness wiring, sanitizer scoping).
- `review-2026-10-06-r4.md`: review of the R4 protocol fixes (sync
  snapshot, readdir listing, rename verification).
- `review-2026-10-06-trace-validation.md`: review of the first trace
  validation; the recorder's cuts and the script's pass rule let forbidden
  orderings validate (fixed in 12.2b).
- `review-2026-10-07-trace-validation-2.md`: re-review after 12.2b; one
  recorder bug (held lines dropped when a directory dies mid-listing) and
  two "failed" cuts that did not check the request's outcome.
- `review-2026-10-07-phase23.md`: review of Phase 23 (stubs, removed objects, FORGET reconciliation, copy_file_range/ioctls/tmpfile, relatime).
