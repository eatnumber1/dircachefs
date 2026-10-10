"""qemu_test(name, guest_script, disks): a dcfs QEMU end-to-end test.

Boots the shared dcfs QEMU initramfs (:initramfs_checked for the small and
medium tiers, :initramfs for large and enormous; see initramfs_for) and test kernel
(//third_party/linux:vmlinuz, Alpine's linux-virt) under the pinned, Bazel-built
@alpine_qemu//:qemu_system_x86_64 (step 4.4), telling guest/init (via
the dcfs_test= kernel command-line parameter) to run the given guest_script,
found inside the initramfs at /tests/<basename of guest_script>. See
test/qemu/scripts/run-qemu.sh for the boot/verdict mechanics and
test/qemu/guest/init for the guest side.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//test/qemu:coverage.bzl", "E2E_COVERAGE_OBJECTS", "coverage_args", "coverage_data")
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

# Step 26.14: guest vCPUs. One for every test that does not need more: with one
# vCPU the kernel's concurrent activity (flusher threads, kswapd, the daemon
# and the client running at once) cannot land at a different moment in each
# run. A test whose point is concurrency keeps two: the stress tests
# (fsstress -p), the cancellation tests (requests in flight while a signal
# arrives) and the benchmarks (they measure the daemon's threads).
E2E_CPUS = 1
CONCURRENT_CPUS = 2

# `size` also sets Bazel's default resource estimate (small assumes about 20
# MB), which is wrong for a QEMU guest, so each test declares its real needs
# as tags. Bazel's scheduling resources are a tag, which cannot depend on the build
# configuration, so a test declares its plain allowance plus the QEMU
# process's own overhead (QEMU_OVERHEAD_MB); an ASan run schedules as if it
# were a plain one. Use fewer test jobs for ASan runs (--local_test_jobs).
QEMU_OVERHEAD_MB = 100

# Step 26.2: the dcfs a guest boots, by tier. The fast and presubmit tiers
# (small, medium) run the testonly checking build (:initramfs_checked,
# //dcfs:main_static_checked: docs/design.md, "Runtime invariant checks");
# the large and enormous tiers the plain build that ships (:initramfs).
# //test/qemu:invariant_checks_on_test checks that a small test gets the
# checked one.
def initramfs_for(size, plain_dcfs = False, checked_dcfs = False):
    """The initramfs label a qemu_test of tier `size` boots."""
    if plain_dcfs and checked_dcfs:
        fail("plain_dcfs and checked_dcfs exclude each other")
    if plain_dcfs:
        return ":initramfs"
    if checked_dcfs:
        return ":initramfs_checked"
    return ":initramfs_checked" if size in ("small", "medium") else ":initramfs"

def mem_args_for(mem, asan_mem):
    """run-qemu.sh's --mem flag for the build configuration (a select())."""
    sanitizer_args = ["--mem", str(asan_mem)]
    return select({
        "//test/qemu:asan_build": sanitizer_args,
        "//test/qemu:ubsan_build": sanitizer_args,
        "//conditions:default": ["--mem", str(mem)],
    })

def sanitizer_args():
    """run-qemu.sh --cmdline dcfs_sanitizer=<asan|ubsan> (a select()); guest/init exports it as DCFS_SANITIZER."""
    return select({
        "//test/qemu:asan_build": ["--cmdline", "dcfs_sanitizer=asan"],
        "//test/qemu:ubsan_build": ["--cmdline", "dcfs_sanitizer=ubsan"],
        "//conditions:default": [],
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

def qemu_test(name, guest_script, size = None, timeout = None, disks = [], rootfs = None, mem = None, asan_mem = None, modules = [], kernel_failure = None, plain_dcfs = False, power_cut = [], cmdline = "", checked_dcfs = False, tags = [], cpus = E2E_CPUS, systemd_image = None, boots = 1, target_compatible_with = []):
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
        target_compatible_with: sh_test's, for a test that cannot run in some
            build configurations (step 17.1: the xfstests shards are
            incompatible with --config=asan and --config=ubsan).
        cpus: the guest's vCPUs (run-qemu.sh --cpus; step 26.14), also the
            basis of the Bazel `cpu:` tag. Default E2E_CPUS (1); pass
            CONCURRENT_CPUS for a test whose point is concurrency (stress,
            cancellation, benchmarks).
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
        plain_dcfs: boot the plain dcfs that ships (:initramfs) whatever
            the tier (step 26.2), for a test that measures the shipped
            binary's own costs: memory_test its RSS and
            readdir_boundary_test a listing's time, which the checking
            build's queries and bookkeeping would add to.
        power_cut: list of scenario names (step 11.2): instead of one boot,
            run-qemu.sh --power-cut boots the guest twice per scenario over
            the same disk images, killing QEMU at the cut the first time (a
            real power loss) and giving the second boot's verdict; the guest
            script reads dcfs_cut=kill, dcfs_scenario= and dcfs_boot= from
            its kernel command line (guest/fault_power.sh).
        checked_dcfs: boot the checking build of dcfs (:initramfs_checked)
            although the tier is large, for a large test that exercises what
            the invariant checks watch (the ACE sequences' recoveries).
        cmdline: extra words for the guest's kernel command line (run-qemu.sh
            --cmdline), which the guest script reads from /proc/cmdline.
        systemd_image: optional label of a released Debian cloud image
            (normally "//third_party/debian_cloud:image", step 15.6) to boot
            with systemd as PID 1: run-qemu.sh --systemd-image puts an
            overlay of it on a virtio disk and names its root partition as
            dcfs_systemd= on the kernel command line, and guest/init
            switches into it, with dcfs and the test installed, instead of
            running the guest script itself (the script runs from a oneshot
            unit). See third_party/debian_cloud/README.md.
        boots: how many times the guest boots over the same disks
            (run-qemu.sh --boots; the guest script reads dcfs_boot= and
            dcfs_boots= from its kernel command line and reboots itself
            between the boots). Default 1.
        tags: extra sh_test tags, e.g. ["manual"] for a test that only runs
            when asked for by name (step 11.2b: stress_random_test).
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
    power_cut_args = ["--power-cut", ",".join(power_cut)] if power_cut else []
    power_cut_args += ["--cmdline", "'" + cmdline + "'"] if cmdline else []

    if systemd_image and rootfs:
        fail("qemu_test(%s): systemd_image and rootfs exclude each other" % name)
    if boots < 1 or (boots > 1 and power_cut):
        fail("qemu_test(%s): boots is 1 or more, and excludes power_cut" % name)
    systemd_data = [systemd_image, "@alpine_qemu_img//:qemu_img"] if systemd_image else []
    systemd_args = [
        "--systemd-image",
        "$(location " + systemd_image + ")",
        "--qemu-img",
        "$(location @alpine_qemu_img//:qemu_img)",
    ] if systemd_image else []
    boots_args = ["--boots", str(boots)] if boots > 1 else []

    rootfs_data = [rootfs] if rootfs else []
    rootfs_args = ["--rootfs", "$(location " + rootfs + ")"] if rootfs else []

    # The test kernel is Alpine's linux-virt (step 24.2): //third_party/linux:
    # vmlinuz. Its drivers are modules; the initramfs gets the archive of the
    # ones this test needs (modules.bzl), which run-qemu.sh appends.
    modules_archive = modules_cpio(test_modules(disks, modules, rootfs != None or systemd_image != None))
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
    resource_tags = ["cpu:%d" % cpus, "resources:memory:%d" % (mem + QEMU_OVERHEAD_MB)]

    initramfs = initramfs_for(size, plain_dcfs, checked_dcfs)
    # Under `bazel coverage`, the dcfs that wrote the profiles: the one this
    # guest boots.
    cov_objects = [
        "//dcfs:main_static_checked" if o == "//dcfs:main_static" and initramfs == ":initramfs_checked" else o
        for o in E2E_COVERAGE_OBJECTS
    ]
    sh_test(
        name = name,
        srcs = ["scripts/run-qemu.sh"],
        data = kernel_data + qemu_data + [
            initramfs,
            guest_script,
        ] + rootfs_data + systemd_data + coverage_data(cov_objects),
        args = qemu_args + ["--cpus", str(cpus)] + coverage_args(cov_objects) + kernel_failure_args + power_cut_args + boots_args + rootfs_args + systemd_args + mem_args + sanitizer_args() + kernel_args + [
            "$(location " + initramfs + ")",
            guest_script_basename,
        ] + disk_args,
        tags = [
            "e2e",
            "no-sandbox",
            "requires-kvm",
        ] + resource_tags + tags,
        size = size,
        timeout = timeout,
        target_compatible_with = target_compatible_with,
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
        fstypes = ["ext4", "xfs", "btrfs"],
        plain_dcfs = False,
        power_cut = [],
        cmdline = "",
        checked_dcfs = False,
        tags = [],
        cpus = E2E_CPUS):
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
        plain_dcfs: same as qemu_test.
        power_cut: same as qemu_test.
        cmdline: same as qemu_test.
        checked_dcfs: same as qemu_test.
        cpus: same as qemu_test.
        tags: same as qemu_test.
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
            plain_dcfs = plain_dcfs,
            power_cut = power_cut,
            cmdline = cmdline,
            checked_dcfs = checked_dcfs,
            tags = tags,
            cpus = cpus,
        )
    native.alias(
        name = name,
        actual = ":" + name + "_" + fstypes[0],
    )
