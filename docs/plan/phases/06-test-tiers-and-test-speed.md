# Phase 6 — Test tiers and test speed

**Rule (russ): quality over speed.** No optimization may weaken a test:
same assertions, same coverage, same isolation (one fresh guest per test).
Every speed change is reviewed for that, with before/after timings.

**6.1 Tiers, using Bazel's native test sizes.** Every test's `size`
attribute is its tier, filtered with `--test_size_filters` (Bazel 9.2):
- `small` = fast: unit tests and quick e2e tests; run constantly during
  development (`bazel test --config=fast //...` =
  `--test_size_filters=small`).
- `medium`: the rest of the per-feature e2e matrix (ext4/xfs/btrfs),
  pjdfstest, NFS; run before submitting (`--config=presubmit` =
  `--test_size_filters=small,medium`).
- `large` / `enormous` = slow: power-loss replay, fsstress/fsx, xfstests,
  the systemd guest, full benchmark runs, the ASan matrix; run by CI (no
  filter).
`size` also sets Bazel's default timeout and its resource estimate for
scheduling (small assumes ~20 MB of RAM), which is wrong for a QEMU guest
(256 MB-1 GB). So each QEMU test also declares its real needs (the `cpu:N`
tag and a memory resource tag; verify the exact tag syntax Bazel 9.2
accepts) and an explicit `timeout`, so a `small` QEMU test does not make
Bazel start too many guests at once. Today every QEMU test is `large`
(`qemu_test.bzl` default); sizes are reassigned from measured times
(target: all small tests in about a minute with KVM, presubmit in about
ten).
**6.2 Speed, without compromise.** Candidates, each measured:
- Replace sleeps and polling loops in guest scripts with waits on the
  actual event (mount appears, process exits, file exists).
- Shard long tests (pjdfstest by test directory, xfstests by group,
  fsstress seeds) with Bazel `shard_count`.
- Let Bazel run guests in parallel: keep guest memory small and check host
  limits so parallel runs do not swap.
- Build images (filesystem images, initramfs, rootfs) once per build and
  copy them per test instead of rebuilding.
- The minimal kernel (Phase 3 (Drop the kernel patch; download and build the test kernel)) and the TCG investigation (5.1) for boot
  time.
Order: 6.1 with the CI phase (CI needs the tiers); 6.2 continuously,
first pass right after 6.1.
