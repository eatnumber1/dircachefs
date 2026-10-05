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
