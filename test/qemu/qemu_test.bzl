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
            "@kernel_image//:rootfs_debian.ext4"). When given, run-qemu.sh
            attaches it as an extra virtio-blk disk (the next /dev/vd<letter>
            after `disks`) and passes dcfs_rootfs=/dev/vd<letter> on the
            kernel command line; guest/init then mounts it, bind-mounts
            /proc, /sys and /dev over it, copies dcfs/fhtest/testutil and
            /tests in, and chroots into it to run guest_script with GNU
            userspace and nfs-utils available. See guest/init's
            dcfs_rootfs= branch and test/qemu/scripts/mkrootfs-debian.sh.
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

    sh_test(
        name = name,
        srcs = ["scripts/run-qemu.sh"],
        data = [
            ":initramfs",
            "@kernel_image//:bzImage",
            guest_script,
        ] + rootfs_data,
        args = rootfs_args + [
            "$(location @kernel_image//:bzImage)",
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
