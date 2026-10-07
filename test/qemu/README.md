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
-m <mem> -smp 1              # unit tests (-smp 2 for e2e tests); <mem> is the
                             # test's allowance, see "Guest memory" below
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
| large / enormous | `bazel test //...` (everything; CI) | nfs_test (large), pjdfstest on three filesystems (enormous; about 10 minutes alone, about 18 when two run side by side) | about 36 minutes |

`size` also sets Bazel's resource estimate (small assumes about 20 MB), so
each QEMU test declares its real needs with tags: `cpu:2` (e2e, `-smp 2`) or
`cpu:1` (unit, `-smp 1`) and `resources:memory:<guest allowance + 100>`
(see "Guest memory"). Bazel then schedules only as many guests as fit in the
machine. Timeouts are explicit (`short` unit, `moderate` e2e, `long` nfs,
`eternal` pjdfstest).

New tests: pick the tier from the measured duration (read it from
`bazel-testlogs/**/test.xml`).

## Guest memory

Every guest prints one line just before its verdict, from a sampler that
`guest/init` runs for the whole test (every 0.1 s, one `awk` process):

```
MEM total=1018448 min_avail=964728 peak_used=54452 peak_cached=... peak_anon=... peak_shmem=... peak_slab=... dcfs_hwm=... top=<name>:<pid>:<VmHWM> samples=N up=<s>
```

(all KiB except `samples` and `up`, seconds of guest uptime at the last sample). `peak_used` is the highest `MemTotal - MemAvailable`: what could
not be reclaimed. `peak_shmem` is tmpfs (`/tmp`, and the initramfs itself:
about 22 MiB plain, 84 MiB under ASan, whose runtime libraries are copied
in). `dcfs_hwm` is the kernel's own `VmHWM` of the daemon. The same
`run-qemu.sh` that reads the line prints a summary of it, warns when a guest
came within 10% of running out (or 40% of `MemTotal` in tmpfs, which is
capped at half of it), and refuses to pass a run that lacks the line.
`grep '^MEM ' bazel-testlogs/test/qemu/<test>/test.log` for any test.

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
| unit tests (`qemu_cc_test`) | 22-35 | 52-268 (`dir_cache_fs_test` 268, `metadata_cache_test` 224, `backing_test` 187, the rest under 125) | 192 | 384 (`dir_cache_fs_test` 448) |
| boot, cache_permissions, lifecycle, atime, removed, copy, boundary, credentials, create, crash, handles, power, readonly, rename, setattr, release_leak, nfs, passthrough (60-102 plain) | 52-102 | 145-190 (nfs 169) | 256 | 384 |
| names, names_random, readdir_boundary, idle_short, idle_long, pjdfstest (3 shards) | 57-75 | 433-570 | 256 | 832 |
| write | 136-153 | 224-248 | 320 | 448 |
| memory | 197-256 | 279-376 | 448 | 576 |
| bench_smoke | 79-87 | 1169-1178 | 256 | 1856 |
| destroy | 501 | not measured | 832 | 1856 (a guess, from bench_smoke) |
| bench_readdir | 157 | 1432-1509 | 320 | 2304 |
| bench_full | 569 | over 2001 (killed by the OOM killer at 2048; not measured further) | 896 | 3072 (a guess) |

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
  kernel log to the end of the serial log as `KERNEL-OOPS: ...` lines;
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
`presubmit`, `full`; the top-level README's "Continuous integration"
section has the whole story) with the same scripts and the same timeouts as
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
| banned symbols in `//dcfs:main_static` (26.8) | `//tools:banned_symbols_self_check_test` (the real checker and deny list over a program that calls `realpath`) |
| shipped dependency golden (26.9) | `//tools:shipped_deps_self_check_test` (the real comparison over a golden with a line removed) |
| repository shape: third_party READMEs, guest scripts used, DISABLED_ checks listed (26.13) | `//tools:repo_shape_self_check_test` (fixture trees with a missing README, an unreferenced script, an unlisted check) |
| commit subjects on the CI push range (26.13) | `//tools:commit_subjects_test` (canned subject lists through the real `.github/ci/commit_subjects.sh`) |
| `check_cold` / `quiesce_daemon` (guest helper, not a gate of its own) | a helper whose gate, `quiesce_daemon`'s wait, is exercised by `//test/qemu:release_leak_test` and the `written-forgotten` check of idle (`guest/idle.sh`): both fail if the daemon is not quiesced |

A gate without a self-check is a review finding: the review checklist asks
whether every gate in the tree has a self-check, and if not, why not.

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

**Step 6.2: pjdfstest shards.** `pjdfstest_test_<fstype>` is a `test_suite`
over three guests, `pjdfstest_{rename,chown,rest}_test_<fstype>`, each a
three-line wrapper (`guest/pjdfstest_<shard>.sh`) that sets `PJD_SHARD` and
sources `guest/pjdfstest.sh`. The assignment is by test directory and fixed
in `shard_of()` there (`rename/`; `chown/` and `chmod/`; every other
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
  CLI -- usage/flag validation, a missing or non-directory `--source`, a
  cache database refused because it belongs to a different filesystem,
  `--fuse_opt` (good and bad options), a clean SIGTERM shutdown (exit 0,
  unmounted, WAL checkpointed), mounting dcfs back over its own `--source`,
  and restarting against a previously-used cache database.
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
