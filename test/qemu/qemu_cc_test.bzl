"""qemu_cc_test(name, srcs, deps, ...): a dcfs unit test that runs inside a
QEMU guest, root, on the project's own kernel.

PROJECT DECISION: dcfs requires root (real open_by_handle_at,
FS_IOC_GETFSUUID, etc.), so there is no host-side test execution -- every
test, including plain unit tests, boots a minimal kernel
(//third_party/linux:vmlinuz) under the Alpine's QEMU
(@alpine_qemu//:qemu_system_x86_64, step 4.4; see test/qemu/README.md
for the boot-time budget this depends on) and runs as root inside it. This
is the replacement for a plain cc_test.

Builds:
  - `<name>_bin`: a testonly cc_binary from `srcs`/`deps`, statically
    linked (see the fully_static_link note below).
  - `<name>.cpio.gz`: a per-test initramfs (scripts/mkinitramfs.sh) with
    busybox, guest/init, the test binary at /test/run, any `data` files
    under /test/data/ (relative paths preserved), a /test/args file
    holding `args` (space-joined; guest/init runs `/test/run $(cat
    /test/args)`), and -- if `disks` is given -- a /test/disk0 marker so
    guest/init mounts the first disk at /tmp/test_tmpdir instead of
    tmpfs.
  - `<name>`: an sh_test running scripts/run-qemu.sh --unit, which passes
    iff the serial log contains DCFS-TEST-EXIT=0.

fully_static_link: normal builds link `<name>_bin` fully statically (no
dynamic loader in the initramfs). glibc cannot be fully statically linked
with ASan (or UBSan's runtime, which has the same problem in practice), so
under `--config=asan`/`--config=ubsan` (detected via the sanitizer
config_settings below, set by --define=dcfs_sanitizer=... in .bazelrc)
the binary is linked dynamically instead, and mkinitramfs.sh copies its
ldd(1) closure (and the ELF interpreter) into the initramfs at the same
absolute paths so the dynamic loader finds them with no rpath surgery.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//test/qemu:modules.bzl", "modules_cpio", "test_modules")
load("//test/qemu:qemu_test.bzl", "QEMU_OVERHEAD_MB", "mem_args_for", "resolve_mem")

# Unit guests (step 6.2): the plain tests peak at 35 MiB (MemTotal - MemAvailable
# at its lowest), the ASan ones at 234 MiB (the sanitizer libraries in the
# initramfs, 83 MiB, and the test binary's resident set). The rule and the
# table are in test/qemu/README.md "Guest memory".
UNIT_MEM = 192
UNIT_ASAN_MEM = 384

def _relpath(label):
    """Best-effort package-relative path for a same-package source label."""
    s = str(label)
    if s.startswith(":"):
        return s[1:]
    if "//" in s:
        s = s.split("//", 1)[1]
    if ":" in s:
        pkg, name = s.split(":", 1)
        return name
    return s

def qemu_cc_test(
        name,
        srcs,
        deps = [],
        data = [],
        args = [],
        disks = [],
        size = None,
        timeout = None,
        tags = [],
        mem = None,
        asan_mem = None,
        modules = [],
        **kwargs):
    """Declares a dcfs unit test that boots the QEMU guest to run it.

    Args:
        name: the test target's name.
        srcs: cc_binary srcs for the test.
        deps: cc_binary deps for the test.
        data: source files installed under /test/data/ in the guest
            (TEST_SRCDIR), relative paths preserved (same-package labels
            only).
        args: command-line arguments for the test binary, written to
            /test/args (space-joined) and word-split by guest/init.
        disks: optional list of (device, fstype, size) tuples, e.g.
            [("vdb", "ext4", "64M")]; the first one is mounted at
            /tmp/test_tmpdir (so TEST_TMPDIR is a real disk instead of
            tmpfs) by guest/init. See run-qemu.sh for the device-letter
            convention.
        size: required sh_test size, the test's tier (small, medium,
            large, enormous; see test/qemu/README.md "Test tiers").
        timeout: required sh_test timeout.
        tags: extra tags, in addition to the ones this macro always sets.
        mem: guest RAM in MiB for the plain build (default UNIT_MEM), also the
            basis of the Bazel resource tag.
        asan_mem: guest RAM in MiB for --config=asan/ubsan builds (default
            UNIT_ASAN_MEM, or mem if that is larger).
        modules: kernel modules the test needs beyond the defaults (fuse,
            and virtio_blk plus the filesystem modules of `disks`); see
            qemu_test.bzl.
        **kwargs: forwarded to the underlying cc_binary (e.g. extra
            copts).
    """
    if size == None or timeout == None:
        fail("qemu_cc_test(%s): size and timeout are required (the test tier; see test/qemu/README.md)" % name)
    bin_name = name + "_bin"
    initramfs_out = name + ".cpio.gz"

    cc_binary(
        name = bin_name,
        testonly = 1,
        srcs = srcs,
        deps = deps,
        linkstatic = True,
        features = select({
            "//test/qemu:asan_build": [],
            "//test/qemu:ubsan_build": [],
            "//conditions:default": ["fully_static_link"],
        }),
        **kwargs
    )

    disk0 = disks[0][0] if disks else "-"
    data_pairs = ["%s:$(location %s)" % (_relpath(d), d) for d in data]

    native.genrule(
        name = name + "_initramfs",
        testonly = 1,
        srcs = [
            "@alpine_busybox//:root/bin/busybox.static",
            "//test/qemu:guest/init",
            ":" + bin_name,
        ] + data,
        outs = [initramfs_out],
        cmd = " ".join([
            "$(location //test/qemu:scripts/mkinitramfs.sh)",
            "--unit",
            "$@",
            "$(location @alpine_busybox//:root/bin/busybox.static)",
            "$(location //test/qemu:guest/init)",
            "$(location :" + bin_name + ")",
            disk0,
            "'" + " ".join(args) + "'",
        ] + data_pairs),
        tools = ["//test/qemu:scripts/mkinitramfs.sh"],
    )

    # A disk-spec with mke2fs options has spaces: Bazel shell-tokenizes `args`.
    disk_args = ["'" + ":".join(d) + "'" if len(d) > 3 else ":".join(d) for d in disks]

    # The test kernel is Alpine's linux-virt (step 24.2): //third_party/linux:
    # vmlinuz. Its drivers are modules; the initramfs gets the archive of the
    # ones this test needs (modules.bzl), which run-qemu.sh appends.
    modules_archive = modules_cpio(test_modules(disks, modules))
    kernel_data = ["//third_party/linux:vmlinuz", modules_archive]
    kernel_args = [
        "--modules",
        "$(location " + modules_archive + ")",
        "$(location //third_party/linux:vmlinuz)",
    ]

    # Step 4.4: the Alpine's QEMU and qboot ROM, passed explicitly --
    # run-qemu.sh does no host lookup of its own.
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

    mem, asan_mem = resolve_mem(mem, asan_mem, UNIT_MEM, UNIT_ASAN_MEM)

    sh_test(
        name = name,
        srcs = ["//test/qemu:scripts/run-qemu.sh"],
        data = kernel_data + qemu_data + [
            ":" + initramfs_out,
        ],
        args = [
            "--unit",
        ] + qemu_args + mem_args_for(mem, asan_mem) + kernel_args + [
            "$(location :" + initramfs_out + ")",
        ] + disk_args,
        # run-qemu.sh --unit: -smp 1 and the allowance above.
        tags = ["no-sandbox", "requires-kvm", "cpu:1", "resources:memory:%d" % (mem + QEMU_OVERHEAD_MB)] + tags,
        size = size,
        timeout = timeout,
    )
