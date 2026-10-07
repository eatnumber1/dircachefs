# The test kernel: Alpine's linux-virt (Phase 24)

Every QEMU test boots Alpine's `linux-virt` kernel of the pinned Alpine
branch (`MODULE.bazel`'s `alpine_index`, `third_party/alpine/README.md`):
`//third_party/linux:vmlinuz` is the package's `/boot/vmlinuz-virt`. It
replaces the kernel Bazel used to build from source (a pinned kernel.org
release from `tinyconfig` plus a fragment: about 690 s on a cold cache, with
host flex, bison, libelf and GNU bc).

## Series, not version

The branch carries one kernel series for its life (v3.24: 6.18), so the
branch is the pin. Alpine publishing 6.18.56 for 6.18.55 is taken as it
comes: `guest/boot.sh` accepts `6.18.*-virt`, never an exact release, and
nothing hard-codes `uname -r` or the module directory (which carries the
release). The kernel and its modules always come from the same apk. A new
branch with another series needs `boot.sh`'s pattern updated (the failure
says so).

## Modules

Alpine builds most drivers as modules (virtio_blk, fuse, ext4, xfs, btrfs,
nfs, device-mapper targets), so a guest loads what its test needs, and only
that: loading is the bulk of the boot time (measured: xfs about 2 s, ext4
0.85 s, jbd2 0.5 s, btrfs 0.5 s, virtio_blk 0.5 s under KVM; the whole set
5 s against 1.8 s for the minimum).

`qemu_test`, `qemu_test_matrix` and `qemu_cc_test` take `modules = [...]`.
What every test needs is the default: `fuse` always, and `virtio_blk` and
the filesystem modules of the test's `disks` (and of its `rootfs`). `modules`
adds to that. `test/qemu/scripts/mkmodules.py` builds, per distinct module
set, a small cpio archive with the modules and their dependencies
(`modules.dep`, plus `modules.softdep`: btrfs wants its checksum algorithms
loaded first), decompressed at build time; `run-qemu.sh` appends it to the
shared initramfs and `guest/init` runs `insmod` over `/etc/dcfs-modules`,
which lists them in load order. A module the kernel has built in is skipped
(an Alpine config change inside a branch never fails a test); an unknown
module fails the build with its name.

## The option list

`required_options.txt` lists the kernel options the tests rely on, each with
its reason (it was `kernel.config`, the build fragment).
`//third_party/linux:kernel_config_test` checks every one against the
package's `/boot/config-*`: built in or a module, never off. To add a
requirement, add the line with the reason; if Alpine's kernel lacks it the
test fails, and the options of the kernel are Alpine's to choose, so the answer
is then to drop the need or to take a different kernel (not to patch one).
`CONFIG_DM_DUST` is the one option we gave up (Alpine does not build it;
Phase 11 uses dm-error and dm-flakey).

## What we do not control

Alpine's kernel has ACPI, EFI, many drivers and module signing
(`MODULE_SIG` on, not forced: our modules are the signed ones from the apk,
loaded by `insmod`). The bzImage is 12.6 MB (the old one 3.5 MB) and boots
in about the same time with the minimal module set (spike, 2026-10-07).
The kernel image and modules are in the Bazel output base under
`external/+alpine_package+alpine_linux_virt/`.

## Kernel matrix (not yet)

Testing the minimum supported kernel (6.9) as well would need a second
kernel with none of the options newer than 6.9 (`FUSE_IO_URING` is 6.14) and
a Bazel flag choosing the kernel in the `qemu_test` macros. Alpine's older
branches carry older series (v3.20: 6.6), so a second `alpine_package` of an
older branch is one candidate; nothing here does it yet.
