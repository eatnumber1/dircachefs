"""xfstests_test: a QEMU test of the xfstests generic tests (step 17.1).

Every target that runs xfstests goes through this macro, so that it always has
the tags .github/ci/test.sh keys on (the `xfstests` job runs them, the full,
asan and ubsan partitions leave them out), the two vCPUs the measured runs had
(since step 26.14 the default is one) and the incompatibility with the
sanitizer configurations (test/qemu/README.md, "xfstests (Phase 17)").
//test/qemu:xfstests_tags_test fails if BUILD.bazel starts an xfstests guest
script any other way.
"""

load(":qemu_test.bzl", "CONCURRENT_CPUS", "qemu_test")

# Not under the sanitizers: the helpers and the tools of the guest's image are
# glibc programs next to a musl userland, which the sanitizer builds (dynamic,
# with the sanitizer runtime) cannot be put in; a shard run under ASan failed
# within 36 s relocating the preloaded error-message library.
XFSTESTS_COMPATIBLE = select({
    "//test/qemu:asan_build": ["@platforms//:incompatible"],
    "//test/qemu:ubsan_build": ["@platforms//:incompatible"],
    "//conditions:default": [],
})

def xfstests_test(name, fstype, guest_script, tags = [], mem = 1024, **kwargs):
    """A qemu_test of an xfstests guest script on a backing file system.

    Args:
        name: the target's name.
        fstype: ext4, xfs or btrfs: the backing file system of both disks; the
            test also gets the tag xfstests-<fstype>.
        guest_script: the guest/xfstests*.sh script.
        tags: more tags (the slow and native variants add `manual`).
        mem: guest RAM in MiB (measured: README.md, "Guest memory").
        **kwargs: for qemu_test (cmdline, ...).
    """
    qemu_test(
        name = name,
        disks = [("vdb", fstype, "1G"), ("vdc", fstype, "1G")],
        cpus = CONCURRENT_CPUS,
        guest_script = guest_script,
        mem = mem,
        rootfs = "//test/qemu:xfstests_rootfs",
        size = "enormous",
        tags = ["xfstests", "xfstests-" + fstype] + tags,
        target_compatible_with = XFSTESTS_COMPATIBLE,
        timeout = "eternal",
        **kwargs
    )
