"""qemu_test(name, guest_script, disks): a dcfs QEMU end-to-end test.

Boots the shared dcfs QEMU initramfs (:initramfs) and test kernel
(@kernel_image//:bzImage) under QEMU, telling guest/init (via the
dcfs_test= kernel command-line parameter) to run the given guest_script,
found inside the initramfs at /tests/<basename of guest_script>. See
test/qemu/scripts/run-qemu.sh for the boot/verdict mechanics and
test/qemu/guest/init for the guest side.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

def qemu_test(name, guest_script, disks = []):
    """Declares a QEMU end-to-end test.

    Args:
        name: the test target's name.
        guest_script: label of the guest/*.sh script to run inside the
            guest (must already be baked into :initramfs, i.e. matched by
            the guest/*.sh glob in this package's initramfs genrule).
        disks: list of (device, fstype, size) tuples, e.g.
            ("vdb", "ext4", "256M"). device is a /dev/vd<letter> name;
            see run-qemu.sh for how the letter maps to QEMU drive order.
    """
    disk_args = [d[0] + ":" + d[1] + ":" + d[2] for d in disks]
    guest_script_basename = guest_script.split("/")[-1]

    sh_test(
        name = name,
        srcs = ["scripts/run-qemu.sh"],
        data = [
            ":initramfs",
            "@kernel_image//:bzImage",
            guest_script,
        ],
        args = [
            "$(location @kernel_image//:bzImage)",
            "$(location :initramfs)",
            guest_script_basename,
        ] + disk_args,
        tags = [
            "qemu",
            "exclusive",
            "no-sandbox",
        ],
        size = "large",
        timeout = "long",
    )
