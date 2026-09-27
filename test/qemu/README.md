# dcfs QEMU end-to-end tests

Boots dcfs (statically linked, `//dcfs:main_static`) and `//tools:fhtest`
inside a minimal busybox initramfs under QEMU, and checks the serial
console output for `ALL-TESTS-PASSED`.

## Prerequisites

- `qemu-system-x86_64`
- `mkfs.ext4`, `mkfs.btrfs`, `mkfs.xfs`, `truncate`
- A statically linked `busybox` (checked at `/usr/bin/busybox`, falling
  back to `/bin/busybox-static`; override with `DCFS_BUSYBOX`)
- `/dev/kvm` is used opportunistically (`-accel kvm:tcg`) but not
  required; being in the `kvm` group is optional. Without it, QEMU falls
  back to TCG, which is much slower — see the `timeout` below.

## Building the test kernel

The kernel is *not* built by Bazel (it's a 30-90 minute out-of-tree kernel
build that doesn't belong in the Bazel action graph). Build it once with:

```
test/qemu/scripts/build-kernel.sh
```

This builds `${LINUX:-$HOME/Sources/linux}` out of tree into
`${DCFS_KERNEL_BUILD:-$HOME/.cache/dcfs/kernel-build}` and logs to
`$DCFS_KERNEL_BUILD/../kernel-build.log`. `$LINUX` must not have an
in-tree `.config` (run `make mrproper` there first if it does).

Until the kernel is built, `@kernel_image//:bzImage` is a placeholder file
(first line `DCFS-KERNEL-MISSING`); `bazel build`/`bazel test //...` still
work fine (the qemu tests are excluded by the default
`test_tag_filters=-qemu`), but running a qemu_test target fails fast with
a clear message pointing back at this script.

If you rebuild the kernel into a different directory, export
`DCFS_KERNEL_BUILD` before invoking Bazel (or pass
`--repo_env=DCFS_KERNEL_BUILD=...`) so `@kernel_image` picks it up.

## Tests

- `boot_test` (`guest/boot.sh`): dcfs and fhtest are present and runnable,
  and the scratch disk mounts.
- `readonly_test` (`guest/readonly.sh`): step 3.2's read-only ops
  (Lookup/Getattr/Readdir(plus)/Readlink/Getxattr/Listxattr/Statfs) served
  from the cache -- across a real submount, inode numbers matching the
  backing filesystems, and (checked via `/sys/block/<dev>/stat`) that a
  warm cache causes *zero* reads from either backing block device,
  including after killing and restarting the daemon against the same
  cache database.

## Running

```
bazel test --config=qemu //test/qemu:boot_test
bazel test --config=qemu //test/qemu:readonly_test
```

(`--config=qemu` sets `--test_tag_filters=qemu`, overriding the default
`.bazelrc` filter that excludes these tests from a plain `bazel test
//...`.)

The serial console log lands at
`bazel-testlogs/test/qemu/boot_test/test.outputs/serial.log` (Bazel's
`TEST_UNDECLARED_OUTPUTS_DIR`), or in the test's `TEST_TMPDIR` if that
variable isn't set (e.g. running `scripts/run-qemu.sh` by hand).

## Adding a new test

Add a `guest/<name>.sh` script (it's picked up automatically by the
`glob(["guest/*.sh"])` in the `:initramfs` genrule) and wire it up in
`BUILD.bazel`:

```
qemu_test(
    name = "<name>_test",
    guest_script = "guest/<name>.sh",
    disks = [("vdb", "ext4", "256M")],
)
```

`guest/init` runs the script named by the `dcfs_test=` kernel command-line
parameter (the macro fills this in from `guest_script`'s basename) via
`sh -e`, then reports `ALL-TESTS-PASSED` or `TEST-FAILED` based on its
exit status.
