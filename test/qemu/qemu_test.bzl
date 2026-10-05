"""qemu_test(name, guest_script, disks): a dcfs QEMU end-to-end test.

Boots the shared dcfs QEMU initramfs (:initramfs) and test kernel
(@kernel_image//:bzImage) under QEMU, telling guest/init (via the
dcfs_test= kernel command-line parameter) to run the given guest_script,
found inside the initramfs at /tests/<basename of guest_script>. See
test/qemu/scripts/run-qemu.sh for the boot/verdict mechanics and
test/qemu/guest/init for the guest side.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

def qemu_test(name, guest_script, disks = [], rootfs = None, size = "large", timeout = "long"):
    """Declares a QEMU end-to-end test.

    Args:
        name: the test target's name.
        guest_script: label of the guest/*.sh script to run inside the
            guest (must already be baked into :initramfs, i.e. matched by
            the guest/*.sh glob in this package's initramfs genrule).
        disks: list of (device, fstype, size) tuples, e.g.
            ("vdb", "ext4", "256M"). device is a /dev/vd<letter> name;
            see run-qemu.sh for how the letter maps to QEMU drive order.
        rootfs: optional label of a Debian rootfs ext4 image (normally
            "//third_party/debian:rootfs"). When given, run-qemu.sh
            attaches it as an extra virtio-blk disk (the next /dev/vd<letter>
            after `disks`) and passes dcfs_rootfs=/dev/vd<letter> on the
            kernel command line; guest/init then mounts it, bind-mounts
            /proc, /sys and /dev over it, copies dcfs/fhtest/testutil and
            /tests in, and chroots into it to run guest_script with GNU
            userspace and nfs-utils available. See guest/init's
            dcfs_rootfs= branch and third_party/debian/README.md.
        size: sh_test size, e.g. "enormous" for a much longer-running guest
            script than the default e2e tests here (most run in a few
            seconds); defaults to "large", the size every test in this
            package used before this parameter existed.
        timeout: sh_test timeout, e.g. "eternal" (3600s) to pair with
            size = "enormous"; defaults to "long" (900s), unchanged from
            before this parameter existed.
    """
    disk_args = [d[0] + ":" + d[1] + ":" + d[2] for d in disks]
    guest_script_basename = guest_script.split("/")[-1]

    rootfs_data = [rootfs] if rootfs else []
    rootfs_args = ["--rootfs", "$(location " + rootfs + ")"] if rootfs else []

    # Step 3.1a: --//test/qemu:kernel selects which kernel this test boots
    # (see BUILD.bazel's :kernel string_flag/:kernel_stock config_setting).
    kernel_data = select({
        "//test/qemu:kernel_stock": ["//third_party/linux:bzImage"],
        "//conditions:default": ["@kernel_image//:bzImage"],
    })
    kernel_args = select({
        "//test/qemu:kernel_stock": ["$(location //third_party/linux:bzImage)"],
        "//conditions:default": ["$(location @kernel_image//:bzImage)"],
    })

    sh_test(
        name = name,
        srcs = ["scripts/run-qemu.sh"],
        data = kernel_data + [
            ":initramfs",
            guest_script,
        ] + rootfs_data,
        args = rootfs_args + kernel_args + [
            "$(location :initramfs)",
            guest_script_basename,
        ] + disk_args,
        tags = [
            "e2e",
            "exclusive",
            "no-sandbox",
            "requires-kvm",
        ],
        size = size,
        timeout = timeout,
    )

# Step 5.2: dcfs's own README/plan promise ext4, xfs and btrfs, but every
# e2e test used to hard-code "ext4" in its disks= tuple. qemu_test_matrix
# generates one qemu_test per filesystem in `fstypes`, varying only the
# first disk's (vdb, the backing source) filesystem type -- every other
# disk (e.g. vdc, used by the submount/boundary-refusal checks) keeps
# whatever fstype `disks` gave it, since those checks only care that vdc is
# a *different* device, not what filesystem it carries. This generates the
# guest_script's checks three times with no script duplication: the guest
# script itself detects which filesystem it is running on (see
# guest/lib.sh's backing_fstype) and branches internally wherever a
# filesystem's own semantics genuinely differ (generation/ACL/xattr/statx
# specifics -- see README.md's "tested on" line and docs/conformance.md).
#
# `name` becomes an alias to `name + "_" + fstypes[0]` (ext4 by default),
# so every existing `bazel test //test/qemu:<name>_test` command and every
# doc reference to it keeps working unchanged.
def qemu_test_matrix(
        name,
        guest_script,
        disks,
        size = "large",
        timeout = "long",
        rootfs = None,
        fstypes = ["ext4", "xfs", "btrfs"]):
    """Declares one qemu_test per backing filesystem in `fstypes`.

    Args:
        name: the base name; generates "<name>_<fstype>" per fstypes entry
            plus a plain "<name>" alias to the first (ext4) variant.
        guest_script: same as qemu_test.
        disks: same shape as qemu_test's `disks`, but the first entry's
            fstype field is overridden per generated variant -- pass
            whatever placeholder fstype reads best (by convention "ext4").
        size: same as qemu_test.
        timeout: same as qemu_test.
        rootfs: same as qemu_test.
        fstypes: filesystems to generate variants for, in order; the first
            is what plain "<name>" aliases to.
    """
    if not disks:
        fail("qemu_test_matrix(%s): disks must have at least one entry " % name +
             "(the backing source disk, vdb, whose fstype is varied)")
    for fstype in fstypes:
        varied_disks = [(disks[0][0], fstype, disks[0][2])] + list(disks[1:])
        qemu_test(
            name = name + "_" + fstype,
            guest_script = guest_script,
            disks = varied_disks,
            size = size,
            timeout = timeout,
            rootfs = rootfs,
        )
    native.alias(
        name = name,
        actual = ":" + name + "_" + fstypes[0],
    )
