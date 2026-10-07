"""qemu_test(name, guest_script, disks): a dcfs QEMU end-to-end test.

Boots the shared dcfs QEMU initramfs (:initramfs) and test kernel
(//third_party/linux:vmlinuz, Alpine's linux-virt) under the pinned, Bazel-built
@alpine_qemu//:qemu_system_x86_64 (step 4.4), telling guest/init (via
the dcfs_test= kernel command-line parameter) to run the given guest_script,
found inside the initramfs at /tests/<basename of guest_script>. See
test/qemu/scripts/run-qemu.sh for the boot/verdict mechanics and
test/qemu/guest/init for the guest side.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//test/qemu:modules.bzl", "modules_cpio", "test_modules")

# Guest memory (step 6.2). Every guest has an allowance, in MiB, passed to
# run-qemu.sh as --mem; the defaults below are for a test that is no bigger
# than the lightest. The measurements and the rule they follow (peak +
# max(50%, 128 MiB), the peak counting the kernel's own reservation) are in
# test/qemu/README.md "Guest memory"; every guest prints its own peak on a
# `MEM ...` line (guest/init), so a change that grows one shows up in the
# serial log, and a guest that runs out of memory fails with the OOM
# killer's lines (run-qemu.sh).
#
# Sanitizer builds need more: the ASan runtime's libraries land in the
# initramfs (about 60 MiB of tmpfs the plain static build does not have),
# dcfs's resident set grows several times over, and so does the benchmark
# and test binaries'. mem= is the plain build's allowance and asan_mem= the
# --config=asan and --config=ubsan builds' (picked by select() on
# //test/qemu:asan_build and :ubsan_build; the UBSan runtime is a smaller
# version of the same, so it shares ASan's number).
E2E_MEM = 256
E2E_ASAN_MEM = 384

# `size` also sets Bazel's default resource estimate (small assumes about 20
# MB), which is wrong for a QEMU guest, so each test declares its real needs
# as tags. Bazel's scheduling resources are a tag, which cannot depend on the build
# configuration, so a test declares its plain allowance plus the QEMU
# process's own overhead (QEMU_OVERHEAD_MB); an ASan run schedules as if it
# were a plain one. Use fewer test jobs for ASan runs (--local_test_jobs).
QEMU_OVERHEAD_MB = 100

def mem_args_for(mem, asan_mem):
    """run-qemu.sh's --mem flag for the build configuration (a select())."""
    sanitizer_args = ["--mem", str(asan_mem)]
    return select({
        "//test/qemu:asan_build": sanitizer_args,
        "//test/qemu:ubsan_build": sanitizer_args,
        "//conditions:default": ["--mem", str(mem)],
    })

def resolve_mem(mem, asan_mem, default, asan_default):
    """The (plain, sanitizer) allowances; neither is ever below the other's floor."""
    if mem == None:
        mem = default
    if asan_mem == None:
        asan_mem = max(asan_default, mem)
    if asan_mem < mem:
        fail("asan_mem (%d) is smaller than mem (%d)" % (asan_mem, mem))
    return mem, asan_mem

def qemu_test(name, guest_script, size = None, timeout = None, disks = [], rootfs = None, mem = None, asan_mem = None, modules = [], kernel_failure = None):
    """Declares a QEMU end-to-end test.

    Args:
        name: the test target's name.
        guest_script: label of the guest/*.sh script to run inside the
            guest (must already be baked into :initramfs, i.e. matched by
            the guest/*.sh glob in this package's initramfs genrule).
        disks: list of (device, fstype, size) tuples, e.g.
            ("vdb", "ext4", "256M"). device is a /dev/vd<letter> name;
            an ext4 tuple may have a fourth element, mke2fs options
            (e.g. "-O casefold -E encoding=utf8", no colons);
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
        mem: guest RAM in MiB for the plain build (run-qemu.sh's --mem;
            default E2E_MEM), also the basis of the Bazel resource tag.
        asan_mem: guest RAM in MiB for --config=asan/ubsan builds (default:
            E2E_ASAN_MEM, or mem if that is larger).
        modules: kernel modules this test needs beyond the defaults (fuse,
            and virtio_blk plus the filesystem modules of `disks` and
            `rootfs`), e.g. ["nfsd", "nfsv4"]; the guest loads them with
            their dependencies before the test runs (modules.bzl,
            third_party/linux/README.md).
        kernel_failure: None (the default: a kernel oops, BUG, WARNING or
            panic in the serial log fails the run) or "expected" (step 23.7):
            run-qemu.sh --expect-kernel-failure <guest script>, for the one
            test whose guest script reproduces a kernel bug as a DISABLED_
            check (casefold_tune_oops_test). The oops is tolerated only
            if the guest reports it as "would FAIL (kernel: ...)"; the
            verdict follows its other checks. Never for a test of dcfs.
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
    # A disk-spec with mke2fs options has spaces: Bazel shell-tokenizes `args`.
    disk_args = ["'" + ":".join(d) + "'" if len(d) > 3 else ":".join(d) for d in disks]
    guest_script_basename = guest_script.split("/")[-1]

    if kernel_failure not in (None, "expected"):
        fail("qemu_test(%s): kernel_failure must be None or \"expected\"" % name)
    kernel_failure_args = ["--expect-kernel-failure", guest_script.split("/")[-1]] if kernel_failure else []

    rootfs_data = [rootfs] if rootfs else []
    rootfs_args = ["--rootfs", "$(location " + rootfs + ")"] if rootfs else []

    # The test kernel is Alpine's linux-virt (step 24.2): //third_party/linux:
    # vmlinuz. Its drivers are modules; the initramfs gets the archive of the
    # ones this test needs (modules.bzl), which run-qemu.sh appends.
    modules_archive = modules_cpio(test_modules(disks, modules, rootfs != None))
    kernel_data = ["//third_party/linux:vmlinuz", modules_archive]
    kernel_args = [
        "--modules",
        "$(location " + modules_archive + ")",
        "$(location //third_party/linux:vmlinuz)",
    ]

    # Step 4.4: the Alpine's QEMU and qboot ROM, passed explicitly --
    # run-qemu.sh does no host lookup of its own. See run-qemu.sh's usage
    # comment and test/qemu/README.md.
    qemu_data = [
        "@alpine_qemu//:qemu_system_x86_64",
        "@alpine_qemu//:root/usr/share/qemu/qboot.rom",
        # R3 (L5): the pinned mkfs tools for the scratch disks.
        "@alpine_fstools//:mkfs_btrfs",
        "@alpine_fstools//:mke2fs",
        "//third_party/e2fsprogs:mke2fs.conf",
        "@alpine_fstools//:mkfs_xfs",
    ]
    qemu_args = [
        "--mke2fs",
        "$(location @alpine_fstools//:mke2fs)",
        "--mke2fs-conf",
        "$(location //third_party/e2fsprogs:mke2fs.conf)",
        "--mkfs-xfs",
        "$(location @alpine_fstools//:mkfs_xfs)",
        "--mkfs-btrfs",
        "$(location @alpine_fstools//:mkfs_btrfs)",
        "--qemu",
        "$(location @alpine_qemu//:qemu_system_x86_64)",
        "--qboot",
        "$(location @alpine_qemu//:root/usr/share/qemu/qboot.rom)",
    ]

    mem, asan_mem = resolve_mem(mem, asan_mem, E2E_MEM, E2E_ASAN_MEM)
    mem_args = mem_args_for(mem, asan_mem)
    resource_tags = ["cpu:2", "resources:memory:%d" % (mem + QEMU_OVERHEAD_MB)]

    sh_test(
        name = name,
        srcs = ["scripts/run-qemu.sh"],
        data = kernel_data + qemu_data + [
            ":initramfs",
            guest_script,
        ] + rootfs_data,
        args = qemu_args + kernel_failure_args + rootfs_args + mem_args + kernel_args + [
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
        asan_mem = None,
        modules = [],
        fstypes = ["ext4", "xfs", "btrfs"]):
    """Declares one qemu_test per backing filesystem in `fstypes`.

    Args:
        name: the base name; generates "<name>_<fstype>" per fstypes entry
            plus a plain "<name>" alias to the first (ext4) variant.
        guest_script: same as qemu_test.
        disks: same shape as qemu_test's `disks`, but the first entry's
            fstype field is overridden per generated variant -- pass
            whatever placeholder fstype reads best (by convention "ext4").
            An optional fourth element (mke2fs options) is kept for the
            ext4 variant only.
        size: required; the tier of the first (ext4) variant, which
            "<name>" aliases.
        other_size: required; the tier of the remaining variants (xfs,
            btrfs), normally one tier up from `size`.
        timeout: required; same as qemu_test.
        rootfs: same as qemu_test.
        mem: same as qemu_test.
        asan_mem: same as qemu_test.
        modules: same as qemu_test.
        fstypes: filesystems to generate variants for, in order; the first
            is what plain "<name>" aliases to.
    """
    if not disks:
        fail("qemu_test_matrix(%s): disks must have at least one entry " % name +
             "(the backing source disk, vdb, whose fstype is varied)")
    if size == None or other_size == None or timeout == None:
        fail("qemu_test_matrix(%s): size, other_size and timeout are required" % name)
    for fstype in fstypes:
        # The mkfs options (a fourth element) are ext4's: the other variants
        # drop them.
        first = (disks[0][0], fstype, disks[0][2])
        if fstype == "ext4":
            first = tuple(disks[0])
        varied_disks = [first] + list(disks[1:])
        qemu_test(
            name = name + "_" + fstype,
            guest_script = guest_script,
            disks = varied_disks,
            size = size if fstype == fstypes[0] else other_size,
            timeout = timeout,
            rootfs = rootfs,
            mem = mem,
            asan_mem = asan_mem,
            modules = modules,
        )
    native.alias(
        name = name,
        actual = ":" + name + "_" + fstypes[0],
    )
