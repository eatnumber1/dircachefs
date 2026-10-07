# Phase 24 — Test kernel and host-side tools from Alpine, pinned to a series

**Decision (russ, 2026-10-07).** The test kernel and the host-side test
tools come from Alpine's current release branch, pinned to a release
series (e.g. the branch's 6.18 kernel) rather than to a hash, with
integrity from Alpine's package signatures. Spike: `spike-alpine` branch
in lane-2; report in notes/alpine-spike-2026-10-07.md. Libraries linked
into dcfs stay source builds. The systemd guest stays Debian (released
cloud image). dm-dust is dropped (Phase 11 uses error and flakey).

## Design (from the spike's recommendations)
1. `alpine_package` rule (third_party/alpine/): one shared, signed index
   per branch resolved once (module extension), newest version of the
   series, apk signature + index digest + datahash verified with the
   checked-in Alpine key; `alpine.lock` records the picked versions and
   origins; a `drift_test` fails when Alpine has moved, run by the
   scheduled CI job (GitHub Actions cron) so updates fail as their own
   job, never on a push. Escape hatch: `version=`/`sha256=`/`expires=`
   pins, served from a copy we keep as a GitHub release asset (Alpine
   drops superseded files within days); the expiry test is `external`.
2. Kernel: `linux-virt` of the series; each test declares the modules it
   needs (`modules = [...]` on the test macros), decompressed at initramfs
   build time and loaded by `guest/init` (add insmod/modprobe to the
   busybox fragment, or take busybox-static from Alpine); `boot.sh`
   identity checks relaxed to the series (`6.18.*-virt`). Remove
   third_party/linux's source build and config fragment (keep the option
   list as a test against the Alpine kernel's config: built-in or module,
   never off). FUSE_IO_URING: present in 6.18.
3. Host tools from apk closures run through musl's loader with small
   wrappers: QEMU (`QEMU_MODULE_DIR`, `-L`), e2fsprogs, xfsprogs,
   btrfs-progs, util-linux's mkfs, busybox-static; remove our source builds
   of QEMU, e2fsprogs, libarchive, xfsprogs, btrfs-progs, util-linux, urcu,
   inih, busybox and bc, their pkg-config shims and the exec_file wrappers
   of 6.3b, and the QEMU device-allowlist test (Alpine's QEMU has ~450
   devices; the harness passes explicit devices and `-nodefaults`).
4. SBOM: `alpine.lock` feeds the test-only SBOM as
   `pkg:apk/alpine/<origin>@<ver>?distro=alpine-<branch>`; a test that no
   component is a binary subpackage name (silent false negative otherwise).
5. Drift: dev and CI may see different kernels within the series; kernel
   and modules always come from the same apk; nothing hard-codes
   `uname -r`.
6. Tests first: the drift/expiry/signature tests failing first (tampered
   index, expired pin); boot_test on the Alpine kernel with the per-test
   module list; the full suite green; cold-build time before/after (the
   kernel's 690 s and the tools' ~900 s disappear; downloads ~100 MB).
Owner: dcfs-investigator for 1-3 (musl wrappers, modules), dcfs-implementer
for 4-6. Order: after the guest-memory step merges (both touch run-qemu.sh
and the macros).
