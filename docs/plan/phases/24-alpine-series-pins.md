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

## 24.1 SELinux under Fedora's default policy (russ, 2026-10-18; backlog, not started, low priority, not in a milestone)

russ, 2026-10-18: "Let's add to the backlog somewhere some SELinux testing
with Fedora's ruleset. It's not a feature I need, but Fedora ships it by
default and Fedora is popular."

Scope: a guest test configuration that boots an SELinux-enabled kernel with
Fedora's targeted policy (selinux-policy-targeted) in enforcing mode, and runs
dcfs's mount, xattr (security.selinux labels through dcfs: getxattr, setxattr,
listxattr; the `context=` mount option; file creation labels), credentials and
passthrough tests under it. Known touch points: security_capget can deny
capget (17.2e restored its error path); the security.* xattrs; FUSE's handling
of security labels (fuse_init_security, FUSE_SECURITY_CTX); the daemon's own
domain (unconfined, or a confined type).

First sub-step: find whether an Alpine or hermetic Fedora policy build is
feasible in the QEMU harness, and what the test kernel config needs
(CONFIG_SECURITY_SELINUX, and the policy's userspace tools if any).

Owner: dcfs-investigator for the first sub-step; the implementer after that.

## 24.2 Minimum kernel 6.12: Alpine v3.22 (russ, 2026-10-18; queued for Thursday 2026-10-22, not before)

russ, 2026-10-18: "Minimum kernel 6.12: Alpine v3.22". Facts (2026-10-18): v3.22
ships `linux-virt` and `linux-virt-dev` 6.12.113-r0, supported until
2027-05-01. v3.21 also has 6.12 but ends 2026-11-01; v3.23 and v3.24 are 6.18.
russ's rulings of 2026-10-18: the minimum supported kernel becomes Linux 6.12
LTS (was 6.9; 6.9-6.11 are EOL non-LTS); the test kernel is Alpine's
`linux-virt` 6.12.x from the branch that carries it, patch version floating
(`third_party/linux/README.md`, "Series, not version"); the toolchain's UAPI
headers come from the same branch's `linux-virt-dev` through a genrule ("A
genrule is acceptable"), floating in lockstep with the kernel.
russ, 2026-10-19, on the header part: "Yes. Keep the Debian 6.12 headers."

Header decision (2026-10-19): no genrule. The sysroot's Debian trixie
`linux-libc-dev` 6.12.107 (`MODULE.bazel`, line 501) already matches the 6.12
minimum. UAPI headers do not change within a stable series, and the pin is
major.minor with the patch floating, so the sysroot's headers stay as they are.
Research behind it: `linux-virt-dev` ships raw kernel headers and
`headers_install.sh` but no compiled `unifdef`, so a genrule would need
`unifdef` built or reimplemented, plus `headers_install`'s sed transforms.

Work:

- MODULE.bazel: `alpine_index` branch v3.24 -> v3.22, one branch for every
  Alpine package (the single-pin design); QEMU and the filesystem tools move
  with it.
- `test/qemu/guest/boot.sh`: `6.18.*-virt` -> `6.12.*-virt`.
- `third_party/linux/README.md` and `test/qemu/README.md`: 6.18 -> 6.12, and
  the minimum-kernel text.
- No header work: the Debian sysroot's `linux-libc-dev` (6.12, see the header
  decision above) stays, and no genrule reads `linux-virt-dev`.
- `dcfs/proc_status.cc`: the `USER_NS_INIT_INO` exception, exactly as russ
  wrote it (russ, 2026-10-18; 6.12's headers lack the constant, it entered
  UAPI in 6.17):

  ```
  #ifndef USER_NS_INIT_INO
  #define USER_NS_INIT_INO 0xEFFFFFFD
  #endif  // USER_NS_INIT_INO
  ```

- README: the minimum 6.9 -> 6.12 (the requirements bullet and the badge);
  the "Kernel matrix" bullet, which describes testing the minimum as not yet
  done, is re-checked for the new minimum.
- When 6.12 lands, update every remaining 6.9 floor: the README badge (Kernel
  6.9+), phases/05-ci.md 5.5, phases/13-connected-backing-fds.md,
  phases/15-mount-dcfs-wrapper.md and phases/21-real-hardware-checks.md (grep
  `6\.9` for others).
- Then re-run every tier, and re-baseline what the kernel and tool change
  moves: io and cpu budgets, the xfstests and pjdfstest expected-failure
  lists, `kernel_facts`, and state counts if any. Each re-baseline commit says
  why.
Owner: dcfs-investigator, then dcfs-implementer.
