"""The kernel modules a QEMU test's initramfs carries (step 24.2).

The test kernel is Alpine's linux-virt, whose drivers are mostly modules:
see third_party/linux/README.md.
"""

# The module of each backing or root filesystem type a test's `disks` and
# `rootfs` name.
_FS_MODULES = {
    "btrfs": "btrfs",
    "ext4": "ext4",
    "xfs": "xfs",
}

def test_modules(disks, extra = [], rootfs = False):
    """Returns the sorted module names a test needs.

    Args:
        disks: the test's (device, fstype, size) tuples.
        extra: modules the test needs beyond the defaults.
        rootfs: whether the test boots a Debian ext4 root filesystem.

    Returns:
        fuse (dcfs mounts a FUSE filesystem in every test), virtio_blk and
        the filesystem modules of the disks when there are any, and extra.
    """
    modules = {"fuse": True}
    for module in extra:
        modules[module] = True
    if disks or rootfs:
        modules["virtio_blk"] = True
    for disk in disks:
        modules[_FS_MODULES[disk[1]]] = True
    if rootfs:
        modules["ext4"] = True
    return sorted(modules.keys())

def modules_cpio(modules):
    """Declares (once per package) the archive for a module set.

    Args:
        modules: the sorted module names.

    Returns:
        The label of the archive, relative to the calling package.
    """
    name = "modules_" + "_".join([m.replace("-", "_") for m in modules])
    if not native.existing_rule(name + "_cpio"):
        native.genrule(
            name = name + "_cpio",
            testonly = 1,
            srcs = [
                "//third_party/linux:package",
                "//third_party/linux:vmlinuz",
            ],
            outs = [name + ".cpio.gz"],
            cmd = " ".join([
                "$(location //test/qemu:mkmodules_bin)",
                # The package's root directory is two levels above the image.
                "--root \"$$(dirname \"$$(dirname \"$(location //third_party/linux:vmlinuz)\")\")\"",
                "--out $@",
            ] + modules),
            tools = ["//test/qemu:mkmodules_bin"],
        )
    return ":" + name + ".cpio.gz"
