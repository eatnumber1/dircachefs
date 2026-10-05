# Phase 4 — Pinned host tools; third_party/ convention

**Decisions (russ, 2026-10-05).**
- Everything the tests run is pinned, not just the compiler: QEMU, busybox,
  the Debian test image (and with it the filesystem tools and systemd
  inside it), alongside the kernel (Phase 3 (Drop the kernel patch; download and build the test kernel)) and the toolchain (Phase 7 (Pinned toolchain, coverage, warnings, UBSan, clang-tidy)).
- Convention (russ, 2026-10-05): Bazel downloads, builds and includes
  every third-party dependency. Third-party source is not checked in
  unless there is no other way (and then the reason is written next to
  it). Prefer the BCR; otherwise `http_archive`/`http_file` with sha256
  plus Bazel rules that build it. `third_party/<name>/` holds only what
  we write: BUILD overlays, rules, patches, config fragments, lock files
  and a README with the update procedure.

**Design.**
- `third_party/linux/`: the config fragment and the kernel build rule
  (Phase 3).
- `third_party/qemu/`: a pinned QEMU release (`http_archive`, sha256)
  built by Bazel, as minimal as possible (russ, 2026-10-05): everything we
  do not use is disabled, so the build has the fewest dependencies and
  the least to go wrong.
  - Start from nothing: `--without-default-features` (every optional
    feature off) and `--without-default-devices`, then enable only what
    the guests use.
  - One target: `--target-list=x86_64-softmmu`. No user-mode emulation,
    no other architectures.
  - Accelerators: KVM, and TCG for the fallback without KVM (Phase 5.1).
  - Devices, from a checked-in device config: the microvm machine,
    virtio-mmio, virtio-blk, the ISA serial port and the RTC the harness
    enables (`rtc=on`); add a virtio RNG only if Phase 5.1 shows entropy
    waits. Nothing else: no PCI devices, USB, network devices, display,
    audio or input.
  - Block layer: raw images only (`--block-drv-whitelist=raw`; no qcow2,
    no network block drivers).
  - Off: user interfaces (SDL, GTK, VNC, Spice, curses), networking
    backends (slirp, vhost-net, vhost-user), audio, TPM, the guest agent,
    tools (`qemu-img` and the rest), docs, plugins, tracing backends,
    seccomp, crypto libraries, and every optional library dependency.
  - Firmware: only qboot, built from its pinned source by Bazel if that is
    simple, otherwise the `qboot.rom` shipped inside the pinned QEMU
    tarball (not checked in). No other firmware blobs installed.
  - Dependencies: glib is the one QEMU always requires (from the BCR);
    pixman, zlib and the rest only if the pinned release still requires
    them with this configuration. `third_party/qemu/README.md` lists every
    remaining dependency, enabled feature and device with the reason it is
    needed, so the minimal set stays deliberate.
  - Test first: the whole suite passes with this QEMU; a check records
    `-device help` and `-machine help` output and fails if a device or
    machine appears that the README does not list.
  - If a hermetic build proves impractical, stop and report to russ with
    the reason before falling back to anything else.
- `third_party/busybox/`: a pinned release (`http_archive`) built
  statically by a Bazel rule with a checked-in config (the applets the
  guest scripts use, e.g. `mountpoint` and `find -ls`, which the host
  busybox lacked).
- `third_party/debian/`: the Debian test image built by `rules_distroless`
  (BCR) from a fixed snapshot.debian.org timestamp and a checked-in
  package list and lock file, replacing `mkrootfs-debian.sh` and its
  mmdebstrap run; the result is assembled into a filesystem image by a
  Bazel rule. The filesystem tools and systemd inside it are pinned with
  it.
- The existing pjdfstest overlay already follows this pattern
  (`http_archive` plus a BUILD overlay); fsstress/fsx (Phase 11), the
  xfstests subset (Phase 17), TLC (Phase 12) and pandoc (Phase 15) follow
  it too.
- The repository rule in `test/qemu/kernel.bzl` that symlinks
  out-of-tree builds and host binaries is removed; test rules depend on
  the Bazel-built targets directly.
- Updating: one documented procedure per tool (bump version and sha256,
  run the full suite), in each `third_party/<name>/README.md`.
- Test first: a guest test that checks the QEMU, busybox and Debian
  versions the run used against the pins.
Owner: Sonnet, Opus review of the QEMU build. Order: with Phase 3 (both change how guests are built).

**R3 follow-up (2026-10-06 review, finding L5/L9; `review-fixes.md`).**
The scratch filesystems the tests format are now pinned too, not only
the Debian guest's own tools:
- `third_party/xfsprogs/` (xfsprogs 7.2.0: static `mkfs.xfs`, `xfs_io`) and
  `third_party/btrfs-progs/` (v7.1: static `mkfs.btrfs`, `btrfs`), with
  private static `libuuid`/`libblkid` (`third_party/util_linux/`, util-linux
  2.42.4), liburcu (`third_party/urcu/`) and libinih (`third_party/inih/`).
  Smoke tests check static linking, no sanitizer runtime under
  `--config=asan`, the pinned `--version` and a real mkfs.
- `third_party/e2fsprogs/mke2fs.conf`: the checked-in profile
  (`MKE2FS_CONFIG`), so no ext4 image depends on the host's
  `/etc/mke2fs.conf`; `mkrootfs.sh` uses it; `mke2fs_conf_test` checks the
  feature set.
- L9: `common --lockfile_mode=error`, plus `debs.lock` (every fetched .deb
  and its sha256, 149 lines) checked by `version_check_test`.
- Remaining: `test/qemu/scripts/run-qemu.sh` still formats with host
  `mkfs.*` until the lane that owns it merges (the switch is a ready-made
  patch in the R3 hand-off).
