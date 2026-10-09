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

QEMU, qboot, busybox, the kernel and the mkfs tools are Alpine's packages,
fetched and signature-checked by Bazel (Phase 24, `third_party/alpine/`):
none is a host tool, and none is built here. The Alpine binaries are linked
against musl, so they run on the glibc host through musl's loader by small
wrappers the repository rule writes (`@alpine_qemu//:qemu_system_x86_64`,
`@alpine_fstools//:mke2fs`, ...). What's left:

- `truncate` (for tests with a `disks =` attribute) creates the small
  scratch-disk images `run-qemu.sh` makes on the host before boot. They are
  formatted by Alpine's `@alpine_fstools//:mke2fs` (with the checked-in
  `third_party/e2fsprogs/mke2fs.conf`, which gives even small images 4 KiB
  blocks), `:mkfs_xfs` and `:mkfs_btrfs` (R3), so the filesystems under test
  come from the same branch everywhere; no host `mkfs.*` or
  `/etc/mke2fs.conf`.
- `/dev/kvm`, writable by you (optional: without it tests run
  under TCG). Put yourself in the `kvm` group
  (`sudo usermod -aG kvm "$USER"`, then re-login), or on a shell that
  predates the group change taking effect, wrap the `bazel test` /
  `run-qemu.sh` invocation in `sg kvm -c '...'`. Without a writable
  `/dev/kvm` (or with `DCFS_FORCE_TCG=1`), QEMU falls back to TCG
  (software emulation), which works but is slower (Phase 5.1 measured
  2-9x, with an 8.6 s boot) -- see `TIMEOUT` below.
- For `nfs_test` only: network access (to `snapshot.debian.org`) is
  needed once per pin, to fetch the pinned packages that assemble
  `//third_party/debian:rootfs` -- see "NFS test and the Debian rootfs"
  below and `third_party/debian/README.md`. The test itself is fully offline
  (loopback only).
- For `mount_dcfs_systemd_test` only: network access (to
  `cloud.debian.org`) is needed once per pin, to fetch the 388 MiB Debian cloud
  image -- see "The systemd guest" below and `third_party/debian_cloud/README.md`.
  The test itself is fully offline.

## The test kernel

`//third_party/linux:vmlinuz` -- Alpine's `linux-virt` of the Alpine branch
pinned in `MODULE.bazel` (Phase 24; `third_party/alpine/README.md`), fetched
and signature-checked by Bazel. The branch carries one kernel series for its
life (v3.24: 6.18), so a new Alpine build inside the branch is taken as it
comes and nothing here names an exact release (`guest/boot.sh` accepts
`6.18.*-virt`). See `third_party/linux/README.md` for the module mechanism
and the option list (`required_options.txt`, checked against the package's
config by `//third_party/linux:kernel_config_test`). It replaces the kernel
Bazel used to build from source (690 s cold, with host flex, bison, libelf
and bc).

It is the kernel every test boots (`qemu_test`, `qemu_test_matrix` and
`qemu_cc_test` alike; there is no other kernel to select).

### Kernel modules

Most drivers are modules in Alpine's kernel, and loading them is the bulk of
the boot time, so each test loads only what it needs. The macros take
`modules = [...]`; the default is `fuse` and, when the test has `disks` (or a
`rootfs`), `virtio_blk` and the filesystem modules of those disks (a matrix
test's `xfs` and `btrfs` variants get `xfs` and `btrfs`, which also need
their checksum algorithm modules: `mkmodules.py` reads `modules.dep` and
`modules.softdep`). A test whose guest script needs more says so, e.g.
`modules = ["dm_delay"]` for the benchmarks' delay device or the NFS
modules for `nfs_test`. A module the guest cannot load shows as
`init: insmod <path> failed` in the serial log; a name the kernel does not
have fails the build. The modules are decompressed when the archive is
built, and `run-qemu.sh --modules <archive>` appends it to the shared
initramfs (the kernel unpacks concatenated archives into one).

### What the guest looks like

Alpine's kernel is a general-purpose virtual-machine kernel (ACPI, EFI,
PCI, many drivers): bigger than the `tinyconfig` kernel used until Phase 24
(a 12.6 MB image against 3.5 MB), and it boots in about the same time when
only the needed modules are loaded (spike, 2026-10-07: 1.8 s against 1.7 s
under KVM with virtio_blk and ext4). Two properties of the guest do not
change:

- **No ACPI power-off.** The guest cannot power off via ACPI (the
  microvm machine has `acpi=off`, see "Fast boot"). `guest/init` instead
  runs `reboot -f` with `reboot=t` on the kernel command line: the kernel
  forces a reboot via triple fault, and QEMU's `-no-reboot` makes that
  *quit* the VM instead of actually rebooting it. If you ever see a QEMU
  guest that boots but never returns, check that `-no-reboot` and
  `reboot=t` are both still there.
- **No PCI devices.** Disks are virtio-mmio (`-device virtio-blk-device`),
  not virtio-pci; the `microvm` machine type has no PCI bus at all.

## Fast boot

The runner (`scripts/run-qemu.sh`) boots the pinned, Bazel-built
`@alpine_qemu//:qemu_system_x86_64` with the `microvm` machine type,
direct kernel boot, and no legacy PC devices this guest doesn't need:

```
-M microvm,x-option-roms=off,pit=off,pic=off,rtc=on,isa-serial=on,acpi=off
-bios <@alpine_qemu//:root/usr/share/qemu/qboot.rom>
-nodefaults -no-user-config -nographic -serial stdio
-accel kvm -cpu host        # falls back to -accel tcg -cpu max, with a
                             # warning line in the log, if /dev/kvm isn't
                             # writable
-m <mem> -smp 1              # unit tests and most e2e tests (-smp 2 for the stress,
                             # cancellation, pjdfstest and benchmark tests: "A
                             # quiet kernel" below); <mem> is the test's
                             # allowance, see "Guest memory" below
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
`CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES` parses). The first test kernel (a
`tinyconfig` build) had no ACPI at all, so with ACPI on/auto (QEMU's
default) the guest never learned where its disks were -- `info qtree` over
the QEMU monitor confirmed the `virtio-blk-device` is correctly attached to
a virtio-mmio transport either way, but `/dev/vd*` only appeared with
`acpi=off`. Alpine's kernel has ACPI; `acpi=off` stays, as what the boot
times were measured with.

**Firmware: qboot, not `bios-microvm.bin`.** `microvm`'s other stock
firmware option, `bios-microvm.bin` (a cut-down SeaBIOS build), turns out
**not** to work with `x-option-roms=off`: its `-kernel`/`-initrd` hand-off
is implemented as an option ROM, so with option ROMs disabled it falls
through to normal BIOS boot-device probing and fails with "No bootable
device" (verified experimentally while building this). qboot has no such
dependency, and has a second advantage: it detects the kernel's PVH entry
point (`CONFIG_PVH=y`, which Alpine's kernel has) and uses
it directly when present -- an even faster, effectively firmware-less boot
-- falling back to the normal Linux/x86 real-mode boot protocol for a
kernel that lacks it. So there's no PVH-vs-not branch in `run-qemu.sh`:
one firmware choice (qboot) covers both, automatically.

Step 4.4: `run-qemu.sh` takes the QEMU binary and the qboot ROM as
mandatory `--qemu`/`--qboot` arguments -- `qemu_test`/`qemu_cc_test`
(`qemu_test.bzl`/`qemu_cc_test.bzl`) pass
`$(location @alpine_qemu//:qemu_system_x86_64)` and
`$(location @alpine_qemu//:root/usr/share/qemu/qboot.rom)`. There is no host lookup, no
`PATH` search and no default path any more: `run-qemu.sh` refuses to run
(and refuses a path that merely *looks* like a host one, e.g. under
`/usr` or `/bin`) if either is missing, and logs the resolved binary path
and its `--version` output to the serial log for anyone auditing a test
run.

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

## Test tiers

A test's Bazel `size` is its tier; every `qemu_test`, `qemu_test_matrix`
and `qemu_cc_test` requires an explicit `size` and `timeout` (the macros
`fail()` without them, so a new test cannot silently default).

| Tier | Command | Contents | Wall time (KVM) |
|---|---|---|---|
| small | `bazel test --config=fast //...` | unit tests, ext4 variant of each e2e matrix test, boot, cache_permissions, lifecycle | about 1 minute |
| medium | `bazel test --config=presubmit //...` (small + medium) | xfs and btrfs variants, readdir_boundary, release_leak | a few minutes |
| large / enormous | `bazel test //...` (everything except `manual`; CI) | nfs_test and mount_dcfs_systemd_test (large), pjdfstest on three filesystems (enormous; about 10 minutes alone, about 18 when two run side by side) | about 36 minutes |

`size` also sets Bazel's resource estimate (small assumes about 20 MB), so
each QEMU test declares its real needs with tags: `cpu:<vCPUs>` (`-smp`: 1,
or 2 for the tests that keep two, "A quiet kernel") and `resources:memory:<guest allowance + 100>`
(see "Guest memory"). Bazel then schedules only as many guests as fit in the
machine. Timeouts are explicit (`short` unit, `moderate` e2e, `long` nfs,
`eternal` pjdfstest).

New tests: pick the tier from the measured duration (read it from
`bazel-testlogs/**/test.xml`).

### Which dcfs a guest runs: the invariant checks

Step 26.2 (docs/design.md, "Runtime invariant checks"). Every `small` and
`medium` `qemu_test` boots `:initramfs_checked`, whose dcfs is the testonly
checking build (`//dcfs:main_static_checked`); `large` and `enormous` boot
`:initramfs`, the plain build that ships (`qemu_test.bzl`,
`initramfs_for`). So `--config=fast` and `--config=presubmit` run every
guest against the checks, and so does CI for those tiers, while CI's large
tier runs what ships. `plain_dcfs = True` boots the plain build whatever
the tier, for a test that measures what ships: `memory_test` (its RSS
ratios would count the checker's full
check at startup filling SQLite's page cache, and its per-request sets) and
`readdir_boundary_test` (its warm listing's time limit would count the
checks' per-request cost). The
forged-request harness (`//dcfs:dir_cache_fs_test`) installs the checker
itself, in every tier.

To run one e2e test against the checking build, run it as usual if it is
small or medium; for a large one, temporarily give it `size = "medium"`.
The checking daemon says `invariant checks: on (...)` at startup.

Reading a violation: the daemon writes one line to the console (the
serial log, `bazel-testlogs/<package>/<test>/test.outputs/serial.log`) and
aborts,

```text
DCFS-INVARIANT-VIOLATION writable-open: inode 7 is open for writing but has no dirty row (in request GETATTR nodeid 1)
```

and `run-qemu.sh` fails the run with `FAIL (dcfs invariant violated ...)`,
quoting the first such line, whatever the guest script's own checks said.
The name after the marker is the invariant (the list is at the top of
`dcfs/testonly/invariant_checker.h`); "in request" names the FUSE request
being served (innermost first, then ", inside ..." for one it ran within),
or "outside any request" for startup and the periodic sync point at a
request's start. The daemon's own log has the same text after
`invariant violated:`. A violation is a bug to fix (or a rule of the design
to correct, with its reason), never a check to loosen.

## A quiet kernel

Step 26.14. Under KVM the guest clock is wall time, so what the kernel does on
its own (writeback on a timer, reclaim of cached dentries and inodes, work on
another CPU) happens at a different moment in every run: a test's behavior and
its coverage should depend on nothing but the test. `guest/init` sets these
before any test runs (the comments there say the event each removes):

| Setting | Removes | Explicit trigger instead |
|---|---|---|
| `vm.dirty_writeback_centisecs=0` | the flusher threads' wakeup every 5 s | `sync`, `fsync`, `testutil syncfs`, dcfs's sync points, `fsfreeze`, unmount |
| `vm.dirty_expire_centisecs=8640000` (a day) | writeback of dirty data once it is 30 s old | the same |
| `vm.laptop_mode=0` (already the default) | writeback after reads that spin a disk up | the same |
| `vm.vfs_cache_pressure=100` (the default, set explicitly; **not lowered**) | nothing: reclaim of cached dentries and inodes happens only under memory pressure, which `require_no_reclaim` fails on | `drop_caches` (`guest/lib.sh` `drop_caches_quiesced`) |
| one vCPU (`qemu_test` `cpus`, default 1; `run-qemu.sh --cpus`) | the daemon, the client and the kernel's threads running at once | the test's own concurrency (`&` in a script) is preempted on one CPU instead |

`vfs_cache_pressure` is not lowered because `drop_caches` reaches dentries and
inodes through the same slab shrinkers, whose counts it scales: at 1 a
`drop_caches` dropped 2 of 1091 unused dentries and `request_counts_test`,
`syscall_traces_test` and `write_test` failed (no LOOKUP or FORGET followed the
drop); at 0 the shrinkers' counts are zero. `quiet_kernel_test` checks that
`drop_caches` still drops dentries.

What stays: writeback driven by the amount of dirty memory (the test's own
write volume against the guest's RAM), the filesystems' own timers (ext4's
5 s journal commit, xfs's log worker, btrfs's 30 s transaction commit), and
`kernel.randomize_va_space`, left at 2. dcfs is PIE and nothing it does
depends on an address, except that Abseil seeds its hash tables with an
address, so ASLR changes the iteration order of dcfs's `absl::flat_hash_*`
members (`DirCacheFS::Destroy` reconciles the written files in that order);
no test asserts that order.

The tests that keep two vCPUs (`CONCURRENT_CPUS` in `qemu_test.bzl`) are those
whose point is concurrency or its cost: the stress tests (`fsstress -p`),
`cancel_test` and `cancel_inventory_test` (requests in flight while a signal
arrives), pjdfstest, and the benchmarks. A new test gets one unless its script
needs a second CPU to mean anything.

`//test/qemu:quiet_kernel_test` reads the settings back, checks that the
guest has one vCPU, and that a dirty page stays dirty for 10 s without a
`sync` (`nr_dirty` in `/proc/vmstat`) and is written by one. The determinism of
the coverage this buys is measured by `tools/coverage_diff.py` over two
`bazel coverage` runs of one commit: `docs/plan/notes/coverage-determinism-2026-10-08.md`.

## Guest timeout

`run-qemu.sh` stops the guest (`timeout`) after `TEST_TIMEOUT` less 60 s,
where `TEST_TIMEOUT` is the seconds Bazel sets for the target's `timeout`
(`eternal` is 3600), so the target's own `timeout` is the one knob and Bazel
never kills the run before the guest's log is collected. Without it (a manual
run) an e2e guest gets 1800 s (7200 under TCG) and a unit test 60 s (300).
A unit test gets at least that 60 s (300): `TEST_TIMEOUT` less 60 only when
that is more, so a `short` one (60) keeps 60 s and a `moderate` one gets
240 (step 26.2).
`TIMEOUT=<seconds>` in the environment overrides all of that
(`--test_env=TIMEOUT=...` under Bazel).

## Guest memory

Every guest prints one line just before its verdict, from a sampler that
`guest/init` runs for the whole test (every 0.1 s, one `awk` process):

```
MEM total=1018448 min_avail=964728 peak_used=54452 peak_cached=... peak_anon=... peak_shmem=... peak_slab=... dcfs_hwm=... top=<name>:<pid>:<VmHWM> samples=N up=<s> reclaim_scans=N slabs_scanned=N
```

(all KiB except `samples` and `up`, seconds of guest uptime at the last sample). `peak_used` is the highest `MemTotal - MemAvailable`: what could
not be reclaimed. `peak_shmem` is tmpfs (`/tmp`, and the initramfs itself:
about 22 MiB plain, 84 MiB under ASan, whose runtime libraries are copied
in). `dcfs_hwm` is the kernel's own `VmHWM` of the daemon. The same
`run-qemu.sh` that reads the line prints a summary of it, warns when a guest
came within 10% of running out (or 40% of `MemTotal` in tmpfs, which is
capped at half of it), and refuses to pass a run that lacks the line.
`grep '^MEM ' bazel-testlogs/test/qemu/<test>/test.log` for any test.

**`peak_used` alone is not enough: a sizing run must show `reclaim_scans=0`.**
`MemAvailable` counts the reclaimable caches (cached inodes, dentries, page
cache) as available, so a guest can be short of memory, with kswapd evicting
inodes and the kernel sending FORGETs for them, while `peak_used` looks
small: `destroy_test` at 832 MiB showed `peak_used` 349 MiB of 781 and lost
a third of its held descriptors. `reclaim_scans` is the pages kswapd and
direct reclaim scanned since the sampler started (`slabs_scanned`, the slab
shrinkers' objects, also counts `drop_caches`, so only the first is judged).
`run-qemu.sh` warns when it is nonzero; a test whose meaning depends on
nothing being evicted (`destroy.sh`, `idle.sh`, `release_leak.sh`) calls
`require_no_reclaim` (`guest/lib.sh`) and fails. For a test that holds many
inodes, size by `MemTotal - MemFree` at the end of the run instead.
`memory_test` is sized that way (step 7.4b; `memory.sh` calls
`require_no_reclaim`): its `peak_used` grows with the allowance (the page
cache and slab fill what they are given), so the table's numbers are the
`peak_used` at the allowance shown, and the allowance is the least that
showed no reclaim (plain 576 on xfs, ASan 768) plus one 64 MiB step.

**The allowance rule**: peak plus 50% or 128 MiB, whichever is more, the
peak including the kernel's own reservation (-m less `MemTotal`: about 17
MiB at 256, 30 MiB at 1024), rounded up to 64 MiB. A guest's RAM is only
resident on the host when the guest touches it (the page cache fills what
it is given, though), so the allowance mostly bounds the scheduler's
reservation and what a runaway can take, but it also keeps a test honest: a
guest at 1024 MiB hid that ASan `bench_smoke_test` was having dcfs killed by
the guest's OOM killer while the test passed.

Measured peaks (`peak_used`, MiB; KVM, 2026-10-07; ranges are across the
ext4/xfs/btrfs variants) and the allowances (`mem=` plain, `asan_mem=` for
`--config=asan`/`ubsan`, in `test/qemu/qemu_test.bzl`, `qemu_cc_test.bzl` and
`BUILD.bazel`):

| Test | peak plain | peak ASan | `mem` | `asan_mem` |
|---|---|---|---|---|
| unit tests (`qemu_cc_test`) | 22-35 | 52-612 (`dir_cache_fs_test` 612 under ASan, 383 without ReaddirWorkTest, `metadata_cache_test` 224, `backing_test` 187, the rest under 125) | 192 | 384 (`dir_cache_fs_test` 960) |
| boot, cache_permissions, lifecycle, atime, removed, copy, boundary, credentials, create, crash, handles, power, readonly, rename, setattr, release_leak, nfs, passthrough (60-102 plain) | 52-102 | 145-190 (nfs 169) | 256 | 384 |
| names, names_random, readdir_boundary, idle_short, idle_long, pjdfstest (3 shards) | 57-75 | 433-570 | 256 | 832 |
| enospc_backing (step 11.4c; ASan measured at 1536, 2026-10-08) | 72-90 | 286-314 ext4/xfs, 486 btrfs (`reclaim_scans=0`; 384 ran out on btrfs in CI) | 256 | 832 |
| enospc_cache, fault_shutdown (step 11.4c, the same way) | 69-88 | 284-312 | 256 | 576 |
| write | 136-153 | 224-248 | 320 | 448 |
| memory (sized by `reclaim_scans=0`, see above) | 104-270 at 576 | 354-478 at 768 | 704 | 832 |
| mount_dcfs_systemd (a Debian cloud image booted by systemd, 5 daemons, step 15.6) | 61-81 | 368-457 | 256 | 768 |
| bench_smoke | 79-87 | 1169-1178 | 256 | 1856 |
| destroy (20000 files, step 6.4b) | 105 | 833 | 384 | 1344 |
| cancel, cancel_inventory (cold 20000-entry listing, steps 22.1-22.2) | 89-93 (`reclaim_scans=0` at 384 for cancel; 256 scans 18073 pages) | 713-745 at 1216 (`reclaim_scans=0`) | 384 (cancel_inventory 256) | 1216 |
| bench_readdir | 157 | 1432-1509 | 320 | 2304 |
| bench_full | 569 | 2697 at 4096 (`reclaim_scans=0`; 3072 was killed by the OOM killer in CI; `dcfs_hwm` 625, five daemons at 410-625 each, see `BUILD.bazel`) | 896 | 4352 |

What fills the memory, in the plain guests: the initramfs (22 MiB of
tmpfs, always), the kernel (about 30 MiB at 1024), the page cache of the
scratch disks (a 320 MiB disk written once adds 25-90 MiB; `write_test`'s
`dd`s 150 MiB), and dcfs itself, which is small (8-11 MiB of `VmHWM`, 16 for
`bench_readdir`, 22 for `bench_full`; `memory_test` and the benchmarks show
slab growing to 140-430 MiB for 100,000 entries, mostly the kernel's dentry
and inode caches for the FUSE and backing trees, reclaimable). sqlite,
pjdfstest and the Debian chroot for `nfs_test` add nothing visible (the
chroot's rootfs is a disk, `nfs_test` peaks at 67 MiB). Under ASan, what
grows is the initramfs (+62 MiB for the runtime and its shared-library
closure) and dcfs (and the benchmark binary, and each test binary), by a
factor of 40 or more in resident set: 400-620 MiB per daemon once it holds
tens of thousands of objects.

Bazel's resource tag cannot depend on the build configuration, so
`resources:memory:` is the plain allowance + 100 MiB for QEMU's own
overhead (`QEMU_OVERHEAD_MB`) in every build: an ASan run schedules as if
it were a plain one. Run ASan with fewer jobs (`--local_test_jobs=1` or 2).

A test that outgrows its allowance fails visibly instead of hanging:

- the OOM killer's lines are printed by `guest/init` as `MEM-OOM: ...` and
  `run-qemu.sh` fails the run with `ERROR: the guest ran out of memory`
  and those lines (the console's loglevel hides them otherwise);
- a kernel oops, `BUG`, `WARNING` or panic fails the run whatever the test's
  checks said (`run-qemu.sh`, step 23.7): the console shows only the worst of
  them at loglevel 3, so `guest/init` copies the matching lines of the
  kernel log to the end of the serial log as `KERNEL-OOPS: ...` lines
  (real warnings only: the hardware-vulnerability advisories some CPUs
  print at boot, such as AMD's SRSO, contain "WARNING:" and are not
  failures);
  the one exception is `casefold_tune_oops_test` (`kernel_failure =
  "expected"`, `run-qemu.sh --expect-kernel-failure <its script>`, refused
  for any other script): it reproduces a Linux bug on purpose (below), and
  its oops is tolerated only when the guest itself reports it as `would
  FAIL (kernel: ...)`; a warning or panic still fails it;
- a guest too small for its own initramfs says `Initramfs unpacking failed`
  or panics before init, which `run-qemu.sh` reports as `the guest died
  while booting`;
- anything in between (tmpfs full, ENOMEM) fails the test's own checks with
  a `WARNING` from `run-qemu.sh` if the guest was within 10% of running out.

`bazel test --test_env=DCFS_MEM=<MiB> ...` overrides every guest's allowance
(to measure a test with room to spare, or to see how one fails). To
re-measure after a change: run the tier with `--test_output=all` (or read
`bazel-testlogs/**/test.log`) and compare the `MEM` lines with the table.

## CI

`.github/workflows/ci.yml` runs the three tiers as three jobs (`fast`,
`presubmit`, `full`) plus the sanitizer suites as two more, parallel to `full`
(`asan`, `ubsan`; each of the three is a matrix of three shards, `test.sh
--shard=I/3`; the top-level README's "Continuous integration"
section has the whole story, with the shard each large test lands in and the
expected time per shard) with the same scripts and the same timeouts as
a development machine:

- `.github/ci/prepare.sh` makes `/dev/kvm` usable when the runner has it and
  installs the host tools (README.md, "Host requirements"). Tests that run
  under TCG are visible in the log: `run-qemu.sh` prints `using tcg` and
  `.github/ci/accelerator.sh` counts them.
- Without KVM `.github/ci/test.sh` raises Bazel's timeouts (short, moderate,
  long, eternal: 300, 1800, 3600, 7200 s) and drops pjdfstest on xfs and
  btrfs: Phase 5.1 measured pjdfstest on ext4 at 3502 s under TCG against the
  3600 s `eternal` limit, and each filesystem costs that again.
- `--cache_test_results=no`: every test runs on the runner even when the
  restored Bazel disk cache holds a result for it.
- A failed `bazel test` uploads each test's `test.log`, `test.xml` and
  `test.outputs/` (the guest's `serial.log`).

First `act` runs (Phase 5.2, KVM, 4-core 12 GB machine shared with other
work; times are for that machine): `fast` 52/52 tests and `presubmit`
100/100 pass (warm cache: about 3 and 8-16 minutes; cold, the kernel, QEMU
and the mkfs tools build in about an hour). The `full` job's plain
`bazel test //...` ran 117 tests in 67 minutes (pjdfstest 721-845 s per
filesystem, `idle_long_test` 662 s) and failed only `idle_short_test_xfs`
(60 s idle saw +4 backing writes; the same test passed in `presubmit`, so it
is load-sensitive). Its `--config=asan` pass failed 9 of 117: `memory_test`
x4 (ASan inflates RSS to 7862 bytes per entry against the 256 limit),
`names_random_slow_test` x4 (since deleted: the daemon disconnected,
ENOTCONN, during its 100,000-name run) and `idle_short_test_xfs` again. These are test findings,
not host dependencies; the ASan tier of CI stays red until they are
resolved or the tests are excluded from `--config=asan`.

`bazel run //third_party/act -- -j full` runs a job locally in a container
that has only what a fresh runner has (`third_party/act/README.md`): the way
to find out that a test quietly uses a tool of your machine.

## Coverage

`bazel coverage` (`.bazelrc`, `coverage` section) builds our code with clang's
source-based coverage and collects it from the guests:

- `guest/init`: with `dcfs_cov=<disk>` on the kernel command line it sets
  `LLVM_PROFILE_FILE=/cov/%m.profraw` (one merged file per binary; chrooted
  e2e tests bind-mount `/cov`) and `dump_profraw` tars `/cov` onto that disk
  before the verdict. The serial console moves about 8 KB/s, too slow for
  profiles of a few MB, hence the disk (every guest therefore loads
  `virtio_blk`).
- `run-qemu.sh`: with `COVERAGE_DIR` set and `--cov-*` flags (passed only
  under `bazel coverage`, by `coverage.bzl`) it adds that disk (a raw 256 MiB
  sparse file, the next letter after the other disks), and after the run
  unpacks the tar and calls `scripts/cov-lcov.sh` (`llvm-profdata merge`,
  `llvm-cov export -format=lcov`, external code, tests and `testonly/`
  dropped, `/proc/self/cwd/` paths made relative) to write
  `COVERAGE_DIR/qemu-<target>.dat`, which Bazel's lcov merger folds into
  the test's `coverage.dat` and the combined report. The test's lcov is also
  kept beside the serial log as an undeclared output.
- Profiles are written in continuous mode (`LLVM_PROFILE_FILE=/cov/%m%c.profraw`,
  with `-mllvm -runtime-counter-relocation` in `.bazelrc`): the counters live
  in the profile file's mapping, so a process killed mid-exit or by SIGKILL still
  leaves a complete profile. This was found the hard way: a test's cleanup
  SIGTERMs the daemon, which may already be writing its exit-time profile, and
  libfuse restores SIGTERM's default action once the session loop ends, so one
  atime_test run in three left a truncated profile (`cov-lcov.sh` now refuses
  such a file, and the test fails). Only a guest that is cut off as a whole loses
  its processes' profiles.

- Counter increments are atomic (`-fprofile-update=atomic` in `.bazelrc`): all
  the daemons of one test share one profile file per binary, so a plain
  increment loses updates and llvm-cov then prints a branch count of 4294967295
  (`docs/coverage.md`, "Known coverage artifacts"; `tools/coverage_gate.sh`
  fails on such a count). `bazel coverage --test_env=DCFS_KEEP_PROFRAW=1`
  keeps each test's profiles under `test.outputs/profraw/`;
  `--test_env=DCFS_FORCE_CPUS=<n>` overrides the vCPU count.

- A fork must not return in both processes (`dcfs/fork_split.h`): the two
  processes share the counters, a function entered once and left twice breaks
  the flow conservation llvm-cov derives counts from, and a branch gets a
  negative count (4294967295 and its sums, `docs/coverage.md`, step 26.14f).
  `scripts/cov-lcov.sh` fails a test whose lcov has a count of 2^31 or more,
  naming the lines.

Self-checks: `//test/qemu:coverage_pipeline_test` runs an instrumented
fixture through the same `cov-lcov.sh` and requires the function that ran to
have hits and the one that did not to appear with 0 hits (`check-lcov.sh
--zero`); `.github/ci/coverage.sh` runs `check-lcov.sh` over the combined
report so an empty report fails CI.

## Gates and their self-checks

Step 26.1: every enforcement target (a gate: a test or check that rejects
something) ships a committed self-check test that verifies the gate actually
rejects known-bad input, so gates cannot rot into vacuous passes. Examples:
pjdfstest passed while busybox's `tail -1` didn't exist; ASan `bench_smoke`
passed while dcfs was OOM-killed; `quiesce_daemon` never waited because `usleep`
was missing. Each gate below has a self-check target that must fail when its
gate is disabled.

| Gate | Self-Check Target |
|------|-------------------|
| run-qemu.sh MEM-OOM detection | `//test/qemu:run_qemu_verdict_test` |
| run-qemu.sh MEM line presence | `//test/qemu:run_qemu_verdict_test` |
| run-qemu.sh boot failure detection | `//test/qemu:run_qemu_verdict_test` |
| run-qemu.sh kernel failure detection | `//test/qemu:run_qemu_verdict_test` |
| run-qemu.sh mkfs tool path validation | `//test/qemu:run_qemu_mkfs_test` |
| run-qemu.sh `--boots` (every boot a full run, the first failure stops) and `--systemd-image` (overlay, root partition, Bazel-built qemu-img) | `//test/qemu:run_qemu_verdict_test` |
| run-qemu.sh disk-spec fourth field (ext4 only) | `//test/qemu:run_qemu_mkfs_test` |
| require_commands (missing applets in guest) | `//test/qemu:require_commands_test` (sources the real `guest/lib.sh`) |
| pjdfstest-suite-sane (tooling health, tail -1 lesson) | `//test/qemu:pjdfstest_suite_sane_test` (sources the real `guest/pjdfstest_lib.sh`, which `pjdfstest.sh` calls) |
| kernel_config_test (required kernel options) | `//third_party/linux:kernel_config_test` (BuiltinTest, CheckerTest) |
| busybox_test (required applets present) | `//third_party/alpine:busybox_test_self_check` (runs the real `busybox_test.sh` over a fake busybox) |
| signature_test (Alpine package signatures) | `//third_party/alpine:signature_test` (TestKey, tampered packages) |
| tools_test (wrapper-outside-tree detection) | `//third_party/alpine:tools_test` (lines 50-54) |
| mkmodules_test (unknown module rejection) | `//third_party/alpine:mkmodules_test` (test_unknown_module_is_refused) |
| dcfs_8_test (man page sections/flags) | `//man:dcfs_8_test_self_check` (runs the real `dcfs_8_test.py`) |
| flags_consistency_test (README/main.cc match) | `//man:flags_consistency_test_self_check` (runs the real `flags_consistency_test.py`) |
| sbom_test (pins have SBOM entries) | `//tools/sbom:sbom_test` (test_every_pin_has_an_entry, test_pins_json_entry_removed_fails) |
| ownership_test (Debian image root ownership) | `//third_party/debian:mkrootfs_test` (its `--skip-ownership` control image must be user-owned, the real one root-owned) |
| rootfs_invariant_test (Debian rootfs matches tar) | `//third_party/debian:rootfs_invariant_self_check` (imports the real `header_problems`) |
| the C++ toolchain is the pinned clang (7.1) | `//tools:toolchain_self_check_test` (the checker over a gcc and a wrong-version clang info file must fail) |
| the coverage report has coverage in it (7.2) | `//test/qemu:coverage_pipeline_test` (an instrumented fixture: the function that ran has hits, the one that did not shows 0 hits; an empty lcov, a missing source and a covered function claimed uncovered are rejected by `check-lcov.sh`) |
| the toolchain takes nothing from the host but glibc's runtime (7.1b: include list, C library files, configuration file, loaded libraries) | `//tools:toolchain_hermetic_self_check_test` (the real checker over a good probe and one probe per leak: a host include directory, a host `libc.a`, a host `libxml2` loaded, a second configuration file) |
| the LLVM unpacking keeps what `toolchains_llvm` needs (7.1b skip list) | `//third_party/llvm:extract_test` (the real `extract.py` over a release-shaped archive: nothing the template or its tool list names is skipped, what is skipped is not written, no written link dangles, a wrong prefix fails; with `clangd` back in the skip list it fails naming it) |
| a dynamic guest binary's libraries come from the sysroot (7.1b) | `//test/qemu:mkinitramfs_test` (libc and the interpreter equal the sysroot's; a failing readelf and a library missing from the sysroot fail the script) |
| the shipped outputs are reproducible (26.12) | `//tools:repro_compare_self_check_test` (the comparison over a binary that embeds the build date: identical files pass, two builds at different times fail and the report names the date string); the gate is `bazel run //tools:reproducible_build` (CI job `reproducible`) |
| `dcfs/*.cc` coverage does not fall below the committed baseline (8.1; a rise passes with a note naming the new baseline) | `//tools:coverage_gate_self_check_test` (the real gate over canned lcovs: below fails with the numbers, equal and within 0.1 pass, 0.1 above passes with the note) |
| banned symbols in `//dcfs:main_static` (26.8) | `//tools:banned_symbols_self_check_test` (the real checker and deny list over a program that calls `realpath`) |
| the ASan build reports and dies (C++ runtime linked, 7.1) | `//dcfs:asan_runtime_test` (only under `--config=asan`: alloc-dealloc-mismatch must kill the process) |
| the UBSan build reports and dies (7.4) | `//dcfs:ubsan_runtime_test` (only under `--config=ubsan`: a signed overflow, a misaligned load and a vptr misuse must each kill the process; the vptr one failed to die until `-fsanitize=vptr` was named) |
| the fault-injection harness injects (11.1) | `//test/qemu:fault_selftest_test` (each mode of `guest/fault_lib.sh` on a filesystem over the wrapped disk; with `fault_table` answering `healthy` for every mode, 6 of its checks and the checks of all three failure tests below fail) |
| a power cut's first boot has a verdict (11.2) | `//test/qemu:run_qemu_verdict_test` (fake QEMUs: no marker, the marker only inside another line, a failed check, an oops on the console, a `KERNEL-OOPS:` or `MEM-OOM:` line (the guest prints the kernel log's failures and OOM-killer lines itself before the marker, from `guest/lib.sh`, whose patterns the test checks are `guest/init`'s), an invariant violation, a QEMU that ended on its own: each fails; QEMU must die of our SIGKILL, status 137) |
| the kill-mode cut keeps what was synced and loses the rest (11.2) | `//test/qemu:fault_power_kill_test_ext4` and its xfs and btrfs variants (the `before` scenario: a file synced before the cut must survive it and a file written after must not) |
| the ACE checker can fail (11.2) | `fault_ace_a_test`'s `fixtures` kind (a persistence point that was not made, and a name added behind dcfs's back, must each be reported) and `fault_power_test`'s `comparison-detects-*` checks (a name, a mode, a link count) and `snapshot-sees-contents` |
| the fsstress/fsx test's checks (11.2b: tree digests, fsx's A-OK line and disabled set, fsstress's successes and EIO, the random seed) | `//test/qemu:stress_checks_test` (the real `guest/stress_lib.sh` under the guest's busybox over trees differing in one byte, a mode, a name, a symlink target, an mtime, an xattr or a file type, empty trees, and fsx and fsstress logs that are bad, short, empty, all-failed or EIO; with the success count made to count failures, it fails on the all-failed log) |
| coverage is the same in two runs of one commit (26.14: line and branch status per file, per test) | `//tools:coverage_diff_test` (the real `coverage_diff.py` over canned lcovs: identical pass; a line or branch covered in one run only, or missing, fails naming file and line; counts differ only under `--exact`; per test it names the test that differs) |
| shipped dependency golden (26.9) | `//tools:shipped_deps_self_check_test` (the real comparison over a golden with a line removed) |
| repository shape: third_party READMEs, guest scripts used, DISABLED_ checks listed (26.13) | `//tools:repo_shape_self_check_test` (fixture trees with a missing README, an unreferenced script, an unlisted check) |
| commit subjects on the CI push range (26.13) | `//tools:commit_subjects_test` (canned subject lists through the real `.github/ci/commit_subjects.sh`) |
| syscall-trace goldens and budgets (26.3, 26.4: the reducer, the golden comparison, the budget comparison) | `//test/qemu:strace_lib_test` (the real `guest/strace_lib.sh` over canned strace output, including a golden that differs and a checkpoint's `poll` of `/dev/fuse`) |
| runtime invariant checks (26.2: each invariant the checker enforces) | `//dcfs:dir_cache_fs_test`'s `DirCacheFSDeathTest.*` (each breaks one invariant on purpose and expects the abort naming it) and `InvariantChecksReportAsAStatus` |
| run-qemu.sh invariant-violation verdict (26.2) | `//test/qemu:run_qemu_verdict_test` (a canned `DCFS-INVARIANT-VIOLATION` line fails the run; the words mid-line do not) |
| fast and presubmit tiers boot the checking build (26.2) | `//test/qemu:invariant_checks_on_test` (a small test whose daemon must say `invariant checks: on`) |
| interrupt checkpoints (Phase 22: an interrupted request replies `EINTR` at its next checkpoint) | `//dcfs:dir_cache_fs_test`'s cancellation tests (a fake interruption source, and a forged `FUSE_INTERRUPT` read by the real `SessionLoop`) and `//test/qemu:cancel_test`; without the checkpoints the harness tests fail, and `cancel_test`'s interrupted listing took 12.1 s after the signal (the population's 13.6-19.5 s in `cancel_inventory_test`) |
| fault sweep: every backing call site a short workload reaches, failed once (26.6) | `//dcfs:dir_cache_fs_fault_sites_test`'s `FaultSitesTest.SweepReportsABrokenInvariant` (an iteration that breaks an invariant is reported, naming the site it failed) |
| slopes: every operation class's steps, transactions, WAL fsyncs and backing syscalls at N = 100 and 1000 bounded by a * N + b (26.4b) | `//dcfs:dir_cache_fs_slope_test`'s `SlopeTest.AnExtraCostPerOperationIsCaught` (two more statement steps per create exceed the bound) |
| FUSE requests per user-level operation (26.4b: `guest/request_budgets.txt`) | `//test/qemu:request_lib_test` (the real `guest/request_lib.sh` delta over canned counter snapshots, and a request count above its budget failing) |
| `check_cold` / `quiesce_daemon` (guest helper, not a gate of its own) | a helper whose gate, `quiesce_daemon`'s wait, is exercised by `//test/qemu:release_leak_test` and the `written-forgotten` check of idle (`guest/idle.sh`): both fail if the daemon is not quiesced |

A gate without a self-check is a review finding: the review checklist asks
whether every gate in the tree has a self-check, and if not, why not.

## Fault injection (Phase 11)

`guest/fault_lib.sh` wraps a disk in a device-mapper device and switches its
table live (`dmsetup suspend --nolockfs`, `load`, `resume`): `healthy` (a
linear table), `error-writes`, `drop-writes`, `error-reads` and `error-io`
(dm-flakey with its `error_writes`, `drop_writes` and `error_reads`
features and an up interval of 0, so every I/O is in a down interval) and
`dead` (dm-error). `fault_wrap NAME /dev/vdb` makes `/dev/mapper/NAME`
(mount that, not the disk); `fault_mode NAME MODE` switches it;
`fault_unwrap NAME` removes it. A test declares `modules = ["dm_flakey"]`
(dm-error is in dm-mod). `dmsetup` is Alpine's `device-mapper` package
(`@alpine_dmsetup` in `MODULE.bazel`: `/sbin/dmsetup` 117 KB and
`libdevmapper` 301 KB, installed by `mkinitramfs.sh` at their Alpine paths
beside strace's musl; the static package is 1.2 MB), in every e2e initramfs
but the traced one.

An error shows up later than the table switch, because a filesystem's writes
go to its page cache and journal: an fsync, a `sync` or the journal's own
commit meets it, and ext4 then aborts its journal and remounts read-only
(`touch` fails with EROFS). `drop-writes` is the power cut: writes complete
and are lost, so after the filesystem is unmounted (its own writes dropped)
and mounted again on a healthy table it holds exactly what had reached the
disk. `guest/fault_dcfs_lib.sh` builds dcfs on two such disks (backing at
`$SRC`, cache database at `/cache`): `fd_cut` drops writes on both at one
instant (a power cut is one instant), `fd_restore` brings both back, and
`fd_freeze`/`fd_thaw` (FIFREEZE) hold the daemon inside a phase, since a
mutation blocks in its first write to a frozen filesystem: a frozen backing
filesystem holds a create in phase 2 with phase 1 durable in the cache, a
frozen cache filesystem holds it in phase 3 or in the sync point's clearing
of the dirty set. `fd_blocked` waits until the daemon is held (state D, in a
syscall on a descriptor under the path given, three looks in a row: a
`syncfs` waiting for its I/O is in state D for a moment too).

| Test | What it injects | What must hold |
|---|---|---|
| `fault_selftest_test` | each mode on a plain filesystem, and a flakey window | the mode does what it says |
| `fault_backing_test` | backing read errors during a cold lookup; backing write errors until the journal aborts, then a create | the error goes back; nothing is recorded as present or absent (the lookup after healing answers the truth); the name is not served; the daemon lives; after a restart the dirty rows are recovered and the served tree matches the backing filesystem |
| `fault_cache_test` | cache-disk write errors during a create's phase 1; the cache filesystem aborted, then a periodic sync point | the mutation never reaches the backing filesystem; the dirty set survives the failed clearing; after a restart everything served matches the backing filesystem and mutations work |
| `fault_recover_test` | each failure mode (backing reads, writes, both, no device, a flakey window) over every operation class (11.3) | the error goes back, the database never says the operation succeeded, after the recovery the same operations work and the served tree is the backing filesystem's |
| `fault_power_test`, `fault_power_kill_test` | a power cut (drop-writes, or a real kill of QEMU) before a create, between phases 1 and 2, between 2 and 3 (backing durable), after the cache has what the backing filesystem lost ("cache ahead"), and inside a sync point | after a restart every entry served matches the backing filesystem (type, size, mode, listings), and the recovery names the dirty rows that survived; a comparison that never differs would pass everything, so the last check adds a name behind dcfs's back and requires the comparison to fail |

Step 11.6 (`fault_freeze_test`, `guest/fault_freeze.sh`, ext4 small, xfs and
btrfs medium; `testutil fsfreeze` and, new, `testutil sql` for a read-only
query of the cache database while the daemon is held) measured a frozen
backing filesystem. The `freeze-table:` lines of its log are:

| Request, backing filesystem frozen | Nothing held | A mutation held |
|---|---|---|
| stat of a cached name, readdir of a filled directory, lookup of an absent name, open for read or write, read through passthrough | answered | blocked behind it |
| write through passthrough | blocks the client (the daemon is not involved); the daemon serves the rest | (n/a) |
| create, mkdir, unlink, rename, chmod, setxattr, truncate | blocked in the backing syscall (the daemon in state D); record unknown and dirty meanwhile; complete after the thaw | (the held one) |
| a sync point (`syncfs` of the frozen filesystem) | runs, returns at once, clears the dirty set | cannot run |
| `SIGTERM` | the daemon exits within 100 ms, recorded clean | waits for the thaw; the clean flag is then left unset (the held create's file is still open) and the next start recovers its dirty entry |

Each blocked check is "still running when the thaw happens, then completes".
The cut while a rename or an unlink is held is `fault_power_kill_test`'s
`frozen_rename` and `frozen_unlink` (and `fault_power_test`'s).

Step 11.3 (`fault_recover_test`, `guest/fault_recover.sh`; ext4 medium, xfs and
btrfs large, about 60 to 100 s each) closes the gap table below. Per failure
mode it makes a tree of its own on the backing filesystem, primes dcfs's cache,
drops the kernel's caches, injects the failure, runs every operation through
dcfs, reads the cache database with `testutil sql` while the daemon is idle,
lets the device recover and runs the same operations again:

| Failure mode | What is injected | The device recovers by |
|---|---|---|
| `error-reads` | dm-flakey `error_reads` | the table back to healthy; a remount if the filesystem stayed failed |
| `window` | `fault_window`: dm-flakey up 6 s, then down 20 s (`error_reads`) | the window ending, then the table switched to healthy (the cycle would repeat); the run fails if the operations outlast the down interval (`window-operations-inside-the-window`) or if ext4 needed a remount (`window-recovered-by-itself`) |
| `error-writes` | `error_writes`, then a change and a `sync` so the journal meets it | healthy table, remount, restart |
| `error-io` | both | the same |
| `dead` | dm-error | the same |

A cell has one of three outcomes, printed as the `TEST` line:

- **asserted** (PASS): the operation returned an error that is not ENOENT or
  ESTALE; or it created, removed or renamed something and the database never
  says it succeeded (a created name present, a removed or renamed name absent,
  a failed listing complete, the new mode valid, a link count of 2, an xattr
  present; no name that exists is recorded absent: a missing row in a
  directory not marked complete is the unknown record). The record checks
  read the database whatever the reply was.
- **truth-only** (PASS or SKIP): a fill (lookup, getattr, readdir) answered the
  truth from a cache the filesystem still has. With a read that reached the
  device (`sectors_read` of the dm device changed) it is a PASS; with none it
  is a SKIP, because no failure was met. At least one fill per read-failing
  mode must return an error (`<mode>-some-fill-fails`).
- **skip**: a chmod, a setxattr, a fsync that worked and a writable open that
  worked needed no device I/O (SKIP "no failure was met"; the record checks
  still run). Every reply of ENOENT or ESTALE for a name that exists is a
  FAIL (step 11.3b; before it, xfs had 18 to 23 such cells, then SKIPs).

After the recovery the same operations succeed, their effects are on the
backing filesystem, the daemon lives (the checking build aborts on a broken
invariant) and the tree dcfs serves equals the backing filesystem's. A writer
holds a file open with a dirty page across the injection, so that `testutil
fsync` meets the failure; the record of an inode open for writing is unknown
meanwhile.

Gap table (12 operation classes by 5 failure modes = 60 cells; a write
failure's "window" is its `error-writes` cell: a shut-down filesystem does not
recover by itself). Before 11.3 two cells had a check, one (readdir by
error-reads) a partial one, and 57 were gaps; all 60 now have checks (names
`<mode>-<name>`), of which the first named is the cell's failure-met check:

| Operation | Covered before 11.3 | Checks of `fault_recover.sh` | Cell outcome |
|---|---|---|---|
| lookup fill | `fault_backing:lookup-read-error-replied`, `lookup-after-heal-present` (error-reads only) | `lookup-fails`, `lookup-not-recorded-absent`, `lookup-after-recovery` | truth-only: pass on an error; SKIP if answered without a device read |
| getattr fill | gap | `getattr-fails` (a stat of the chmod's target), `setattr-name-not-recorded-absent`, `getattr-after-recovery` | truth-only (it follows a chmod that may have worked) |
| readdir fill | `fault_backing:listing-after-heal` (error-reads only, after the heal) | `readdir-fails`, `readdir-not-marked-complete`, `listed-names-not-recorded-absent`, `readdir-after-recovery` | truth-only; the first readdir after a failing read replies EIO (ESTALE before 11.3b) |
| create | `fault_backing:create-error-replied`, `-name-not-present`, `-listing`, `restart-recovers-dirty` (error-writes only) | `create-fails`, `create-not-recorded-present`, `create-after-recovery` | asserted |
| mkdir | gap | `mkdir-fails`, `mkdir-not-recorded-present`, `mkdir-after-recovery` | asserted |
| unlink | gap | `unlink-fails`, `unlink-not-recorded-absent`, `unlink-after-recovery` | asserted |
| rename | gap | `rename-fails`, `rename-source-not-recorded-absent`, `rename-target-not-recorded-present`, `rename-after-recovery` | asserted |
| setattr | gap | `setattr`, `setattr-record-is-the-new-mode` or `setattr-new-mode-not-valid`, `setattr-after-recovery` | skip where it worked (ext4 before the abort); asserted records; EIO on xfs/btrfs (11.3b) |
| setxattr | gap | `setxattr`, `setxattr-not-recorded-present` or `-succeeded-not-recorded-absent`, `setxattr-after-recovery` | as setattr |
| link | gap | `link-fails`, `link-not-recorded-present`, `link-count-not-valid-as-2`, `link-after-recovery` | asserted |
| symlink | gap | `symlink-fails`, `symlink-not-recorded-present`, `symlink-after-recovery` | asserted |
| write-through data | gap | `data-attributes-not-valid-while-open`, `data-fsync-fails`, `data-open`, `data-after-recovery`, `failed-write-inode-still-dirty` (after a failed fsync and an in-place recovery); `effects-on-backing` and `served-equals-backing` close every mode | `data-fsync-fails` asserted where writes fail; SKIP where the fsync worked; `data-open` skip where it worked |

What the filesystems answer (the `fault_recover:` lines of the logs):
ext4 fails a read with EIO and, once its journal has aborted, answers every
change with EROFS (an fsync through dcfs: EROFS; a chmod that only needs the
journal may still work before the abort); after a read error on a block
bitmap it refuses to allocate in that group with EUCLEAN until it is mounted
again (`data-after-remount`). xfs and btrfs answer EIO to every change and
stay failed once the device is back: every mode needs the remount, and only
ext4's `error-reads` and `window` recover in place. A cold inode that xfs or
btrfs cannot read makes `open_by_handle_at` fail with ESTALE. dcfs used to
forget the row and reply ESTALE (the first readdir) or ENOENT (a chmod, a stat
or a setxattr of a name that exists); since step 11.3b it looks the name up
through the parent's descriptor and replies EIO, keeping the row (attributes
unknown), unless the name is gone or names another handle.

A btrfs inode read that fails warns in the kernel (`btrfs_destroy_inode`,
`fs/btrfs/inode.c:8047`): a directory whose inode item was updated only in
memory (a relatime atime update from a listing) takes `btrfs_fill_inode`'s fast
path with `index_cnt = -1`, and when the on-disk read then fails
`iget_failed`'s `make_bad_inode` makes it `S_IFREG` and `btrfs_destroy_inode`
reads `index_cnt` as `csum_bytes` (shared storage since v6.11): a spurious
warning, not a data bug (`docs/plan/notes/kernel-bugs-2026-10-09.md`). The
harness fails a boot on a WARNING. `fault_recover_test_btrfs` therefore pins
every inode of its tree (`O_PATH` descriptors on the files, working directories
on the directories), and its failures meet metadata, never an inode read, so
the ESTALE path is not exercised there. `fault_recover_btrfs_unpinned_test` is
the same script without the pinning (`dcfs_pin=0` on the kernel command line),
with `kernel_failure = "expected"` (`run-qemu.sh` tolerates exactly that
warning, and only if the guest reports it as `would FAIL (kernel: ...`, which
the `DISABLED_BTRFS_FAILED_INODE_READ_WARNS` check does): its log has the
ESTALE and ENOENT replies and the warning on record, and
`run_qemu_verdict_test` checks the tolerance. A fixed kernel logs no warning
and the test passes.

`fault_selftest_test` checks `fault_window` (reads work in the up interval,
fail inside the down one and work after it: `window-works-before`,
`window-fails-inside`, `window-works-after`).

Not covered: a failure that starts during a mutation (the freeze tests and
the fault sweep of `//dcfs:dir_cache_fs_fault_sites_test` cover phases);
`error-writes` as a window. After a failed write-back, the size dcfs
recorded from the filesystem's cached inode can differ from the size of the
inode re-read from disk once it is evicted: the in-place comparison compares
that file on its own, prints the difference, and asserts that the inode is
still in the dirty set and that the writer's fsync failed
(`failed-write-inode-still-dirty`); a restart's recovery makes it equal. The
limit is in the user-facing README's Limitations.

How these checks were shown to fail: with `inject` a no-op, 98 checks of the
ext4 run failed (every mutation succeeded, and the database said `present`,
`absent` and a link count of 2); inverting two record assertions failed them
with the database's real answers (`''`, no row in a directory not marked
complete, and `norow-incomplete`). The shipped script has neither change.

Step 11.2 added three things:

- **The tests over three backing filesystems.** `fault_backing_test`,
  `fault_cache_test` and `fault_power_test` are `qemu_test_matrix` (ext4
  small, xfs and btrfs medium); the cache disk stays ext4. A failed xfs
  answers every access with EIO and a failed btrfs lists nothing, so
  `fault_backing` requires that dcfs's listing equal the backing
  filesystem's own and never show the failed create.
- **Real power cuts** (`fault_power_kill_test`, `run-qemu.sh --power-cut`).
  The same `guest/fault_power.sh` and checks as `fault_power_test`, with the
  cut a real one: per scenario two boots over the same disk images, the first
  until the guest prints `DCFS-POWER-CUT-NOW`, at which the host kills QEMU
  (SIGKILL: no flush, no shutdown); the second, without formatting, runs the
  checks. A killed QEMU keeps every write the guest completed to a disk and
  loses the guest's page cache, which is the state `drop-writes` models, so
  the two tests passing the same checks, on all three filesystems, shows that
  the model and a real kill agree at these five points. Neither loses a write
  the disk acknowledged without a flush, so what a missing FLUSH or FUA could
  reorder is not tested. Boot 1 is held to the verdict rules of any boot (a
  failed check, a kernel failure, an invariant violation, a guest out of
  memory before the marker fail it, and QEMU must have been running at the
  marker and been killed by us, not by the time limit), and the `before`
  scenario is the witness that the kill keeps what the guest synced and loses
  what it did not: a file fsynced on the backing filesystem must be there in
  boot 2, one written afterwards must not. The 2n boots of a test share its
  time limit equally. Coverage of each boot 2 is written under its own name.
  the model. The `ahead` scenario needs no drop on the backing disk here: the
  journal's commit interval is made long (`-o commit=600` where the
  filesystem takes it) so the creates are still in its running transaction.
- **ACE-style sequences** (`fault_ace_a_test`, `fault_ace_b_test`,
  `fault_ace_fs_test`, `guest/fault_ace.sh`): sequences of one or two
  operations from `create mkdir unlink rename replace link xrename chmod
  append` on a small tree, a power cut (both disks drop writes) after them, a
  remount, a restart, and then `served`: the tree dcfs serves equals the
  backing filesystem's (type, size, mode, links, the md5 of every file); and,
  for sequences with a persistence point, `durable`: the backing filesystem is
  exactly what it was at that point. Two persistence points, because they leave
  different states: an fsync through dcfs (`testutil fsync`; the backing
  filesystem durable and the dirty set cleared, the cache possibly ahead by
  what phase 1's fsync made durable) and a syncfs of the backing filesystem
  itself behind dcfs (the backing filesystem durable, the cache's phase 3 not:
  recovery has the dirty entries to forget; an fsync of the directory alone
  persists no file data, which the first run showed on `append`). No sequence cuts inside a
  mutation (`fault_power_test` does). `dcfs_ace_kinds=` and `dcfs_ace_ops=` on
  the kernel command line pick the kinds (`one pair persist split direct dsplit
  fixtures`) and the operations: target a runs 182 sequences (about 10
  minutes, `eternal`), b 90, and the xfs and btrfs variants 62 over four
  operations; all boot the checking build although they are large, since
  recovery is what they exercise. The `fixtures` kind is the checker's negative
  fixture: a persistence point that was not made, and a name added behind
  dcfs's back, must each be reported.

The small and medium ones run with the checking build of dcfs
(`initramfs_checked`): an error path that leaves an invariant broken aborts
the daemon and the test fails (the large ones run the plain build, except the ACE
targets). Each uses the default guest
memory (every MEM line shows `reclaim_scans=0`: peak 69 to 126 MiB plain, xfs
the highest, and 158 MiB under ASan, of 256 and 384). The cache disk's I/O errors (`SQLITE_IOERR`, and `SQLITE_READONLY` from a cache
filesystem that aborted its journal) reach the caller as EIO: `sqlite.cc` attaches
the errno payload and keeps the status code `UNAVAILABLE`; `fault_cache_test`
requires it (it first said EAGAIN, "Resource temporarily unavailable").

`fault_shutdown_test` (step 11.5, `qemu_test_matrix`: ext4 small, xfs and
btrfs medium) crashes the backing filesystem itself, under a running dcfs:
`testutil shutdown <path> <default|logflush|nologflush>` is
`FS_IOC_SHUTDOWN`, xfstests' `godown` (the filesystem fails every operation
until it is unmounted and mounted again; `default` freezes first,
`logflush` commits the journal, `nologflush` nothing). btrfs has the ioctl
only from Linux 6.19, behind `CONFIG_BTRFS_EXPERIMENTAL`; there the test
turns the disk into dm-error and makes btrfs abort its transaction (a
direct create and a `syncfs`). ext4 is mounted `commit=60` so that "not yet
durable" holds for the whole run. A shut-down xfs refuses `FITHAW` (EIO)
and stays frozen, so its variant skips the create held in phase 2 by a
freeze.

| Test | What it injects | What must hold |
|---|---|---|
| `fault_shutdown_test` | per flavour: unsynced completed mutations (create and data, mkdir, rename, setxattr) and a create held in phase 1 when the backing filesystem crashes; a create held in phase 2; a crash while idle, then a start without a remount; a daemon crash and a restart (which re-reads the directory) before the crash; a crash with dirty rows, then a start without a remount; the backing disk's writes failing until the filesystem goes read-only by itself (btrfs's transaction abort, ext4's `emergency_ro`; xfs shuts down), with sync points every second | the held create, new mutations and reads of contents fail; nothing is served that was not served before; the clean shutdown keeps the dirty set (each restart recovers rows); a start without a remount (refused on xfs, whose open of `--source` fails, and over a filesystem that went read-only by itself) serves nothing new, and a create through it fails; no sync point over a filesystem that went read-only clears the dirty set (the bug step 11.5's review found: btrfs's second sync point did, and the lost directory was served after the remount); after the remount everything served matches the backing filesystem, `nologflush` lost the unsynced create and `default`/`logflush` kept it, and the change the restart after a daemon crash re-read is not served once the crash lost it (12.6's dirty rows kept until a sync point; the pre-12.6 code failed this) |

`fault_shutdown_test` boots the checking build in all three variants (ext4
small, xfs and btrfs medium) with the default guest memory.

Out of space (step 11.4): `fd_fill DIR` (`guest/fault_dcfs_lib.sh`) takes
every free block of the filesystem DIR is on (fallocate, then blocks
appended until `ENOSPC`, as root, so ext4's reserved blocks go too).

| Test | What it injects | What must hold |
|---|---|---|
| `enospc_backing_test` (`qemu_test_matrix`: ext4 small, xfs and btrfs medium) | the backing filesystem full before dcfs starts; then through dcfs: creates until one fails, a 64 KiB write past a file's end, a mkdir, a 2000-byte xattr, a rename into the full directory | each that fails fails with `ENOSPC` and is not served as done (served tree and xattr equal the backing filesystem's); on ext4 and xfs every one fails, on btrfs (metadata reserved apart) only the write must; after the fill is removed through dcfs the same operations work, also after a restart |
| `enospc_cache_test` (ext4, small) | the cache database's filesystem full: before a create (phase 1), while a create and then a rename are held in phase 2 (phase 3), before a periodic sync point | the create fails with `ENOSPC` and never reaches the backing filesystem; the held create is replied as done or `EEXIST` and the kernel never answers "no such file" for it (the bug this step fixed), the held rename is replied as done; reads fail with `ENOSPC` or answer right; the failed sync point keeps the dirty set; after space is freed and a restart, everything served matches the backing filesystem |

Both boot the checking build in every variant (ext4 small, xfs and btrfs
medium) with the default guest memory.

## Syscall traces

`//test/qemu:syscall_traces_test` (step 26.3) pins the backing-filesystem
syscalls of each operation, observed with Alpine's `strace` on the running
daemon rather than from dcfs's own accounting. `strace` and its musl closure
are `@alpine_strace` (`MODULE.bazel`), installed by `mkinitramfs.sh` at their
Alpine paths (`/usr/bin/strace`, `/lib/ld-musl-x86_64.so.1`, `/usr/lib`:
about 3.8 MB in every e2e initramfs; Alpine has no static strace, and the
guest has no musl loader of its own, so the dynamic binary carries it).

To trace one operation in a guest script (`guest/strace_lib.sh`, sourced
after `lib.sh`; `DAEMON_PID`, `SRC` and `DB` set):

    strace_op NAME command...      # quiesce, attach, run, quiesce, detach
    strace_golden CHECK NAME <<'EOT'
    openat(backing)
    EOT

`strace_op` writes `/tmp/strace/NAME.{raw,all,trace,counts}`: `.all` is one
`name(kind)` line per traced syscall (` !ERRNO` appended on failure), kind
being `backing`, `cache`, `procfd` (`/proc/self/fd/N`, how dcfs reaches a
backing object through a descriptor), `proc`, `fuse` or `other`; `.trace`
keeps `backing`, `procfd` and `other` (a golden never has `other`), and
`.counts` the number of calls of each kind for step 26.4's ratchets.

Under `--config=ubsan` there is one more kind, `sanitizer`: UBSan's vptr check
(`-fsanitize=vptr`) probes whether a vtable prefix is readable the first time
it sees a (static type, dynamic type) pair, in the daemon's own process, with
`pipe2`, `fcntl` (`F_GETFL`, `F_SETFL O_NONBLOCK`), a 16-byte `write` and two
`close`s (compiler-rt's `IsAccessibleMemoryRange`). No report is printed and
nothing is wrong; the probe lands in whichever trace first reaches a new
pair (`cold-lookup`, the daemon's first request). The reducer files exactly
that sequence under `sanitizer`, and only when the guest's `DCFS_SANITIZER`
(from `qemu_test.bzl`, through the kernel command line) says `ubsan`; `.trace`
leaves it out and `.counts` counts it. Any other use of a pipe, and the probe
in a plain or ASan guest, stays `other` and fails the golden
(`strace_lib_test` checks all three).

To update a golden, run the test, read the diff it prints (the full trace and
raw strace output follow it), and replace the heredoc in
`guest/syscall_traces.sh`. A golden change needs a sentence of
`docs/design.md` that explains the new syscalls: the comment above each golden
names the passage that explains it. Run the daemon with
`--sync_interval_sec=1000000`, or the periodic `syncfs` lands in whichever
trace is running at the time.

### Budgets

`guest/syscall_budgets.txt` (step 26.4) holds, per operation, the most calls
of each kind it may make: `backing` and `procfd` syscalls and `sync` (fsync,
fdatasync and syncfs of any kind, so the cache database's WAL fsyncs count)
from strace, and `sql_stmts` (statement steps) and `sql_txns` (outermost
`BEGIN`s) from the checking daemon's cost counter (step 26.4b,
`dcfs/testonly/cost_counter.h`: `$DCFS_COUNTERS_FILE`, read by
`counter_value`; the checker's own statements are left out; until 26.4b
they were counted from `--v=2` log lines, with the same results). `strace_budget` fails an operation
whose count rose above its budget, naming both numbers; a count below passes.
The budgets started at the observed counts. Raising one is a deliberate edit
of the file whose commit says why. The goldens pin the order; the budgets pin
the counts where an order may legitimately change. Only deterministic counts
are budgeted: no time, no sizes, and the cache database's own syscalls
(`cache`) are reported, not gated. The gate's self-check is
`//test/qemu:strace_lib_test`.

### Request budgets

`guest/request_budgets.txt` (step 26.4b) holds, per user-level operation
(`ls -l` of 100 entries, `find` over a tree, `stat` of a path 10 deep, `cat`
of a file), the most FUSE requests of each opcode (LOOKUP, GETATTR,
READDIRPLUS, READDIR, OPENDIR, OPEN) the kernel may send dcfs for it, on a
cold kernel cache and a warm dcfs cache. `//test/qemu:request_counts_test`
counts them with the checking daemon's cost counter (`$DCFS_COUNTERS_FILE`,
`guest/request_lib.sh`) and compares them as the syscall budgets are
compared. The budgets started at the observed counts.

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
  "Sanitizers" below), and the resulting initramfs carries its shared
  libraries and loader, copied from the pinned toolchain's glibc sysroot
  (`@dcfs_llvm//sysroot`, step 7.1b), not the host's.
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

**At most four virtio disks.** A guest sees four of the disks it is given
and no more: with five `-device virtio-blk-device` drives (a `vda` filler, the
test's `vdb`, `vdc`, `vdd` and a fifth) the kernel's command line has five
`virtio_mmio.device=` entries and `/proc/partitions` four disks (measured
2026-10-09, step 15.6). A disk-spec that starts at `vdb` makes `vda` a filler
that takes a slot, the Debian rootfs, the systemd image and the coverage disk
come after the last spec: a test that needs the most starts its specs at
`vda` (`mount_dcfs_systemd_test`: ext4 `vda`, xfs `vdb`, btrfs `vdc`, the
image `vdd`).

### Sanitizers

```
bazel test --config=asan //dcfs:sqlite_test //dcfs:metadata_cache_test //dcfs:backing_test
```

works the same way: `test/qemu:asan_build`/`test/qemu:ubsan_build`
(`config_setting`s matched on `--define=dcfs_sanitizer=asan`/`ubsan`, set
by `.bazelrc`'s `build:asan`/`build:ubsan`) tell `qemu_cc_test` to skip the
`fully_static_link` feature, and `mkinitramfs.sh` to copy the dynamically
linked test binary's shared-library closure (and ELF interpreter) into the
initramfs instead, from the toolchain's glibc sysroot (`DCFS_SYSROOT` and
`DCFS_READELF`, set by the genrules through `guest_libs.bzl`). ASan needs `ptrace` to symbolize leak reports, which the
guest doesn't have configured; `guest/init` sets
`ASAN_OPTIONS=detect_leaks=0` (only if unset) so that limitation doesn't
fail every ASan build.

`--config=ubsan` is `-fsanitize=undefined -fsanitize=vptr` with
`-fno-sanitize-recover=all` (any report kills the process, so the test
fails) and the C++ runtime linked (`-fsanitize-link-c++-runtime`). Named
explicitly because clang 22's `undefined` omits `vptr`, so `//dcfs:ubsan_runtime_test`
would not catch the misuse. Deliberately left out: `implicit-conversion`
(measured 2026-10-07: the first report of every unit test is SQLite's
`sqlite3.c:190778`, a defined `int` to `unsigned` sign change, and it ends
the run) and `unsigned-integer-overflow` (defined wraparound that Abseil's
hashing and SQLite rely on; `-Weverything` finds our own conversions at
compile time),
`float-divide-by-zero` (defined by IEEE 754), `local-bounds` (traps without
a message; `array-bounds` in `undefined` reports), `nullability` (we have no
annotations). `guest/init` sets `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`
(only if unset); the guest has no llvm-symbolizer, so a stack is addresses
and module offsets, while the report's first line names the source file,
line and column from the compiler's own data. The whole suite passes
under it with no report in our code, Abseil, SQLite, libfuse, liburing or
numactl, so there is no suppressions file (`UBSAN_OPTIONS=suppressions=` is
the way to add one, with a reason per line). UBSan guests need no more
memory than ASan's `asan_mem=`; `memory_test_xfs` reclaims
(`reclaim_scans` > 0) in the plain build too, at its 448 MiB.

Tests that run on the host and do not depend on how our code is built (TLC,
the man page and flag checks, the repository-shape, SBOM and toolchain checks,
the shell and Python tests of the test tooling: 98 targets) carry
`target_compatible_with = HOST_ONLY_COMPATIBLE` (`test/qemu/host_only.bzl`;
the `tlc_test` and `tla_trace_log_test` macros add it themselves), so
`--config=asan`, `--config=ubsan` and `bazel coverage` skip them instead of
rerunning them under a new configuration hash (`formal:large_test` and
`nolock_test` alone were 19 minutes per sanitizer job): 100 of the 226 test
targets are skipped under each sanitizer (101 under coverage), 2 in the plain
build. A new host-only test gets the same line. Tests that read the build's
own flags or binaries (the `flags_test`s, `banned_symbols_test`) are not
host-only and say which builds they are for.

The tools a guest test uses (QEMU, `mke2fs`/`debugfs`, `mkfs.xfs`,
`mkfs.btrfs`, busybox) are downloaded Alpine packages, not built here, so
the sanitizer flags cannot touch them and `--config=asan`/`--config=ubsan`
use exactly the plain configuration's files. (Until Phase 24 they were built
from source in the exec configuration, with `exec_file` forwarding rules and
two tests, `tool_identity_test` and `tool_keys_test`, to prove it; all of
that is gone.) `//third_party/alpine:tools_test` runs them: QEMU with KVM,
the mkfs tools, busybox.

## e2e tests (`qemu_test`)

`boot_test` and `readonly_test` (`test/qemu/BUILD.bazel`) are the bigger,
slower, whole-daemon tests: they boot `//dcfs:main_static` and
`//tools:fhtest` in a shared initramfs and drive them through a
`guest/<name>.sh` script picked by the `dcfs_test=` kernel command-line
parameter. They're tagged `e2e` (not `qemu` -- that distinction is gone now
that everything is QEMU) plus `no-sandbox`, `requires-kvm` and the
resource tags in "Test tiers" (no longer `exclusive`), and run as part of
a plain `bazel test //...`; `--config=fast`/`presubmit` select tiers.

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

**Step 11.2b: fsstress and fsx.** `stress_{short,long}_test_<fstype>` run
xfstests' `fsstress` (four or eight processes of random namespace and data
operations, fixed seeds) and `fsx` (random reads, writes, truncates and mmap
operations checked against its own model) against dcfs, then compare the tree
seen through dcfs with the backing file system's own (`stress_digest` in
`guest/stress_lib.sh`) three times: right after the run (mostly the kernel's
FUSE caches, with their one-hour timeouts, answer), after `drop_caches` with
the same daemon running (dcfs's own cache answers), and after a restart
(recovery, then cold lookups). In the small and medium tiers the daemon is
the checking build, which aborts on a broken invariant, so its survival is a
check. `stress_short_test_<fstype>` is medium on all three;
`stress_long_test_<fstype>` is large (about 280 s); `stress_random_test_<fstype>`
is `manual` (name it on the command line; CI's sharded `test.sh` skips it
too) and seeds both tools from the kernel's random pool and prints the seeds.
To resize or replay it, give the target `cmdline = "stress_ops=N
stress_fsx_ops=N stress_seed=S stress_fsx_seed=S"` (any subset). What is not
built (AIO, io_uring, btrfs subvolume operations, XFS ioctls) is in
`third_party/xfstests/README.md`.

What the comparison covers: names (sorted), type, mode, link count, owner,
size, symlink target, device numbers, every xattr, mtime and ctime, and the
md5 of every regular file. What it leaves out: atime, directory sizes,
`st_blocks`, inode numbers and generations, and the records of absent names
(they show only through a listing that lacks the name and through the
lookups the run made, which fsstress does not check).

Only the short run checks `require_no_reclaim`. The long and random runs
write more file data than the guest has memory (page cache of 400+ MiB in
512 MiB, about 1.5 M pages scanned), so page-cache reclaim and the FORGETs it
sends are expected, and they assert nothing reclaim can invalidate: every
comparison is against the backing file system, none counts held inodes or
FORGETs.

What the run does not prove. fsstress exits 0 whatever its operations
returned, so the test prints every operation with its errno (`op errno
count`, errno 0 succeeded), requires at least 0.2% of the operations to
succeed as each of creat, mkdir, link, symlink, rename, unlink and write, and
fails on any EIO; the rest may fail. On a FUSE mount these fail every time or
often: clonerange, deduperange and fiemap (ioctls dcfs does not implement,
errno 95 or 25), dread and dwrite (O_DIRECT; the alignment query is an XFS
ioctl), the XFS ioctls (bulkstat, resvsp; frequency 0), the fallocate modes
dcfs does not forward (collapse, insert, unshare, write-zeroes), and
whichever of copyrange, splice and the rename flags the kernel or backing
file system refuses. fsx turns the same kinds of feature off at its first
failure; the test lists what it disabled and requires exactly the expected
set (measured on ext4, xfs and btrfs alike): clone range, dedupe range,
RWF_DONTCACHE, and the fallocate modes collapse, insert, unshare and
write-zeroes (atomic writes need O_DIRECT, which fsx only uses with `-Z`). A
feature that starts working changes the set and fails the test until the
expectation in `guest/stress.sh` moves. What the run does prove is that
whatever the operations did, dcfs and the backing file system agree
afterwards, that nothing returned EIO, and that the daemon survived.

**Step 6.2: pjdfstest shards.** `pjdfstest_test_<fstype>` is a `test_suite`
over three guests, `pjdfstest_{rename,chown,rest}_test_<fstype>`, each a
three-line wrapper (`guest/pjdfstest_<shard>.sh`) that sets `PJD_SHARD` and
sources `guest/pjdfstest.sh`. The assignment is by test directory and fixed
in `shard_of()` there (`rename/` and `chmod/` (moved from the chown shard in step
6.5, which was the longest by far); `chown/`; every other
directory, including any pjdfstest adds later), so it does not depend on
file order. Each shard runs the same two-run comparison on its own
directories and applies only the part of
`pjdfstest.<fstype>.expected_failures` that names them; the guest refuses to
start without a valid shard and fails when more than 5% of the checks fail
directly on the raw filesystem (a broken guest toolchain makes every check
fail on both sides and look "not dcfs's fault": see the busybox
`FEATURE_FANCY_TAIL` note in `docs/plan/log.md`).

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
  CLI -- usage/option validation, a missing or non-directory SOURCE, a
  cache database refused because it belongs to a different filesystem,
  `dcfs.fuse_opt` (good, bad and redundant options), a clean SIGTERM
  shutdown (exit 0, unmounted, WAL checkpointed), mounting dcfs back over
  its own SOURCE, and restarting against a previously-used cache
  database. All of it through `mount.dcfs -o dcfs.fstype=none,...,
  dcfs.foreground` (step 15.2: the plain `--source` command line is gone).
- `mount_dcfs_test` (`guest/mount_dcfs.sh`): the `mount.dcfs` wrapper
  (steps 15.1-15.2) on the ext4 image -- the capture of the backing
  filesystem (`dcfs.fstype` absent or `ext4`: served, the FUSE mount's
  source is the spec, no backing mount in the caller's namespace, `umount`
  stops dcfs, SIGKILL releases the superblock), the option split (`ro` vs
  `dcfs.ro`, `remount` touching only the dcfs mount, an unknown `dcfs.`
  option and a failing native mount mount nothing and say why, `-f`, a file
  as mount point), daemonization (stdio on `/dev/null`, own session, syslog
  through busybox's `syslogd`, a failure after the fork reported by the
  wrapper, `dcfs.foreground`), the `bind` and `none` forms, a file mounted
  below the source refused, and a non-root caller. busybox's `mount` runs
  no helpers, so `mount -t dcfs` is a SKIP here (step 15.6 runs it with
  util-linux). Instance identity (15.3), stubs (15.4), fsck and exports
  (15.5) and systemd (15.6) come later.
- `handles_test` (`guest/handles.sh`): step 3.4b's NFS export handles
  (`FUSE_CAP_EXPORT_SUPPORT`, exercised with `//tools:fhtest`) -- a handle
  for a file on the source device opens and
  reads back the right content; the reported generation is 0 for the root
  and nonzero (and stable across a restart) for everything else; a handle
  survives a daemon restart against the same cache database; a doctored
  generation is rejected with ESTALE; an inode number recycled behind
  dcfs's back invalidates the old row so its handle comes back ESTALE; and
  wiping the cache database -- the one case that does *not* survive --
  also yields ESTALE, from a freshly reseeded generation counter. Two
  consequences of the exclusive-access model (no write-through
  invalidation until Phase 4) that this test cannot demonstrate are
  reported as `SKIP`, not a faked pass. A check that fails for a reason
  outside dcfs (a kernel gap) is kept and disabled googletest-style:
  `disabled` (`guest/lib.sh`) reports it as `TEST DISABLED_<name>
  DISABLED (<reason>)` and its would-be verdict ("would PASS"/"would
  FAIL") on the next line, never failing the run. The one kernel
  bug kept this way is `casefold_tune_oops_test`'s
  `DISABLED_casefold-tune-online-oops`, the reproducer (Linux 6.18 to
  7.3-rc): `EXT4_IOC_SET_TUNE_SB_PARAM` (`testutil ext4-tune-casefold`)
  switches casefold on under a mounted ext4 without loading `sb->s_encoding`
  (only mount does), `chattr +F` checks only the feature bit, and the next
  readdir of the directory dereferences NULL in `utf8byte`. It would PASS
  when the listing works or the kernel refuses the ioctl or `+F`. Also step 4.8's runtime submount
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

(plus `mem=`/`asan_mem=` if its `MEM` line, in the serial log, says it needs
more than the default 256/384 MiB; see "Guest memory")

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

An ext4 disk tuple may carry mke2fs options as a fourth element, e.g.
`("vdb", "ext4", "320M", "-O casefold -E encoding=utf8")` (`copy_test`: a
filesystem on which `chattr +F` works); `qemu_test_matrix` keeps them for the
ext4 variant only. Make such features at mkfs time: switching casefold on
under a mounted ext4 (`EXT4_IOC_SET_TUNE_SB_PARAM`) leaves the kernel without
the encoding, and the next readdir of a casefolded directory oopses.

which generates `<name>_test_ext4`/`_xfs`/`_btrfs` plus a plain
`<name>_test` alias to the ext4 variant -- see `guest/lib.sh`'s
`backing_fstype` for how a script detects which filesystem it is actually
running against, needed only if the test has a genuine filesystem-specific
branch (most don't: see the "Step 5.2" paragraph above).

`guest/init` runs the script named by the `dcfs_test=` kernel command-line
parameter (the macro fills this in from `guest_script`'s basename) via
`sh`, then reports `ALL-TESTS-PASSED` or `TEST-FAILED` based on its exit
status.

## The systemd guest (step 15.6)

`mount_dcfs_systemd_test` runs `mount.dcfs` the way a machine runs it: through
util-linux's `mount(8)`, `/etc/fstab` and systemd's mount units, in a
released Debian cloud image booted with systemd as PID 1. The busybox guest
of `mount_dcfs_test` calls the wrapper directly (busybox `mount` runs no
`mount.<type>` helpers), so it cannot show what libmount adds to the helper's
options, how a remount finds `mount.fuse.dcfs`, which source libmount hands
over, what status `mount(8)` returns, or what systemd does with the daemon
(`third_party/debian_cloud/README.md` has the image, its pin and how to move
it).

How it boots, in `qemu_test(systemd_image = ..., boots = 2)`:

1. `run-qemu.sh --systemd-image IMAGE --qemu-img QEMU_IMG` makes a qcow2
   overlay on the pinned image (the image itself is never written) and
   attaches it as the next virtio disk after the test's `disks` (`vdc` when
   the test has `vdb`). The kernel is the project's test kernel, the
   initramfs the usual one with the modules for `ext4`, `fuse` and
   `virtio_blk`; `dcfs_systemd=/dev/vdc1` (the image's root partition) is on
   the command line.
2. `guest/init` mounts that partition, installs dcfs, `testutil`, `fhtest`
   and `/tests` in it (`install_dcfs_into`, the same function the NFS test's
   chroot uses), runs `guest/systemd_install.sh` (the oneshot unit, the
   masked getty templates and network services, an empty `/etc/fstab`), moves
   `/proc`, `/sys` and `/dev` over and `exec switch_root`s into systemd.
3. `dcfs-test.service` (after `multi-user.target`) runs
   `guest/systemd_run.sh`: it starts the memory sampler again (the one
   `guest/init` started lives in the old root; the program is the file
   `/dev/memsampler.awk` they share), runs `guest/mount_dcfs_systemd.sh`,
   prints the `MEM` line, the kernel's failures and `ALL-TESTS-PASSED` or
   `TEST-FAILED` on the serial console like every other guest, and runs
   `systemctl reboot`. The reboot stops the mount units and ends QEMU (no
   ACPI, `-no-reboot`).
4. `--boots 2` boots the same disks again for the checks that need a reboot,
   each boot a full run with its own `MEM` line and verdict
   (`boot1.log`, `boot2.log` in the test's outputs); the script reads
   `dcfs_boot=` from the command line.

A guest that goes wrong before the script (a unit that fails, emergency mode)
shows nothing of why: `ShowStatus=no` keeps systemd's own lines off the
console so that they do not split the `TEST` lines. Add `cmdline =
"systemd.log_level=info systemd.log_target=console"` to the test target for
the length of the investigation, and read `boot1.log` in the test's outputs.

Root login is disabled: the nocloud image logs root in on the serial console
without a password, so the getty templates are masked and the test checks
that nothing listens (`console-login-disabled`). No network, no cloud-init.

What it checks (every check of dcfs fails in a guest without the wrapper
installed and in one whose `mount.dcfs` exits 0 and mounts nothing, which the
step's commit message quotes; the guest's own checks, pid 1, util-linux, the
disabled console login and the `nofail` boot, do not depend on the wrapper):

- through `mount(8)`: `mount -t dcfs` reaches the helper; libmount's `rw`,
  `nofail`, `_netdev`, `defaults` are not errors; `mount -o remount,dcfs.ro`
  and `remount,rw` reach the wrapper by the type `fuse.dcfs`
  (`mount.fuse.dcfs`) and toggle only the dcfs mount; a native option in a
  remount is ignored with the wrapper's WARNING on mount's stderr; `umount`
  stops the daemon (the FUSE path: libmount finds no `umount.dcfs` and calls
  `umount(2)`);
- the README's recipes with util-linux: `mount /data` takes its fstab line's
  options, `mount -o remount,dcfs.ro /data` has libmount merge the line's
  options into the helper's (the native `noatime` is ignored with the
  WARNING), `umount -R /data` unmounts a tree, a SIGKILLed daemon leaves a
  dead mount that `umount -l` clears before the instance mounts again,
  `x-systemd.requires-mounts-for=` is accepted and recorded;
- the quiet kernel (`guest/init`, step 26.14) is still in force under
  systemd: nothing in the image changes it (`/usr/lib/sysctl.d` of Debian 13
  has `kernel.sysrq`, `fs.protected_*`, `vm.max_map_count`, `net.ipv4.*`, the
  pid limit and the core pattern, no `vm.dirty_*`, `laptop_mode` or
  `vfs_cache_pressure`), and the test reads the four values back;
- the exit statuses `mount(8)` returns: a usage error 1, a failed start 32
  (cache database locked), a native failure its own (`mount -t bogusfs`, a
  missing device), each with dcfs's one `E...` ERROR line on mount's stderr
  and nothing mounted;
- fstab: a native ext4 by `UUID=` (type autodetected, `noatime` passed to the
  native mount), an xfs (`dcfs.fstype=xfs`) and a btrfs (autodetected) by
  `UUID=` (the kernel mounts them; the image has no xfsprogs or btrfs-progs,
  `run-qemu.sh` made the filesystems), a nested bind (`dcfs.ro`), the `none`
  form with `_netdev` and a `nofail` mount of a device that is not there,
  mounted by `mount -a` and then by the units systemd generates from them
  (`systemctl start`, `status`, `findmnt`), with one daemon and one cache
  database each (not hosted: `dcfs.fstype=nfs`, the image has no NFS client
  and the guest no network; `nfs_test` covers NFS on the other side); a child
  requires its parent; stopping the child leaves the parent and stopping the
  parent stops the child; a start that fails marks the unit failed with
  dcfs's message in the journal;
- the journal: the daemon's syslog lines are attributed to its mount unit
  (`journalctl -u data.mount`, `_SYSTEMD_UNIT=`) at the threshold the
  options set (`dcfs.stderrthreshold=0` shows the INFO narrative; the
  default hides it);
- after the reboot: the fstab mounts were made by systemd at boot, the
  parent before the child; every instance recovered and none with dirty
  entries (that every daemon shut down cleanly is
  `DISABLED_reboot-every-daemon-shuts-down-cleanly`: in about half the runs
  one daemon, killed by systemd's last SIGTERM while it was still finishing,
  starts "uncleanly" after an ordinary reboot); a warm tree reads nothing from
  the backing disk (`sectors_read`); a `nofail` mount of a missing device did
  not hold the boot (the script began while that mount's start job was still
  waiting, with its device timeout set to ten minutes).

What the real `mount(8)` path showed that the busybox guest could not:

- libmount resolves a `UUID=`/`LABEL=` source to the device path before it
  calls the helper (`mount.dcfs /dev/vdb /data -o ...`, also from systemd's
  mount unit), so the FUSE mount's source, and `findmnt`'s, is `/dev/vdb`,
  not the spec as written in fstab (plan decision 11 holds for sources
  libmount does not resolve: `/srv/raw`, `nas:/export`). The test pins what
  happens; the wrapper cannot recover the tag.
- the daemon lives in its mount unit's cgroup and its syslog lines carry the
  unit, with no systemd-specific code, as designed. systemd calls the unit
  stopped as soon as the mount is gone, while the daemon is still syncing the
  backing filesystem and closing the cache database for up to two seconds
  ("Unit process N (mount.dcfs) remains running after unit stopped"), so
  **`systemctl restart` of a dcfs mount races the old daemon**: the new one
  finds `Cache database ... is in use by another dcfs process` (exit 32) in
  most restarts, and for a mount that `local-fs.target` requires (every fstab
  line without `nofail`) the failed start sends the machine to
  `emergency.target`. The test keeps the check as `DISABLED_systemd-restart-
  parent-restarts-child` (`would FAIL` in the log, the way `lib.sh`'s
  `disabled` keeps a kernel bug) on its own pair of `nofail,noauto` units and
  waits for the old daemon to exit before the stop/start checks. The same
  race hits `umount` followed at once by `mount` of the instance without
  systemd (`DISABLED_umount-then-mount-at-once`); with the README's wait
  (`flock <cache database> true` returns when the daemon has let go) it
  works (`umount-then-mount-restarts-an-instance`). A fix is the new daemon
  waiting for the cache lock, or a `umount.dcfs` that returns when the daemon
  has exited (plan step 15.6b).
- `mount -t nosuchfs` is `-t no` + `suchfs` to util-linux (the `no` prefix
  negates a type list); the test uses `bogusfs`.

Cost (2026-10-09, KVM, a 4-core host shared with other work): a 388 MiB image
download once; the whole test about 60 s, systemd taking 7 to 11 s to reach
the script on each boot; the default 256 MiB guest (`peak_used` 61 MiB,
`reclaim_scans=0`, dcfs 7 MiB per daemon); tier large, timeout long (the two
boots share it: 420 s each). Run it with
`bazel test //test/qemu:mount_dcfs_systemd_test`; `--test_output=all` prints
both boots. Coverage is not collected from this guest (the daemons are not
started from `guest/init`'s environment).


## NFS test and the Debian rootfs

`nfs_test` (above) needs `rpc.nfsd`/`rpc.mountd`/`exportfs`/`mount.nfs4`,
none of which fit in the busybox-only initramfs every other test runs
from. It gets them by chrooting into a small Debian tree instead.

### Building the image

Phase 4, part c (`docs/plan/phases/04-pinned-host-tools.md`): the image is
built by Bazel, from packages `rules_distroless` resolves and fetches from
a pinned `snapshot.debian.org` timestamp -- not a live mirror, and no
longer a step run once by hand into `~/.cache/dcfs`. See
`third_party/debian/README.md` for the package list (with each package's
reason), the pin and its update procedure, and exactly how the image is
assembled (`mke2fs -d` against a flattened package tree, using the
Alpine's `@alpine_fstools//:mke2fs` since Phase 24, not
a host tool -- no root, no network, no loop mounts in the Bazel action
itself).

```
bazel build //third_party/debian:rootfs
```

produces `bazel-bin/third_party/debian/rootfs-debian.ext4` (768 MiB
nominal, ~245 MiB actual content -- `nfs_test`'s `rootfs =
"//third_party/debian:rootfs"` attribute depends on this target directly;
`run-qemu.sh` still copies it into each test's own `$TEST_TMPDIR`, same as
before, so the guest can write to it freely without disturbing the cached
original).

### Boot mode: chrooting instead of switching root

The initramfs boot stays the fast path for every other test. `qemu_test`
takes an optional `rootfs = "//third_party/debian:rootfs"` attribute
(`test/qemu/qemu_test.bzl`): when given, `run-qemu.sh` copies the built
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

## Benchmarks

Phase 10. `//bench:dcfs_bench` (google/benchmark, BCR `google_benchmark`)
is installed in the e2e initramfs as `/bin/dcfs_bench` and run by
`guest/bench.sh`; it starts dcfs itself. Its cases (names are
`Case/target`):

- `Stat`, `OpenClose`, `SmallRead` (a 100-byte read), `Lookup` (an
  eight-component path walk), `Readdir` (10000 entries), each on `backing`
  (the backing filesystem directly), `dcfs` (default one-hour kernel
  timeouts) and `dcfs0` (`--attr_timeout_sec=0 --entry_timeout_sec=0`:
  every operation reaches dcfs and its database); and the same three over
  a **slow backing**: a filesystem on `vdc` seen through a device-mapper
  `delay` target (`SLOW_MS`, 5 ms per read and write), as `slow_backing`,
  `slow_dcfs`, `slow_dcfs0`. dcfs's database is warm for every `dcfs*`
  target (a `find` before the run). `Stat`/`OpenClose`/`SmallRead` cycle
  through the entries so that every operation touches a new inode block
  and drop the kernel's caches (untimed) when they wrap; `Lookup` and
  `Readdir` drop them before every iteration. The direct slow cases pay the
  latency, the dcfs ones must not.
- `Startup`: exec to first `stat` on a warm database (`--entries`).
- `Recovery`: restart after SIGKILL with `--dirty` unsynced creations.
- `Memory`: dcfs's RSS after `find`, after `drop_caches=2`, and bytes per
  object.

`bench_readdir_test` (large tier) runs only the three `Readdir` cases on a
`READDIR_ENTRIES`-entry directory (default 10000), for the readdir
performance work. The default build is fastbuild (unoptimized); to measure
an optimized dcfs, SQLite, Abseil, libfuse and benchmark library without
rebuilding the kernel and QEMU (`-c opt` does, and the QEMU build then
needs network access), optimize just those sources:

    bazel test //test/qemu:bench_readdir_test --test_output=all \
      '--per_file_copt=^(dcfs|bench)/.*,external/(abseil-cpp|sqlite3|libfuse|google_benchmark)\+/.*@-O2,-DNDEBUG'

The helpers `dcfs_bench mktree ROOT N BIG` (makes the tree; used by the
idle and memory tests) and `dcfs_bench dm-delay NAME DEV MS` (device-mapper
ioctls; the guest has no dmsetup) are subcommands of the same binary.

| Target | Tier | What |
|---|---|---|
| `bench_smoke_test` (matrix) | small (ext4), medium | every benchmark, one iteration, tiny trees |
| `bench_full_test` | enormous | 100000-entry trees, real iteration counts; prints the numbers for `docs/plan/log.md` |
| `idle_short_test` (matrix) | medium (ext4), large | pass/fail, 60 s: the backing device's `/proc/diskstats` read and write counts do not move while a warm dcfs serves `statfs`, `stat` and `ls` of cached paths (after the kernel's caches were dropped) |
| `idle_long_test` | large | the same for 600 s |
| `memory_test` (matrix) | medium (ext4), large | pass/fail: RSS after `find` is under 256 bytes per entry, and after a `drop_caches` of half the tree a find over the other half adds nothing; see the comment in `guest/memory.sh` for why "RSS shrinks back" cannot be asserted |
| `cancel_inventory_test` | enormous | Phase 22.1: the wall time of each request path that waits on the backing filesystem, every backing I/O delayed 10 ms (dm-delay); prints the table below |
| `cancel_test` | large | pass/fail (Phase 22): on the same delayed backing, an interrupted (`timeout -s INT 1`) or killed (`kill -9`) listing of an unlisted 20,000-entry directory (a different one each) ends within 2 s (8 s under TCG; measured 0.7-1.0 s under KVM, 0.8-1.2 s under TCG), dcfs serves other requests afterwards, and the listing then equals the backing directory's |

The cancellation inventory (`cancel_inventory_test`, 10 ms per backing I/O,
fastbuild dcfs, a loaded host, 2026-10-07; each one request unless noted).
Run it again after a change to a request path: a path above about 100 ms
needs checkpoints (AGENTS.md, "Requests are cancellable"; docs/design.md,
"Cancellation").

| Path | Time |
|---|---|
| population of a 20,000-entry directory (one READDIR) | 13.6-19.5 s (the backing alone, `ls -l` on it: 3.4 s) |
| a cold lookup three directories deep | 210-310 ms |
| a cached READDIRPLUS, then a stat of each of 20,000 entries | 3.3-4.1 s in all, each request a few ms |
| a cold open and read | 340-460 ms |
| create / rename / unlink | 210 / 50-120 / 30-110 ms |
| 32 MiB written, then an fsync (with its sync point) | 330-450 ms |
| DESTROY and FinishRun with 500 written files (no caller) | 370-440 ms |
| start-up: new cache / clean / after a crash with 500 dirty inodes (no caller) | 120-220 / 140-350 / 270-580 ms |

Results are printed, not pass/fail (VM timing is noisy), except in the idle
and memory tests. The guest kernel has POSIX timers
(`CONFIG_POSIX_TIMERS`), so google/benchmark's CPU time column is real CPU
time. The 1M-entry tree the
phase plan mentions does not fit the 1 GiB guest (the kernel's inode cache
for it alone would not), so `--entries` defaults to 100000; the idle test
has no NFS client (that needs the nfs_test Debian rootfs).

## `run-qemu.sh` internals

One script serves both kinds of test (see the usage comment at the top of
`scripts/run-qemu.sh`): `--qemu <...> --qboot <...> --modules <archive> --unit <vmlinuz>
<initramfs> [disk-spec...]` for `qemu_cc_test`, or `--qemu <...> --qboot
<...> --modules <archive> <vmlinuz> <initramfs> <dcfs_test-name> [disk-spec...]` for
`qemu_test`. `--qemu`/`--qboot` are mandatory in both modes (step 4.4; see
"Firmware: qboot" above). Each `disk-spec` is `<device>:<fstype>:<size>[:<mke2fs options>]`,
e.g. `vdb:ext4:256M` (options for ext4 only); disks are attached in `<letter>` order, with a small
unformatted filler drive for any skipped letter, so the guest kernel
enumerates the requested disk at exactly `/dev/vd<letter>`.
