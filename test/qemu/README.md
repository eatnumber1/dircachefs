# dcfs QEMU tests

PROJECT DECISION: dcfs requires root (real `open_by_handle_at`,
`FS_IOC_GETFSUUID`, and so on). There is no host-side test execution:
**every** test -- plain unit tests included -- boots the project's own
minimal Linux kernel under QEMU, as root, with KVM, and runs there. A
`cc_test` in this repo does not exist; `qemu_cc_test` (below) is what
every `dcfs/BUILD.bazel` test target uses instead.

This is not slow. A unit-test VM boots in well under a second (see
"Boot time" below): `bazel test //...` is dominated by compilation, not
booting.

## Prerequisites

- `qemu-system-x86_64` (8.2+; needs the `microvm` machine type and
  `virtio-blk-device`) and `qboot.rom` (Debian/Ubuntu: package `qemu-utils`
  or `qemu-system-data`; it's normally already installed alongside
  `qemu-system-x86`, at `/usr/share/qemu/qboot.rom`).
- `mkfs.ext4`, `mkfs.btrfs`, `mkfs.xfs`, `truncate` (for tests with a
  `disks =` attribute).
- A statically linked `busybox` (checked at `/usr/bin/busybox`, falling
  back to `/bin/busybox-static`; override with `DCFS_BUSYBOX`).
- `/dev/kvm`, writable by you. Put yourself in the `kvm` group
  (`sudo usermod -aG kvm "$USER"`, then re-login), or on a shell that
  predates the group change taking effect, wrap the `bazel test` /
  `run-qemu.sh` invocation in `sg kvm -c '...'`. Without a writable
  `/dev/kvm`, QEMU falls back to TCG (software emulation), which still
  works but is ten-plus times slower -- see `TIMEOUT` below.

## Building the test kernel

The kernel is *not* built by Bazel (it's an out-of-tree kernel build that
doesn't belong in the Bazel action graph). Build it once with:

```
test/qemu/scripts/build-kernel.sh
```

This builds `${LINUX:-$HOME/Sources/linux}` out of tree into
`${DCFS_KERNEL_BUILD:-$HOME/.cache/dcfs/kernel-build}` and logs to
`$DCFS_KERNEL_BUILD/../kernel-build.log`, which ends with the enabled
config-symbol count before/after and the resulting `bzImage` size. `$LINUX`
must not have an in-tree `.config` (run `make mrproper` there first if it
does). The script is idempotent: it always rebuilds `.config` from a fresh
`x86_64_defconfig` + `kvm_guest.config` baseline rather than patching the
previous one, so re-running it after editing the script's enable/disable
list is safe. It builds in the foreground (`nice -n 19 make -jN bzImage`);
run it with `run_in_background` yourself if you don't want to wait.

Until the kernel is built, `@kernel_image//:bzImage` is a placeholder file
(first line `DCFS-KERNEL-MISSING`); `bazel build //...` still works fine,
but running any test fails fast with a clear message pointing back at this
script.

If you rebuild the kernel into a different directory, export
`DCFS_KERNEL_BUILD` before invoking Bazel (or pass
`--repo_env=DCFS_KERNEL_BUILD=...`) so `@kernel_image` picks it up.

### Why this kernel is minimal

Every unit test now pays this kernel's boot cost, so it is trimmed hard:
`x86_64_defconfig` + `kvm_guest.config`, minus every bulky subsystem the
guest never touches (DRM/FB/VGA console, sound, USB, input/HID, I2C,
thermal, watchdog, Bluetooth/NFC/Wi-Fi, media, PCMCIA, ATA/SCSI, every wired
Ethernet driver, netfilter, IPv6, HPET, ACPI, loadable modules, debug info,
kexec, hibernation, cpufreq/cpuidle, Xen/Hyper-V/VMware guest drivers),
plus everything it needs (PVH direct boot, virtio-mmio + virtio-blk +
virtio-net, ext4/btrfs/xfs, FUSE + `FUSE_IO_URING`, NFSv4 client+server,
`FHANDLE`/`EXPORTFS`, devtmpfs, tmpfs with POSIX ACLs). See
`test/qemu/scripts/build-kernel.sh` for the exact list.

Two consequences worth knowing:

- **No ACPI.** The guest cannot power off via ACPI. `guest/init` instead
  runs `reboot -f` with `reboot=t` on the kernel command line: the kernel
  forces a reboot via triple fault, and QEMU's `-no-reboot` makes that
  *quit* the VM instead of actually rebooting it. If you ever see a QEMU
  guest that boots but never returns, check that `-no-reboot` and
  `reboot=t` are both still there.
- **No PCI.** Disks are virtio-mmio (`-device virtio-blk-device`), not
  virtio-pci; the `microvm` machine type has no PCI bus at all.

## Fast boot

The runner (`scripts/run-qemu.sh`) boots QEMU's `microvm` machine type with
direct kernel boot and no legacy PC devices this guest doesn't need:

```
-M microvm,x-option-roms=off,pit=off,pic=off,rtc=on,isa-serial=on,acpi=off
-bios /usr/share/qemu/qboot.rom
-nodefaults -no-user-config -nographic -serial stdio
-accel kvm -cpu host        # falls back to -accel tcg -cpu max, with a
                             # warning line in the log, if /dev/kvm isn't
                             # writable
-m 256 -smp 1                # unit tests; e2e tests use -m 1024 -smp 2
```

**`rtc=on`, deliberately not `rtc=off`.** This is the one deviation from
the "disable every legacy device" instinct that actually *costs* boot
time instead of saving it: without a CMOS RTC device present, the
kernel's boot-time wall-clock read (`mc146818_get_cmos_time`, called from
`arch/x86/kernel/rtc.c` regardless of `CONFIG_RTC_CLASS`/`CONFIG_RTC_DRV_CMOS`
-- it's unconditional x86 arch code, not a driver) finds nothing at ports
0x70/0x71 and burns several seconds in retry loops -- twice per boot,
measured at roughly 2.4s and 2.6s on this host, taking a kernel that
otherwise boots in under a second up to 6+ seconds. `rtc=on` costs
nothing (nothing in the guest polls it) and skips both stalls.

**`acpi=off`, deliberately** (also verified experimentally): microvm tells
the guest about its virtio-mmio disks one of two ways -- an ACPI DSDT
device (ACPI on) or a `virtio_mmio.device=...` parameter QEMU appends to
the kernel command line automatically (ACPI off; this is what
`CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES` parses). The test kernel has no ACPI
at all, so with ACPI on/auto (QEMU's default) the guest never learns
where its disks are -- `info qtree` over the QEMU monitor confirms the
`virtio-blk-device` is correctly attached to a virtio-mmio transport
either way, but `/dev/vd*` only appears with `acpi=off`.

**Firmware: qboot, not `bios-microvm.bin`.** `microvm`'s other stock
firmware option, `bios-microvm.bin` (a cut-down SeaBIOS build, also present
on this host at `/usr/share/seabios/bios-microvm.bin`), turns out **not**
to work with `x-option-roms=off`: its `-kernel`/`-initrd` hand-off is
implemented as an option ROM, so with option ROMs disabled it falls
through to normal BIOS boot-device probing and fails with "No bootable
device" (verified experimentally while building this). qboot has no such
dependency, and has a second advantage: it detects the kernel's PVH entry
point (`CONFIG_PVH=y`, set by `build-kernel.sh`) and uses it directly when
present -- an even faster, effectively firmware-less boot -- falling back
to the normal Linux/x86 real-mode boot protocol for a kernel that lacks
it. So there's no PVH-vs-not branch in `run-qemu.sh`: one firmware choice
(qboot) covers both, automatically. Override the qboot path with
`DCFS_QBOOT` if it's not at `/usr/share/qemu/qboot.rom` on your system.

Disks are virtio-mmio, attached as a `-drive ... -device
virtio-blk-device,drive=...` pair per disk (see "Giving a test a real
disk" below); the `/dev/vd<letter>` naming/filler-disk logic is unchanged
from before this machine-type switch.

### Boot time

`run-qemu.sh` prints its own `date +%s.%N` around the `qemu-system-x86_64`
invocation to the serial log (`grep 'run-qemu.sh: qemu' <serial.log>`);
the kernel's own `Run /init as init process` timestamp
(pr_info, so it only shows at `loglevel=7+`, not the `loglevel=3` these
tests actually run at) is the boot-to-userspace figure. The serial log for
any test target is at
`bazel-testlogs/<package>/<test>/test.outputs/serial.log`.

Measured on this host (nominally 4 cores / 11 GB, KVM, but shared with
several other agents' concurrent `bazel test`/kernel-build jobs while this
step was built -- load average around 100+, ~7.5 GB swapped -- so these
numbers are worse than a quiet machine would show):

- kernel timestamp of `Run /init as init process` at `loglevel=3` (no
  console spam skewing it; captured by having a diagnostic init dump
  `dmesg` after boot instead): **~0.97-1.25s** across three runs.
- end-to-end wall time of a real `bazel test //dcfs:fd_test` unit-test VM
  (`date +%s.%N` around the whole `qemu-system-x86_64` invocation,
  including QEMU/KVM process startup and teardown, which the kernel
  timestamp above does not cover): **~2.1s**.
- for comparison, the same boot with `rtc=off` (this step's first attempt,
  before the finding documented above) or the pre-5.1b kernel/machine
  type: **6+ seconds** -- most of it two CMOS RTC probe timeouts that
  `rtc=on` eliminates entirely.

The end-to-end figure is short of the "well under one second" target
because of host contention during measurement, not the boot path itself:
the kernel-internal figure (which is what direct PVH boot + a minimal
kernel actually control) is already at or under one second, and the gap
between it and the end-to-end figure is QEMU/KVM process-level overhead
(fork/exec, VM creation ioctls, initramfs decompression before the first
kernel timestamp) that scales with host CPU scheduling latency, which was
poor while multiple sibling agents' builds were running. Re-measure on a
quiet host if you need a clean number.

## `qemu_cc_test`: dcfs's replacement for `cc_test`

```
load("//test/qemu:qemu_cc_test.bzl", "qemu_cc_test")

qemu_cc_test(
    name = "foo_test",
    srcs = ["foo_test.cc"],
    deps = [":foo", "@googletest//:gtest_main"],
)
```

Every `dcfs/BUILD.bazel` test target is one of these. It builds:

- `foo_test_bin`: the test binary, normally fully statically linked
  (`fully_static_link`) so it needs no dynamic loader in the initramfs.
  Under `--config=asan`/`--config=ubsan` it's linked dynamically instead
  (glibc can't be fully statically linked with a sanitizer runtime; see
  "Sanitizers" below), and the resulting initramfs carries its `ldd(1)`
  closure.
- `foo_test.cpio.gz`: a per-test initramfs (`scripts/mkinitramfs.sh
  --unit`) with busybox, `guest/init`, the test binary at `/test/run`, any
  `data =` files under `/test/data/` (`TEST_SRCDIR`), and a `/test/args`
  file holding `args =` (space-joined; `guest/init` word-splits it running
  `/test/run $(cat /test/args)`).
- `foo_test`: an `sh_test` running `scripts/run-qemu.sh --unit`, tagged
  `no-sandbox` and `requires-kvm` (**not** `exclusive`: unit-test VMs are
  cheap enough to run in parallel with each other). It passes iff the
  serial log contains `DCFS-TEST-EXIT=0`; the log is copied to
  `$TEST_UNDECLARED_OUTPUTS_DIR/serial.log`.

`TEST_TMPDIR` is set in the guest (`/tmp/test_tmpdir`, on tmpfs by
default), so a test that only touches `$TEST_TMPDIR` and doesn't need real
filesystem semantics (xattrs, `FS_IOC_GETVERSION`, `FS_IOC_GETFSUUID`,
`name_to_handle_at` export support -- tmpfs has none of these) needs no
changes at all to run under `qemu_cc_test`.

### Giving a test a real disk

A test that needs `TEST_TMPDIR` to be a real filesystem (most of the ones
that talk to `dcfs/backing.{h,cc}` or raw syscalls do) takes an optional
`disks` attribute, same shape as `qemu_test`'s:

```
qemu_cc_test(
    name = "backing_test",
    srcs = ["backing_test.cc"],
    deps = [...],
    disks = [("vdb", "ext4", "32M")],
)
```

`guest/init` mounts the first entry at `/tmp/test_tmpdir` before running
the test. Currently used by `backing_test`, `syscalls_test`,
`file_handle_test`, and `device_id_test` (see the comment next to each in
`dcfs/BUILD.bazel` for what specifically needs it).

### Sanitizers

```
bazel test --config=asan //dcfs:sqlite_test //dcfs:metadata_cache_test //dcfs:backing_test
```

works the same way: `test/qemu:asan_build`/`test/qemu:ubsan_build`
(`config_setting`s matched on `--define=dcfs_sanitizer=asan`/`ubsan`, set
by `.bazelrc`'s `build:asan`/`build:ubsan`) tell `qemu_cc_test` to skip the
`fully_static_link` feature, and `mkinitramfs.sh` to copy the dynamically
linked test binary's shared-library closure (and ELF interpreter) into the
initramfs instead. ASan needs `ptrace` to symbolize leak reports, which the
guest doesn't have configured; `guest/init` sets
`ASAN_OPTIONS=detect_leaks=0` (only if unset) so that limitation doesn't
fail every ASan build.

## e2e tests (`qemu_test`)

`boot_test` and `readonly_test` (`test/qemu/BUILD.bazel`) are the bigger,
slower, whole-daemon tests: they boot `//dcfs:main_static` and
`//tools:fhtest` in a shared initramfs and drive them through a
`guest/<name>.sh` script picked by the `dcfs_test=` kernel command-line
parameter. They're tagged `e2e` (not `qemu` -- that distinction is gone now
that everything is QEMU) plus `exclusive`, `no-sandbox`, and
`requires-kvm`, and run as part of a plain `bazel test //...` like
everything else; there's no separate `--config` to opt into them.

```
bazel test //test/qemu:boot_test
bazel test //test/qemu:readonly_test
bazel test //test/qemu:passthrough_test
bazel test //test/qemu:lifecycle_test
bazel test //test/qemu:handles_test
bazel test //test/qemu:setattr_test
```

- `boot_test` (`guest/boot.sh`): dcfs and fhtest are present and runnable,
  and the scratch disk mounts.
- `readonly_test` (`guest/readonly.sh`): step 3.2's read-only ops
  (Lookup/Getattr/Readdir(plus)/Readlink/Getxattr/Listxattr/Statfs) served
  from the cache -- across a real submount, inode numbers matching the
  backing filesystems, and (checked via `/sys/block/<dev>/stat`) that a
  warm cache causes *zero* reads from either backing block device,
  including after killing and restarting the daemon against the same
  cache database.
- `passthrough_test` (`guest/passthrough.sh`): step 3.3's file contents via
  FUSE passthrough -- a small file and a 64 MiB file read through dcfs
  match the backing files, a content read (unlike metadata) does move the
  backing device's block-read counter, dcfs's own CPU time during a 64 MiB
  read stays low enough that the kernel must be reading the backing file
  directly, any non-read-only open is refused with EROFS, 200 open/release
  cycles leak no fds, and all of this still works after killing and
  restarting the daemon against the same cache database.
- `lifecycle_test` (`guest/lifecycle.sh`): step 3.5's daemon lifecycle and
  CLI -- usage/flag validation, a missing or non-directory `--source`, a
  cache database refused because it belongs to a different filesystem,
  `--fuse_opt` (good and bad options), a clean SIGTERM shutdown (exit 0,
  unmounted, WAL checkpointed), mounting dcfs back over its own `--source`,
  and restarting against a previously-used cache database.
- `handles_test` (`guest/handles.sh`): step 3.4b's NFS export handles
  (`FUSE_CAP_EXPORT_SUPPORT`/`FUSE_CAP_ATTR_GENERATION`, exercised with
  `//tools:fhtest`) -- a handle for a file on the source device and one for
  a file on the submount both open and read back the right content; the
  reported generation is 0 for the root and nonzero (and stable across a
  restart) for everything else; a handle survives a daemon restart against
  the same cache database; a doctored generation is rejected with ESTALE;
  an inode number recycled behind dcfs's back invalidates the old row so
  its handle comes back ESTALE; and wiping the cache database -- the one
  case that does *not* survive -- also yields ESTALE, from a freshly
  reseeded generation counter. Two consequences of the exclusive-access
  model (no write-through invalidation until Phase 4) that this test
  cannot demonstrate are reported as `SKIP`, not a faked pass.
- `setattr_test` (`guest/setattr.sh`): step 4.1's `Setattr` write-through --
  chmod on a regular file, a directory, and a fifo (the three dispatch
  paths in `backing::SetAttr`), plus `EOPNOTSUPP` chmod-ing a symlink
  itself; chown; truncate (shrink, grow, and EISDIR on a directory); and
  utimes (an explicit timestamp, "now", and a nanosecond-precision
  timestamp). Every change is checked against `/src` (it actually landed
  on the backing filesystem) and again via `/mnt` after dropping every
  cache with vdb's sectors-read counter unchanged (served from dcfs's own
  cache, not a fresh read), including a final whole-tree pass and a repeat
  of the whole thing after killing and restarting the daemon against the
  same cache database. Uses `//tools:testutil` (a tiny static helper, also
  baked into the initramfs) for the handful of things busybox's applets
  cannot do precisely enough: `truncate(2)` with no intervening `open()`
  (busybox's `truncate -s` opens O_WRONLY first, which `Open()` refuses
  with EROFS until step 4.4, so it would never reach `Setattr` at all) and
  `fchmodat(2)` with `AT_SYMLINK_NOFOLLOW` (busybox's `chmod` has no
  `-h`/`--no-dereference`, so it can never target a symlink itself).

The serial console log lands at
`bazel-testlogs/test/qemu/boot_test/test.outputs/serial.log` (Bazel's
`TEST_UNDECLARED_OUTPUTS_DIR`), or in the test's `TEST_TMPDIR` if that
variable isn't set (e.g. running `scripts/run-qemu.sh` by hand).

### Adding a new e2e test

Add a `guest/<name>.sh` script (picked up automatically by the
`glob(["guest/*.sh"])` in the `:initramfs` genrule) and wire it up in
`test/qemu/BUILD.bazel`:

```
qemu_test(
    name = "<name>_test",
    guest_script = "guest/<name>.sh",
    disks = [("vdb", "ext4", "256M")],
)
```

`guest/init` runs the script named by the `dcfs_test=` kernel command-line
parameter (the macro fills this in from `guest_script`'s basename) via
`sh`, then reports `ALL-TESTS-PASSED` or `TEST-FAILED` based on its exit
status.

## `run-qemu.sh` internals

One script serves both kinds of test (see the usage comment at the top of
`scripts/run-qemu.sh`): `--unit <bzImage> <initramfs> [disk-spec...]` for
`qemu_cc_test`, or the legacy `<bzImage> <initramfs> <dcfs_test-name>
[disk-spec...]` (no leading flag) for `qemu_test`. Each `disk-spec` is
`<device>:<fstype>:<size>`, e.g. `vdb:ext4:256M`; disks are attached in
`<letter>` order, with a small unformatted filler drive for any skipped
letter, so the guest kernel enumerates the requested disk at exactly
`/dev/vd<letter>`.
