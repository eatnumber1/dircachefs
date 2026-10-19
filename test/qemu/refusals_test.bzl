# SPDX-License-Identifier: MIT
"""Pins every refusal of qemu_test, qemu_test_matrix and qemu_cc_test.

One skylib unit test per refusal, each asserting the exact message
(test/qemu/refusals.bzl); the macros pass that message to fail().
"""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(
    "//test/qemu:refusals.bzl",
    "initramfs_refusal",
    "mem_refusal",
    "qemu_cc_test_refusal",
    "qemu_test_matrix_refusal",
    "qemu_test_refusal",
)

_OK = {
    "boots": 1,
    "disks": [],
    "kernel_failure": None,
    "mem": None,
    "name": "t",
    "power_cut": [],
    "ram_disks": False,
    "rootfs": None,
    "size": "small",
    "systemd_image": None,
    "timeout": "short",
}

_TIER = "size and timeout are required (the test tier; see test/qemu/README.md)"
_RAM = {"ram_disks": True, "mem": 512}
_VDB = ("vdb", "ext4", "64")

def _qemu_test(**overrides):
    args = dict(_OK)
    args.update(overrides)
    return qemu_test_refusal(**args)

def _accepts_plain_call_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, None, _qemu_test())
    asserts.equals(env, None, _qemu_test(kernel_failure = "expected"))
    return unittest.end(env)

def _accepts_ram_disk_call_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, None, _qemu_test(disks = [_VDB], **_RAM))
    return unittest.end(env)

def _size_required_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, "qemu_test(t): " + _TIER, _qemu_test(size = None))
    return unittest.end(env)

def _timeout_required_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, "qemu_test(t): " + _TIER, _qemu_test(timeout = None))
    return unittest.end(env)

def _kernel_failure_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test(t): kernel_failure must be None or \"expected\"",
        _qemu_test(kernel_failure = "yes"),
    )
    return unittest.end(env)

def _systemd_and_rootfs_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test(t): systemd_image and rootfs exclude each other",
        _qemu_test(systemd_image = ":s", rootfs = ":r"),
    )
    return unittest.end(env)

def _boots_impl(ctx):
    env = unittest.begin(ctx)
    expected = "qemu_test(t): boots is 1 or more, and excludes power_cut"
    asserts.equals(env, expected, _qemu_test(boots = 0))
    asserts.equals(env, expected, _qemu_test(boots = 2, power_cut = ["x"]))
    return unittest.end(env)

def _ram_disks_exclusions_impl(ctx):
    env = unittest.begin(ctx)
    expected = (
        "qemu_test(t): ram_disks excludes power_cut, boots, rootfs and " +
        "systemd_image (the disks are in the guest's RAM)"
    )
    for overrides in [
        {"power_cut": ["x"]},
        {"boots": 2},
        {"rootfs": ":r"},
        {"systemd_image": ":s"},
    ]:
        asserts.equals(env, expected, _qemu_test(**dict(_RAM, **overrides)))
    return unittest.end(env)

def _ram_disks_mkfs_options_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test(t): ram_disks disks carry no mkfs options",
        _qemu_test(disks = [("vdb", "ext4", "64", "-O x")], **_RAM),
    )
    return unittest.end(env)

def _ram_disks_needs_mem_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test(t): ram_disks needs an explicit mem (the guest holds " +
        "its disks; size it from the MEM line, README.md \"Guest memory\")",
        _qemu_test(ram_disks = True),
    )
    return unittest.end(env)

def _ram_disks_vda_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test(t): a ram_disks disk cannot be named vda " +
        "(the coverage disk's name)",
        _qemu_test(disks = [("vda", "ext4", "64")], **_RAM),
    )
    return unittest.end(env)

def _ram_disks_scsi_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test(t): a ram_disks disk cannot be a SCSI disk",
        _qemu_test(disks = [("sda", "ext4", "64")], **_RAM),
    )
    return unittest.end(env)

def _disk_names_only_matter_for_ram_disks_impl(ctx):
    env = unittest.begin(ctx)
    disks = [("vda", "ext4", "64"), ("sda", "ext4", "64")]
    asserts.equals(env, None, _qemu_test(disks = disks))
    return unittest.end(env)

def _initramfs_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "plain_dcfs and checked_dcfs exclude each other",
        initramfs_refusal(True, True),
    )
    asserts.equals(env, None, initramfs_refusal(True, False))
    asserts.equals(env, None, initramfs_refusal(False, True))
    asserts.equals(env, None, initramfs_refusal(False, False))
    return unittest.end(env)

def _mem_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "asan_mem (256) is smaller than mem (512)",
        mem_refusal(512, 256),
    )
    asserts.equals(env, None, mem_refusal(512, 512))
    asserts.equals(env, None, mem_refusal(512, 1024))
    return unittest.end(env)

def _matrix_no_disks_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "qemu_test_matrix(m): disks must have at least one entry " +
        "(the backing source disk, vdb, whose fstype is varied)",
        qemu_test_matrix_refusal("m", [], "small", "medium", "short"),
    )
    return unittest.end(env)

def _matrix_tiers_impl(ctx):
    env = unittest.begin(ctx)
    expected = (
        "qemu_test_matrix(m): size, other_size and timeout are required"
    )
    disks = [_VDB]
    asserts.equals(
        env,
        None,
        qemu_test_matrix_refusal("m", disks, "small", "medium", "short"),
    )
    asserts.equals(
        env,
        expected,
        qemu_test_matrix_refusal("m", disks, None, "medium", "short"),
    )
    asserts.equals(
        env,
        expected,
        qemu_test_matrix_refusal("m", disks, "small", None, "short"),
    )
    asserts.equals(
        env,
        expected,
        qemu_test_matrix_refusal("m", disks, "small", "medium", None),
    )
    return unittest.end(env)

def _cc_test_tier_impl(ctx):
    env = unittest.begin(ctx)
    expected = "qemu_cc_test(c): " + _TIER
    asserts.equals(env, None, qemu_cc_test_refusal("c", "small", "short"))
    asserts.equals(env, expected, qemu_cc_test_refusal("c", None, "short"))
    asserts.equals(env, expected, qemu_cc_test_refusal("c", "small", None))
    return unittest.end(env)

# unittest.make calls rule(), which only works at the top of a .bzl.
_accepts_plain_call_test = unittest.make(_accepts_plain_call_impl)
_accepts_ram_disk_call_test = unittest.make(_accepts_ram_disk_call_impl)
_size_required_test = unittest.make(_size_required_impl)
_timeout_required_test = unittest.make(_timeout_required_impl)
_kernel_failure_test = unittest.make(_kernel_failure_impl)
_systemd_and_rootfs_test = unittest.make(_systemd_and_rootfs_impl)
_boots_test = unittest.make(_boots_impl)
_ram_disks_exclusions_test = unittest.make(_ram_disks_exclusions_impl)
_ram_disks_mkfs_options_test = unittest.make(_ram_disks_mkfs_options_impl)
_ram_disks_needs_mem_test = unittest.make(_ram_disks_needs_mem_impl)
_ram_disks_vda_test = unittest.make(_ram_disks_vda_impl)
_ram_disks_scsi_test = unittest.make(_ram_disks_scsi_impl)
_disk_names_test = unittest.make(_disk_names_only_matter_for_ram_disks_impl)
_initramfs_test = unittest.make(_initramfs_impl)
_mem_test = unittest.make(_mem_impl)
_matrix_no_disks_test = unittest.make(_matrix_no_disks_impl)
_matrix_tiers_test = unittest.make(_matrix_tiers_impl)
_cc_test_tier_test = unittest.make(_cc_test_tier_impl)

_TESTS = {
    "accepts_plain_call": _accepts_plain_call_test,
    "accepts_ram_disk_call": _accepts_ram_disk_call_test,
    "size_required": _size_required_test,
    "timeout_required": _timeout_required_test,
    "kernel_failure": _kernel_failure_test,
    "systemd_and_rootfs": _systemd_and_rootfs_test,
    "boots": _boots_test,
    "ram_disks_exclusions": _ram_disks_exclusions_test,
    "ram_disks_mkfs_options": _ram_disks_mkfs_options_test,
    "ram_disks_needs_mem": _ram_disks_needs_mem_test,
    "ram_disks_vda": _ram_disks_vda_test,
    "ram_disks_scsi": _ram_disks_scsi_test,
    "disk_names": _disk_names_test,
    "initramfs": _initramfs_test,
    "mem": _mem_test,
    "matrix_no_disks": _matrix_no_disks_test,
    "matrix_tiers": _matrix_tiers_test,
    "cc_test_tier": _cc_test_tier_test,
}

def refusals_test_suite(name):
    """Declares one small test per refusal and a test_suite `name` of them."""
    names = []
    for test, test_rule in _TESTS.items():
        test_name = name + "_" + test
        test_rule(name = test_name, size = "small")
        names.append(test_name)
    native.test_suite(name = name, tests = names)
