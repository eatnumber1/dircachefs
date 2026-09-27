"""Repository rule exposing the out-of-tree dcfs QEMU test kernel and the
host busybox binary to Bazel.

Neither of these is built by Bazel: the kernel is built by
test/qemu/scripts/build-kernel.sh (a 30-90 minute out-of-tree kernel build
that doesn't belong in the Bazel action graph), and busybox is a host tool.
This rule just symlinks them into a repo so other targets can depend on
them normally.

If the kernel hasn't been built yet, `bzImage` is a placeholder text file
(first line "DCFS-KERNEL-MISSING") instead of a hard failure, so that
`bazel build //...` / `bazel test //...` keep analyzing and building fine
with no kernel present; test/qemu/scripts/run-qemu.sh detects the
placeholder and fails with a clear message when the qemu test actually
runs.
"""

_DEFAULT_BUSYBOX = "/usr/bin/busybox"
_FALLBACK_BUSYBOX = "/bin/busybox-static"

_BUILD_FILE_CONTENT = """\
exports_files(["bzImage", "busybox"])
"""

def _kernel_build_dir(repository_ctx):
    build_dir = repository_ctx.getenv("DCFS_KERNEL_BUILD")
    if build_dir:
        return build_dir
    home = repository_ctx.getenv("HOME", "/root")
    return home + "/.cache/dcfs/kernel-build"

def _kernel_image_impl(repository_ctx):
    build_dir = _kernel_build_dir(repository_ctx)
    bzimage_path = build_dir + "/arch/x86/boot/bzImage"

    if repository_ctx.path(bzimage_path).exists:
        repository_ctx.symlink(bzimage_path, "bzImage")
    else:
        repository_ctx.file(
            "bzImage",
            content = "DCFS-KERNEL-MISSING\n" +
                      "Build it with: test/qemu/scripts/build-kernel.sh\n" +
                      "(reads $LINUX, default $HOME/Sources/linux; " +
                      "writes to $DCFS_KERNEL_BUILD, default " +
                      build_dir + ")\n",
        )

    busybox_path = repository_ctx.getenv("DCFS_BUSYBOX", _DEFAULT_BUSYBOX)
    if not repository_ctx.path(busybox_path).exists:
        busybox_path = _FALLBACK_BUSYBOX

    if repository_ctx.path(busybox_path).exists:
        repository_ctx.symlink(busybox_path, "busybox")
    else:
        repository_ctx.file(
            "busybox",
            content = "DCFS-BUSYBOX-MISSING\n" +
                      "No busybox found at " + _DEFAULT_BUSYBOX + " or " +
                      _FALLBACK_BUSYBOX + ".\n" +
                      "Install busybox-static, or set DCFS_BUSYBOX to a " +
                      "statically linked busybox binary.\n",
        )

    repository_ctx.file("BUILD.bazel", content = _BUILD_FILE_CONTENT)

kernel_image = repository_rule(
    implementation = _kernel_image_impl,
    doc = """Exposes the out-of-tree dcfs QEMU kernel build and host
busybox as `bzImage` and `busybox`. Reruns whenever $DCFS_KERNEL_BUILD or
$DCFS_BUSYBOX change (repository_ctx.getenv registers that dependency
automatically).""",
)
