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
- For `nfs_test` only: `mmdebstrap`, `unshare`/`newuidmap`/`newgidmap`
  (util-linux), and an `/etc/subuid`/`/etc/subgid` delegation for your user
  -- see "NFS test and the Debian rootfs" below. Network access (plain
  HTTPS to `deb.debian.org`) is needed once, to build the rootfs image; the
  test itself is fully offline (loopback only).

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
bazel test //test/qemu:create_test
bazel test //test/qemu:rename_test
bazel test //test/qemu:write_test
bazel test //test/qemu:nfs_test
```

**Step 5.2: ext4, xfs and btrfs.** `readonly_test`, `passthrough_test`,
`setattr_test`, `handles_test`, `create_test`, `rename_test`, `write_test`,
`crash_test`, `power_test`, `credentials_test`, `removed_test` and
`pjdfstest_test` are each generated three times -- `<name>_ext4`,
`<name>_xfs`, `<name>_btrfs` -- by `qemu_test_matrix` (`qemu_test.bzl`),
which varies only the first (`vdb`) disk's filesystem across the three
calls to plain `qemu_test` it makes; every command above still works
unchanged (`<name>` is a plain `alias` to `<name>_ext4`), and
`bazel test //...` runs all three variants of each. No guest script is
duplicated: `guest/lib.sh`'s `backing_fstype` detects which filesystem
`vdb` actually is (from the `statfs(2)` magic number -- `stat -f -c %T`
is useless for this, since it prints "ext2/ext3" for ext4's magic
regardless of actual ext2/3/4 version), and the handful of scripts that
have a genuine filesystem-specific branch (`guest/pjdfstest.sh`'s
per-filesystem baseline, `guest/create.sh`'s btrfs-subvolume-boundary
check) use it. See README.md's "tested on" line and docs/conformance.md
for what step 5.2 found: one real dcfs bug (btrfs does not support
`FS_IOC_GETFSUUID` at all; fixed with a `BTRFS_IOC_FS_INFO` fallback) and
otherwise no behavioral differences across the three filesystems --
generation handling, ACL-equivalent-to-mode suppression, and
`security.capability` stripping are all generic VFS behavior, identical on
ext4, xfs and btrfs.

- `boot_test` (`guest/boot.sh`): dcfs and fhtest are present and runnable,
  and the scratch disk mounts.
- `readonly_test` (`guest/readonly.sh`): step 3.2's read-only ops
  (Lookup/Getattr/Readdir(plus)/Readlink/Getxattr/Listxattr/Statfs) served
  from the cache -- inode numbers matching the backing filesystem, and
  (checked via `/sys/block/<dev>/stat`) that a warm cache causes *zero*
  reads from the backing block device, including after killing and
  restarting the daemon against the same cache database. Also step 4.8's
  submount-refusal policy (amendment 12), using vdc as a second
  filesystem: dcfs refuses to start at all with vdc already mounted below
  `--source` (`startup-refuses-submount`), and refuses a boundary that
  appears at runtime instead of caching it (`boundary-*`) -- see the
  README's Limitations.
- `passthrough_test` (`guest/passthrough.sh`): step 3.3's file contents via
  FUSE passthrough -- a small file and a 64 MiB file read through dcfs
  match the backing files, a content read (unlike metadata) does move the
  backing device's block-read counter, dcfs's own CPU time during a 64 MiB
  read stays low enough that the kernel must be reading the backing file
  directly, a non-read-only open succeeds and its write lands on the
  backing filesystem (step 4.4; `write_test` has the full write-through
  test), 200 open/release cycles leak no fds, and all of this still works
  after killing and restarting the daemon against the same cache database.
- `lifecycle_test` (`guest/lifecycle.sh`): step 3.5's daemon lifecycle and
  CLI -- usage/flag validation, a missing or non-directory `--source`, a
  cache database refused because it belongs to a different filesystem,
  `--fuse_opt` (good and bad options), a clean SIGTERM shutdown (exit 0,
  unmounted, WAL checkpointed), mounting dcfs back over its own `--source`,
  and restarting against a previously-used cache database.
- `handles_test` (`guest/handles.sh`): step 3.4b's NFS export handles
  (`FUSE_CAP_EXPORT_SUPPORT`/`FUSE_CAP_ATTR_GENERATION`, exercised with
  `//tools:fhtest`) -- a handle for a file on the source device opens and
  reads back the right content; the reported generation is 0 for the root
  and nonzero (and stable across a restart) for everything else; a handle
  survives a daemon restart against the same cache database; a doctored
  generation is rejected with ESTALE; an inode number recycled behind
  dcfs's back invalidates the old row so its handle comes back ESTALE; and
  wiping the cache database -- the one case that does *not* survive --
  also yields ESTALE, from a freshly reseeded generation counter. Two
  consequences of the exclusive-access model (no write-through
  invalidation until Phase 4) that this test cannot demonstrate are
  reported as `SKIP`, not a faked pass. Also step 4.8's runtime submount
  refusal (amendment 12): no `fhtest handle` can be minted for a name
  behind a boundary vdc is mounted onto at runtime (`handle-boundary-*`).
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
  (busybox's `truncate -s` instead opens O_WRONLY and calls `ftruncate(2)`
  on the resulting fd) and
  `fchmodat(2)` with `AT_SYMLINK_NOFOLLOW` (busybox's `chmod` has no
  `-h`/`--no-dereference`, so it can never target a symlink itself).
- `create_test` (`guest/create.sh`): step 4.2's create-family write-through
  ops -- mkdir (plain, nested, EEXIST, ENOENT on a missing parent), create
  via a shell redirect (content, size immediately after close, and
  O_EXCL/noclobber -> EEXIST without touching the existing file), mknod (a
  FIFO), symlink (resolving and dangling), link (nlink/inode agreement
  between the two names and both sides) -- each checked against both `/src`
  and `/mnt`; a normalized `find`+`stat` listing of the whole tree agreeing
  between the two sides; and (checked via `/sys/block/<dev>/stat`, as in
  `readonly_test`) that a full metadata pass over everything just created
  causes *zero* reads from the backing block device, including after
  killing and restarting the daemon against the same cache database. Also
  step 4.8's runtime submount refusal (amendment 12): mkdir/link against a
  boundary vdc is mounted onto at runtime fail with EXDEV, exactly as they
  would across a real device boundary (`mkdir-boundary-refused`,
  `link-exdev`).
- `rename_test` (`guest/rename.sh`): step 4.3's remove/rename write-through
  ops -- unlink (plain, of one hard link, and of a file still open, whose
  content stays readable and whose row, and so its handle, lives until the
  last close and is then ESTALE), rmdir (empty, ENOTEMPTY, ENOENT), and
  rename (same dir, across dirs, over an existing file whose old row is
  deleted, `RENAME_NOREPLACE` -> EEXIST, `RENAME_EXCHANGE`, a directory with
  children whose whole cached subtree moves with it, a directory over an
  empty directory, and EXDEV renaming into a boundary vdc is mounted onto
  at runtime -- step 4.8's amendment 12 -- via `//tools:testutil rename2`
  since busybox `mv` falls back to copy+delete). Every result is checked on
  `/src` and via `/mnt` after dropping every cache with zero sectors read
  (negative entries and directory completeness are recorded, not re-read),
  then as a whole tree, and again after a daemon restart.
- `write_test` (`guest/write.sh`): step 4.4's write-through file I/O and the
  "one backing file per inode" fix -- plain writes (create, append,
  in-place overwrite, `O_TRUNC` on an existing file via the kernel's own
  `SETATTR(size=0)`, a 64 MiB passthrough write low enough in daemon CPU
  time to prove the kernel moved the bytes, and -- step 4.8's amendment
  12 -- a write into a boundary vdc is mounted onto at runtime failing
  with EXDEV, `write-boundary-refused`); two concurrent opens of one file
  (both readers, and a
  reader held open across a writer's open/write/close) all succeeding,
  which used to EBUSY/EIO before this step because every open got its own
  passthrough backing file; a writer's close making its effect on size
  visible immediately even while another open of the same file remains
  outstanding; fsync; fallocate (plain, `KEEP_SIZE`, `PUNCH_HOLE`, checked
  against `/src`'s block count); setxattr/getxattr/listxattr/removexattr on
  a file, a directory, and (`EPERM` for the `user.` namespace) a symlink,
  with getxattr of an unchanged value served from cache with zero sectors
  read; getxattr through the mount answering exactly what the backing
  filesystem does after a setxattr of an ACL equivalent to the mode (ext4
  stores none), a chmod of a file with an ACL (ext4 rewrites it), and a
  chown, truncate or write of a file with `security.capability` (removed)
  -- step 4.11; a second `mkdir` of an already-existing directory (4.2's
  create-family failure paths now re-resolve the name instead of leaving it
  unknown) followed by listing that directory's own `..` with zero sectors
  read; a normalized `find`+`stat` listing of the whole tree agreeing
  between `/src` and `/mnt`; and all of it -- including the xattr still
  being served from cache -- surviving a daemon restart against the same
  cache database.

- `nfs_test` (`guest/nfs.sh`): step 5.3's real NFS export -- dcfs mounted
  and then re-exported over loopback NFSv4 (`rpc.nfsd`/`rpc.mountd`/
  `exportfs`, from a small Debian chroot; see "NFS test and the Debian
  rootfs" below) checks a normalized listing matching `/src`; a metadata
  pass over NFS causing zero backing-device reads (the NFS client's own
  attribute/dentry cache is defeated by `drop_caches`, same mechanism as
  everywhere else in this repo, so this is a real round trip through nfsd
  and dcfs, not served from the client); a content read matching `/src`
  and moving the backing counter; a file held open over NFS surviving a
  `dcfs` restart with no ESTALE/EIO (`nfsd`'s own export-path cache needs
  an explicit `exportfs -f` after the restart to pick up the new mount --
  ordinary NFS administration, not a dcfs workaround); a write through NFS
  landing on the backing file and reading back correctly (step 4.4); and
  wiping the cache database yielding ESTALE for the pre-wipe handle
  (with zero backing I/O, i.e. caught before ever reaching the backing
  file) followed by a working fresh mount. Also step 4.8's runtime
  submount refusal (amendment 12), using vdc as a second filesystem
  mounted below the source: the boundary is refused locally before it is
  ever exported (`boundary-*`), and `exportfs`'s `crossmnt` option --
  deliberately left on -- reveals nothing for it either
  (`nfs-boundary-*`), since dcfs itself never crosses the boundary in the
  first place.

The serial console log lands at
`bazel-testlogs/test/qemu/boot_test/test.outputs/serial.log` (Bazel's
`TEST_UNDECLARED_OUTPUTS_DIR`), or in the test's `TEST_TMPDIR` if that
variable isn't set (e.g. running `scripts/run-qemu.sh` by hand).

### Adding a new e2e test

Add a `guest/<name>.sh` script (picked up automatically by the
`glob(["guest/*.sh"])` in the `:initramfs` genrule) and wire it up in
`test/qemu/BUILD.bazel`, either as a single-filesystem test:

```
qemu_test(
    name = "<name>_test",
    guest_script = "guest/<name>.sh",
    disks = [("vdb", "ext4", "256M")],
)
```

or, if the test exercises backing-filesystem-dependent behavior worth
checking on ext4, xfs and btrfs (step 5.2), as three:

```
qemu_test_matrix(
    name = "<name>_test",
    guest_script = "guest/<name>.sh",
    disks = [("vdb", "ext4", "256M")],  # the fstype here is only a
                                         # placeholder; qemu_test_matrix
                                         # overrides it per generated
                                         # variant. Use >=320M if any disk
                                         # here might become xfs: mkfs.xfs
                                         # refuses anything under 300MB.
)
```

which generates `<name>_test_ext4`/`_xfs`/`_btrfs` plus a plain
`<name>_test` alias to the ext4 variant -- see `guest/lib.sh`'s
`backing_fstype` for how a script detects which filesystem it is actually
running against, needed only if the test has a genuine filesystem-specific
branch (most don't: see the "Step 5.2" paragraph above).

`guest/init` runs the script named by the `dcfs_test=` kernel command-line
parameter (the macro fills this in from `guest_script`'s basename) via
`sh`, then reports `ALL-TESTS-PASSED` or `TEST-FAILED` based on its exit
status.

## NFS test and the Debian rootfs

`nfs_test` (above) needs `rpc.nfsd`/`rpc.mountd`/`exportfs`/`mount.nfs4`,
none of which fit in the busybox-only initramfs every other test runs
from. It gets them by chrooting into a small Debian tree instead:

### Building the image

```
test/qemu/scripts/mkrootfs-debian.sh
```

Builds `bookworm` (override with `DCFS_ROOTFS_SUITE=trixie` etc.) via
`mmdebstrap --mode=unshare` into `~/.cache/dcfs/rootfs-debian.ext4` (default;
override with `DCFS_ROOTFS_IMAGE`), the same out-of-tree-artifact pattern as
`build-kernel.sh`/`@kernel_image`: not a Bazel target (it downloads ~200 MB
and doesn't belong in the action graph), but exposed to Bazel the same way,
by `test/qemu/kernel.bzl`'s `kernel_image` repo rule symlinking it in as
`@kernel_image//:rootfs_debian.ext4` if present, or a `DCFS-ROOTFS-MISSING`
placeholder otherwise (`run-qemu.sh` fails fast and clearly on the
placeholder, same as for a missing kernel).

Measured on this host: ~35-45s wall time (mmdebstrap ~35-40s of that,
downloading ~200 MB; the tar-to-ext4 conversion is ~1-2s), producing a
1024 MiB nominal image (`DCFS_ROOTFS_SIZE` to change) of which ~265 MiB is
actually used -- most of the headroom is for `/cache/dcfs.db` and whatever
the test writes during a run, since the image is copied (not the same
image reused read-only) into each test's own `$TEST_TMPDIR` so the guest
can write to it freely without disturbing the cached original.

No `//test/qemu:rootfs_debian` Bazel genrule exists, deliberately: the
image has to live outside `bazel-out` for exactly the reason the kernel
does (so it survives `bazel clean` and is shared across worktrees/branches
instead of being rebuilt, and re-downloaded, by every one of them), so a
genrule wrapping the same script would add a layer without adding value.

### The unprivileged mmdebstrap-to-ext4 recipe

This took real trial and error; the working recipe (and why) is documented
inline in `mkrootfs-debian.sh`'s header comment, briefly:

1. **Keyring**: this host's/Ubuntu's `debian-archive-keyring` was too old
   (missing a since-added co-signing key on bookworm's InRelease), producing
   `NO_PUBKEY` errors that look like a mirror problem. Fetch the current
   package directly from `deb.debian.org`'s pool (plain HTTPS, no apt
   circularity) and cache it.
2. **mmdebstrap's own sandbox can't read `$HOME/.cache`**: its internal
   `--mode=unshare` sandbox doesn't map your uid to itself, so anything it
   reads (like `--keyring`) needs to live under a world-traversable
   directory -- `$HOME/.cache` is mode 0700 by convention, so the cached
   keyring gets staged to a `chmod 644` copy under `/tmp` just for the
   mmdebstrap invocation.
3. **Tar-to-ext4 as your own uid loses ownership**: `mkfs.ext4 -d <dir>`
   needs a real directory and preserves whatever ownership it has, but a
   plain `tar -x` as yourself can't restore Debian's real ownership
   (`/etc/shadow`'s group, etc). `unshare --user --map-root-user` only maps
   one id (yours to 0); every other uid/gid in the tarball gets `EINVAL` on
   chown. The fix: `--map-user=0` (your uid to inner 0) *combined with*
   `--map-users=1:<subuid-start>:<subuid-count>` (inner 1..N to your
   `/etc/subuid` delegation) -- these two compose, unlike two `--map-users`
   (whose last occurrence simply wins, per `unshare(1)`).
4. **mknod is unfixable, and doesn't matter**: even with that full mapping,
   creating real device nodes (`/dev/null`, etc.) fails with EPERM --
   `mknod(2)` for a character/block device checks `CAP_MKNOD` against the
   **init** user namespace unconditionally, so no amount of unprivileged
   nesting grants it. `mkrootfs-debian.sh` excludes `./dev/*` from the tar
   extraction and creates an empty `/dev` instead: harmless, since
   `guest/init` bind-mounts the real `/dev` over the chroot before using it
   (see below).
5. **The image needs a couple of things a "complete" Debian install gets
   for free** that a bare `--variant=apt` bootstrap doesn't run the
   postinst scripts for: `/etc/mtab` (normally a symlink to
   `/proc/self/mounts`, set up by the `mount` package's postinst) and a
   clean `/etc/resolv.conf` (mmdebstrap's unshare mode bind-mounts the
   *host's* resolv.conf into its build chroot for apt's own DNS, and that
   host-specific content was ending up baked into the image). Both are
   fixed up directly in the script after extraction, before `mke2fs`.

### Boot mode: chrooting instead of switching root

The initramfs boot stays the fast path for every other test. `qemu_test`
takes an optional `rootfs = "@kernel_image//:rootfs_debian.ext4"` attribute
(`test/qemu/qemu_test.bzl`): when given, `run-qemu.sh` copies the cached
image into the test's own tmpdir, attaches it as an extra virtio-blk disk
at the next free `/dev/vd<letter>`, and passes `dcfs_rootfs=/dev/vd<letter>`
on the kernel command line. `guest/init` mounts it on `/newroot`,
bind-mounts `/proc`, `/sys`, and `/dev` into it, copies `dcfs`/`fhtest`/
`testutil` and `/tests` in, brings up loopback (in the initramfs's own
network namespace -- `chroot(2)` doesn't touch that), and `chroot`s into it
to run the guest script with GNU coreutils/findutils and nfs-utils
available, instead of running it straight from the busybox initramfs.

### Findings from getting `nfs_test` green

None of these are dcfs bugs; all are either genuine environment gaps this
step filled in, or real NFS-server administration this test now does the
way any admin would:

- **Kernel**: `NFSD_LEGACY_CLIENT_TRACKING` needed enabling in
  `build-kernel.sh`. Without it, nfsd upcalls to a userland client-tracking
  daemon on a client's first `SETCLIENTID` (`NFSD: Unable to initialize
  client recovery tracking! (-110)`); on this guest, whichever daemon
  serviced that upcall (`nfsdcld`, or `rpc.mountd`'s own legacy handler)
  crashed reproducibly. With it, nfsd manages `/var/lib/nfs/v4recovery`
  itself, in-kernel, with no upcall at all.
- **`rpc.mountd` segfault, root cause**: straceing it found the real
  culprit was unrelated to client tracking: `openat("/etc/mtab", ...)`
  returning `ENOENT` (see the rootfs section above) right after it
  resolves the export path, followed immediately by a `SIGSEGV`
  (`SEGV_MAPERR`, address `NULL`) -- a NULL check missing on that open's
  failure, in a codepath every export apparently hits. Fixed by shipping
  `/etc/mtab` in the image, not by working around `rpc.mountd`.
- **GNU find's cycle-detection heuristic** (busybox's `find`, used by every
  other `guest/*.sh` script, has no such check) false-positived walking
  this export back when `guest/nfs.sh` still joined a second backing
  filesystem under the source as a real submount: dcfs exposes backing
  inode numbers verbatim (this doc's "Identity model"), and every ext4
  backing filesystem's root directory is inode 2 by convention, so the
  export root and the submount's root collided on `(dev, ino)` once NFS
  presented the whole tree under one device number -- GNU find sees a
  directory sharing `(dev, ino)` with its own ancestor and refuses to
  descend ("File system loop detected"). Moot since step 4.8 (amendment
  12): submounts are refused outright rather than cached, so no such
  collision can arise any more; `guest/nfs.sh`'s `find_stat_tree` still
  prunes one directory (`d`), but only to exclude that step's own
  boundary-refusal test fixture from the general listing comparison, not
  to work around this.
- **`nfsd`'s export-path cache** needs an explicit `exportfs -f` after
  restarting dcfs, or every request comes back EIO: it can hold a reference
  tied to the *old* vfsmount/dentry, independent of anything dcfs does.
  Restarting `nfsd` itself (`rpc.nfsd 0` then re-enabling it) additionally
  drops its own server-side open-file cache, which otherwise can keep
  serving an already-open file without ever re-resolving its handle
  against a cold daemon.
- **The NFS client's own attribute/dentry cache** can just as easily hide
  what a check is trying to prove -- both the "warm metadata" pass and the
  db-wipe-ESTALE check need an explicit `drop_caches` (same mechanism used
  everywhere else in this repo) immediately before the check, or they can
  pass for the wrong reason (served from the client's cache, never reaching
  the server at all).

## `run-qemu.sh` internals

One script serves both kinds of test (see the usage comment at the top of
`scripts/run-qemu.sh`): `--unit <bzImage> <initramfs> [disk-spec...]` for
`qemu_cc_test`, or the legacy `<bzImage> <initramfs> <dcfs_test-name>
[disk-spec...]` (no leading flag) for `qemu_test`. Each `disk-spec` is
`<device>:<fstype>:<size>`, e.g. `vdb:ext4:256M`; disks are attached in
`<letter>` order, with a small unformatted filler drive for any skipped
letter, so the guest kernel enumerates the requested disk at exactly
`/dev/vd<letter>`.
