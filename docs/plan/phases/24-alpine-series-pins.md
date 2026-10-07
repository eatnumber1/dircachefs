# Phase 24 — Test kernel and host-side tools from Alpine, pinned to a series

**Decision (russ, 2026-10-07).** The test kernel and the host-side test
tools come from Alpine's current release branch, pinned to a release
series (e.g. the branch's 6.18 kernel) rather than to a hash, with
integrity from Alpine's package signatures. Spike: `spike-alpine` branch
in lane-2; report in notes/alpine-spike-2026-10-07.md. Libraries linked
into dcfs stay source builds. The systemd guest stays Debian (released
cloud image). dm-dust is dropped (Phase 11 uses error and flakey).

## Design (from the spike's recommendations, simplified by russ 2026-10-07)
**The pin is the Alpine stable branch** (e.g. `v3.22`): a stable branch
carries one kernel series and one major.minor of each tool for its life,
so "series" and "branch" are the same thing. Alpine advancing within the
branch (6.12.0 → 6.12.1, a tool's patch release) must never fail a test or
a job. No version of any package appears in our tree.
1. `alpine_package` repository rule (third_party/alpine/): downloads the
   branch's signed APKINDEX, verifies it with the checked-in Alpine key,
   takes the branch's current version of each named package, downloads
   the apk and verifies its signature and data hash. The resolution lives
   in the repository rule (its result is not recorded in MODULE.bazel.lock,
   which is in `error` mode), or in a module extension marked
   `reproducible`. The rule writes a small `resolved.json` (origin,
   version) into each repo for the SBOM and for the logs. No lock file,
   no exact-version or sha256 escape hatch, no expiry, no drift test
   (russ, 2026-10-07: all three existed only to serve each other). Bazel
   re-runs a repository rule only when its inputs change, so a developer
   keeps what they fetched until `bazel fetch --force --repo=@alpine_...`
   (documented in third_party/alpine/README.md); CI fetches fresh each run
   (externals are not in its restored cache) and so tests the branch's
   current packages. Dev and CI may differ within the series. The weekly
   schedule keeps running the suite, so a bump that breaks something
   surfaces there first; that is the tests on a fresh fetch, not a version
   check. Tests of the rule: a tampered index and a tampered apk are
   refused (signature), a package missing from the branch fails with a
   clear message.
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
4. SBOM: each repo's `resolved.json` feeds the test-only SBOM as
   `pkg:apk/alpine/<origin>@<ver>?distro=alpine-<branch>`; a test that no
   component is a binary subpackage name (silent false negative otherwise).
5. Within-series variation: dev and CI may see different versions of the
   series; kernel and modules always come from the same apk; nothing
   hard-codes `uname -r` or an exact version (identity checks match
   `6.12.*-virt`, tool version checks match the major.minor).
6. Tests first: the signature tests failing first (tampered index,
   tampered apk); boot_test on the Alpine kernel with the per-test
   module list; the full suite green; cold-build time before/after (the
   kernel's 690 s and the tools' ~900 s disappear; downloads ~100 MB).
Owner: dcfs-investigator for 1-3 (musl wrappers, modules), dcfs-implementer
for 4-6. Order: as soon as possible (russ, 2026-10-07: the tests get much
faster); it runs alongside the guest-memory step and rebases over it
(both touch run-qemu.sh and the macros).
