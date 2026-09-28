"""Repository rule exposing the out-of-tree dcfs QEMU test kernel, the
Debian NFS-test rootfs image, and the host busybox binary to Bazel.

None of these is built by Bazel: the kernel is built by
test/qemu/scripts/build-kernel.sh (a 30-90 minute out-of-tree kernel build
that doesn't belong in the Bazel action graph), the rootfs image is built
by test/qemu/scripts/mkrootfs-debian.sh (a ~30-60s mmdebstrap run that
downloads ~200 MB over the network -- also not something that belongs in
the Bazel action graph, and it needs to be cached across `bazel clean`s
the way the kernel already is), and busybox is a host tool. This rule just
symlinks them into a repo so other targets can depend on them normally.

If the kernel hasn't been built yet, `bzImage` is a placeholder text file
(first line "DCFS-KERNEL-MISSING") instead of a hard failure, so that
`bazel build //...` / `bazel test //...` keep analyzing and building fine
with no kernel present; test/qemu/scripts/run-qemu.sh detects the
placeholder and fails with a clear message when the qemu test actually
runs. `rootfs_debian.ext4` works the same way (placeholder first line
"DCFS-ROOTFS-MISSING") for the same reason.
"""

_DEFAULT_BUSYBOX = "/usr/bin/busybox"
_FALLBACK_BUSYBOX = "/bin/busybox-static"

_BUILD_FILE_CONTENT = """\
exports_files(["bzImage", "busybox", "rootfs_debian.ext4"])
"""

def _kernel_build_dir(repository_ctx):
    build_dir = repository_ctx.getenv("DCFS_KERNEL_BUILD")
    if build_dir:
        return build_dir
    home = repository_ctx.getenv("HOME", "/root")
    return home + "/.cache/dcfs/kernel-build"

def _rootfs_image_path(repository_ctx):
    image = repository_ctx.getenv("DCFS_ROOTFS_IMAGE")
    if image:
        return image
    home = repository_ctx.getenv("HOME", "/root")
    return home + "/.cache/dcfs/rootfs-debian.ext4"

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

    rootfs_path = _rootfs_image_path(repository_ctx)
    if repository_ctx.path(rootfs_path).exists:
        repository_ctx.symlink(rootfs_path, "rootfs_debian.ext4")
    else:
        repository_ctx.file(
            "rootfs_debian.ext4",
            content = "DCFS-ROOTFS-MISSING\n" +
                      "Build it with: test/qemu/scripts/mkrootfs-debian.sh\n" +
                      "(writes to $DCFS_ROOTFS_IMAGE, default " +
                      rootfs_path + ")\n",
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
    # The whole point of this repo is to reflect a big out-of-tree kernel
    # build (test/qemu/scripts/build-kernel.sh) that Bazel doesn't build
    # and can't see changing. repository_ctx.getenv tracks $DCFS_KERNEL_BUILD
    # and $DCFS_BUSYBOX themselves as dependencies, but not the mtime/
    # existence of the files those env vars point to, so without `local =
    # True` a bzImage that appears (kernel build finishes) or moves after
    # the first fetch would go unnoticed until something else invalidated
    # the repo. `local = True` makes Bazel re-run this (cheap: a couple of
    # stats and a symlink) on every build instead, which is what lets
    # `bazel build //test/qemu:boot_test` pick up a freshly built kernel
    # without `bazel clean` or `bazel sync`.
    local = True,
    doc = """Exposes the out-of-tree dcfs QEMU kernel build, the Debian
NFS-test rootfs image, and host busybox as `bzImage`, `rootfs_debian.ext4`,
and `busybox`.""",
)
