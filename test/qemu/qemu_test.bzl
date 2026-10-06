"""qemu_test(name, guest_script, disks): a dcfs QEMU end-to-end test.

Boots the shared dcfs QEMU initramfs (:initramfs) and test kernel
(//third_party/linux:bzImage) under the pinned, Bazel-built
//third_party/qemu:qemu_system_x86_64 (step 4.4), telling guest/init (via
the dcfs_test= kernel command-line parameter) to run the given guest_script,
found inside the initramfs at /tests/<basename of guest_script>. See
test/qemu/scripts/run-qemu.sh for the boot/verdict mechanics and
test/qemu/guest/init for the guest side.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

# Guest resources, declared to Bazel's scheduler: `size` also sets the
# default resource estimate (small assumes ~20 MB), which is wrong for a
# QEMU guest. run-qemu.sh gives an e2e guest 1024 MB and -smp 2; the QEMU
# process itself needs a little more than the guest RAM.
E2E_RESOURCE_TAGS = ["cpu:2", "resources:memory:1200"]

def qemu_test(name, guest_script, size = None, timeout = None, disks = [], rootfs = None, mem = None):
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
        mem: optional guest RAM in MiB (run-qemu.sh's --mem; default 1024).
            Also raises the Bazel resource estimate to match.
        size: required sh_test size, the test's tier: "small" (run
            constantly), "medium" (presubmit), "large"/"enormous" (CI).
            See README.md's "Test tiers".
        timeout: required sh_test timeout; explicit because the size's
            default is not tuned to a guest.
        timeout: sh_test timeout, e.g. "eternal" (3600s) to pair with
            size = "enormous"; defaults to "long" (900s), unchanged from
            before this parameter existed.
    """
    if size == None or timeout == None:
        fail("qemu_test(%s): size and timeout are required (the test tier; see test/qemu/README.md)" % name)
    disk_args = [d[0] + ":" + d[1] + ":" + d[2] for d in disks]
    guest_script_basename = guest_script.split("/")[-1]

    rootfs_data = [rootfs] if rootfs else []
    rootfs_args = ["--rootfs", "$(location " + rootfs + ")"] if rootfs else []

    # Step 3.2 dropped the FUSE_ATTR_GENERATION kernel patch; step 4.4
    # removed the deprecated out-of-tree "patched" kernel entirely (and
    # with it the //test/qemu:kernel string_flag) -- every test now boots
    # the pinned, Bazel-built //third_party/linux:bzImage unconditionally.
    kernel_data = ["//third_party/linux:bzImage"]
    kernel_args = ["$(location //third_party/linux:bzImage)"]

    # Step 4.4: the Bazel-built QEMU and qboot ROM, passed explicitly --
    # run-qemu.sh does no host lookup of its own. See run-qemu.sh's usage
    # comment and test/qemu/README.md.
    qemu_data = [
        "//third_party/qemu:qemu_system_x86_64",
        "@qemu//:pc-bios/qboot.rom",
        # R3 (L5): the pinned mkfs tools for the scratch disks.
        "//third_party/btrfs-progs:mkfs_btrfs",
        "//third_party/e2fsprogs:mke2fs",
        "//third_party/e2fsprogs:mke2fs.conf",
        "//third_party/xfsprogs:mkfs_xfs",
    ]
    qemu_args = [
        "--mke2fs",
        "$(location //third_party/e2fsprogs:mke2fs)",
        "--mke2fs-conf",
        "$(location //third_party/e2fsprogs:mke2fs.conf)",
        "--mkfs-xfs",
        "$(location //third_party/xfsprogs:mkfs_xfs)",
        "--mkfs-btrfs",
        "$(location //third_party/btrfs-progs:mkfs_btrfs)",
        "--qemu",
        "$(location //third_party/qemu:qemu_system_x86_64)",
        "--qboot",
        "$(location @qemu//:pc-bios/qboot.rom)",
    ]

    mem_args = ["--mem", str(mem)] if mem else []
    resource_tags = E2E_RESOURCE_TAGS
    if mem:
        resource_tags = ["cpu:2", "resources:memory:%d" % (mem + 200)]

    sh_test(
        name = name,
        srcs = ["scripts/run-qemu.sh"],
        data = kernel_data + qemu_data + [
            ":initramfs",
            guest_script,
        ] + rootfs_data,
        args = qemu_args + rootfs_args + mem_args + kernel_args + [
            "$(location :initramfs)",
            guest_script_basename,
        ] + disk_args,
        tags = [
            "e2e",
            "no-sandbox",
            "requires-kvm",
        ] + resource_tags,
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
        size = None,
        other_size = None,
        timeout = None,
        rootfs = None,
        mem = None,
        fstypes = ["ext4", "xfs", "btrfs"]):
    """Declares one qemu_test per backing filesystem in `fstypes`.

    Args:
        name: the base name; generates "<name>_<fstype>" per fstypes entry
            plus a plain "<name>" alias to the first (ext4) variant.
        guest_script: same as qemu_test.
        disks: same shape as qemu_test's `disks`, but the first entry's
            fstype field is overridden per generated variant -- pass
            whatever placeholder fstype reads best (by convention "ext4").
        size: required; the tier of the first (ext4) variant, which
            "<name>" aliases.
        other_size: required; the tier of the remaining variants (xfs,
            btrfs), normally one tier up from `size`.
        timeout: required; same as qemu_test.
        rootfs: same as qemu_test.
        mem: same as qemu_test.
        fstypes: filesystems to generate variants for, in order; the first
            is what plain "<name>" aliases to.
    """
    if not disks:
        fail("qemu_test_matrix(%s): disks must have at least one entry " % name +
             "(the backing source disk, vdb, whose fstype is varied)")
    if size == None or other_size == None or timeout == None:
        fail("qemu_test_matrix(%s): size, other_size and timeout are required" % name)
    for fstype in fstypes:
        varied_disks = [(disks[0][0], fstype, disks[0][2])] + list(disks[1:])
        qemu_test(
            name = name + "_" + fstype,
            guest_script = guest_script,
            disks = varied_disks,
            size = size if fstype == fstypes[0] else other_size,
            timeout = timeout,
            rootfs = rootfs,
            mem = mem,
        )
    native.alias(
        name = name,
        actual = ":" + name + "_" + fstypes[0],
    )
