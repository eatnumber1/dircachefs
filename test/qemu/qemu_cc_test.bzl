"""qemu_cc_test(name, srcs, deps, ...): a dcfs unit test that runs inside a
QEMU guest, root, on the project's own kernel.

PROJECT DECISION: dcfs requires root (real open_by_handle_at,
FS_IOC_GETFSUUID, etc.), so there is no host-side test execution -- every
test, including plain unit tests, boots a purpose-built minimal kernel
under QEMU (see test/qemu/scripts/build-kernel.sh and
test/qemu/README.md for the boot-time budget this depends on) and runs as
root inside it. This is the replacement for a plain cc_test.

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
        size = "small",
        timeout = "short",
        tags = [],
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
        size: sh_test size.
        timeout: sh_test timeout.
        tags: extra tags, in addition to the ones this macro always sets.
        **kwargs: forwarded to the underlying cc_binary (e.g. extra
            copts).
    """
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
            "@kernel_image//:busybox",
            "//test/qemu:guest/init",
            ":" + bin_name,
        ] + data,
        outs = [initramfs_out],
        cmd = " ".join([
            "$(location //test/qemu:scripts/mkinitramfs.sh)",
            "--unit",
            "$@",
            "$(location @kernel_image//:busybox)",
            "$(location //test/qemu:guest/init)",
            "$(location :" + bin_name + ")",
            disk0,
            "'" + " ".join(args) + "'",
        ] + data_pairs),
        tools = ["//test/qemu:scripts/mkinitramfs.sh"],
    )

    disk_args = [d[0] + ":" + d[1] + ":" + d[2] for d in disks]

    sh_test(
        name = name,
        srcs = ["//test/qemu:scripts/run-qemu.sh"],
        data = [
            ":" + initramfs_out,
            "@kernel_image//:bzImage",
        ],
        args = [
            "--unit",
            "$(location @kernel_image//:bzImage)",
            "$(location :" + initramfs_out + ")",
        ] + disk_args,
        tags = ["no-sandbox", "requires-kvm"] + tags,
        size = size,
        timeout = timeout,
    )
