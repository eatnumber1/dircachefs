# SPDX-License-Identifier: MIT
"""The calls qemu_test, qemu_test_matrix and qemu_cc_test refuse.

Each function returns the refusal message, or None for an acceptable call; the
macros pass it to fail(). A fail() in a macro cannot be caught by Bazel's
analysis tests, but a function that returns the message can be called from a
skylib unit test (test/qemu/refusals_test.bzl), which pins every message.
"""

def initramfs_refusal(plain_dcfs, checked_dcfs):
    """Why initramfs_for refuses these arguments, or None."""
    if plain_dcfs and checked_dcfs:
        return "plain_dcfs and checked_dcfs exclude each other"
    return None

def mem_refusal(mem, asan_mem):
    """Why resolve_mem refuses the resolved allowances, or None."""
    if asan_mem < mem:
        return "asan_mem (%d) is smaller than mem (%d)" % (asan_mem, mem)
    return None

_TIER = "size and timeout are required (the test tier; see test/qemu/README.md)"

def qemu_test_refusal(
        name,
        size,
        timeout,
        kernel_failure,
        systemd_image,
        rootfs,
        boots,
        power_cut,
        ram_disks,
        mem,
        disks):
    """Why qemu_test(name, ...) refuses this call, or None."""
    prefix = "qemu_test(%s): " % name
    if size == None or timeout == None:
        return prefix + _TIER
    if kernel_failure not in (None, "expected"):
        return prefix + "kernel_failure must be None or \"expected\""
    if systemd_image and rootfs:
        return prefix + "systemd_image and rootfs exclude each other"
    if boots < 1 or (boots > 1 and power_cut):
        return prefix + "boots is 1 or more, and excludes power_cut"
    if ram_disks and (power_cut or boots > 1 or rootfs or systemd_image):
        return prefix + (
            "ram_disks excludes power_cut, boots, rootfs and systemd_image " +
            "(the disks are in the guest's RAM)"
        )
    if ram_disks and [d for d in disks if len(d) > 3]:
        return prefix + "ram_disks disks carry no mkfs options"

    # The guest holds its disks: the 256 MiB default has no room for them, and
    # a test that forgot to size it would run with a warning every time.
    if ram_disks and mem == None:
        return prefix + (
            "ram_disks needs an explicit mem (the guest holds its disks; " +
            "size it from the MEM line, README.md \"Guest memory\")"
        )

    # The profile disk of `bazel coverage` is /dev/vda when no virtio disk is
    # attached, so a RAM disk of that name would collide with it.
    if ram_disks and [d for d in disks if d[0] == "vda"]:
        return prefix + (
            "a ram_disks disk cannot be named vda (the coverage disk's name)"
        )
    if ram_disks and [d for d in disks if d[0].startswith("sd")]:
        return prefix + "a ram_disks disk cannot be a SCSI disk"
    return None

def qemu_test_matrix_refusal(name, disks, size, other_size, timeout):
    """Why qemu_test_matrix(name, ...) refuses this call, or None."""
    prefix = "qemu_test_matrix(%s): " % name
    if not disks:
        return prefix + (
            "disks must have at least one entry " +
            "(the backing source disk, vdb, whose fstype is varied)"
        )
    if size == None or other_size == None or timeout == None:
        return prefix + "size, other_size and timeout are required"
    return None

def qemu_cc_test_refusal(name, size, timeout):
    """Why qemu_cc_test(name, ...) refuses this call, or None."""
    if size == None or timeout == None:
        return "qemu_cc_test(%s): %s" % (name, _TIER)
    return None
