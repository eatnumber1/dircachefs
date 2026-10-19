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

def qemu_test_refusal(name, size, timeout, kernel_failure, systemd_image, rootfs, boots, power_cut, ram_disks, mem, disks):
    """Why qemu_test(name, ...) refuses this call, or None."""
    if size == None or timeout == None:
        return "qemu_test(%s): size and timeout are required (the test tier; see test/qemu/README.md)" % name
    if kernel_failure not in (None, "expected"):
        return "qemu_test(%s): kernel_failure must be None or \"expected\"" % name
    if systemd_image and rootfs:
        return "qemu_test(%s): systemd_image and rootfs exclude each other" % name
    if boots < 1 or (boots > 1 and power_cut):
        return "qemu_test(%s): boots is 1 or more, and excludes power_cut" % name
    if ram_disks and (power_cut or boots > 1 or rootfs or systemd_image):
        return "qemu_test(%s): ram_disks excludes power_cut, boots, rootfs and systemd_image (the disks are in the guest's RAM)" % name
    if ram_disks and [d for d in disks if len(d) > 3]:
        return "qemu_test(%s): ram_disks disks carry no mkfs options" % name

    # The guest holds its disks: the 256 MiB default has no room for them, and
    # a test that forgot to size it would run with a warning every time.
    if ram_disks and mem == None:
        return "qemu_test(%s): ram_disks needs an explicit mem (the guest holds its disks; size it from the MEM line, README.md \"Guest memory\")" % name

    # The profile disk of `bazel coverage` is /dev/vda when no virtio disk is
    # attached, so a RAM disk of that name would collide with it.
    if ram_disks and [d for d in disks if d[0] == "vda"]:
        return "qemu_test(%s): a ram_disks disk cannot be named vda (the coverage disk's name)" % name
    if ram_disks and [d for d in disks if d[0].startswith("sd")]:
        return "qemu_test(%s): a ram_disks disk cannot be a SCSI disk" % name
    return None

def qemu_test_matrix_refusal(name, disks, size, other_size, timeout):
    """Why qemu_test_matrix(name, ...) refuses this call, or None."""
    if not disks:
        return ("qemu_test_matrix(%s): disks must have at least one entry " % name +
                "(the backing source disk, vdb, whose fstype is varied)")
    if size == None or other_size == None or timeout == None:
        return "qemu_test_matrix(%s): size, other_size and timeout are required" % name
    return None

def qemu_cc_test_refusal(name, size, timeout):
    """Why qemu_cc_test(name, ...) refuses this call, or None."""
    if size == None or timeout == None:
        return "qemu_cc_test(%s): size and timeout are required (the test tier; see test/qemu/README.md)" % name
    return None
