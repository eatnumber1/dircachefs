#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Boot the dcfs QEMU guest and report the verdict. --qemu and --qboot are
# mandatory in both modes (step 4.4): the Bazel-built
# @alpine_qemu//:qemu_system_x86_64 and @alpine_qemu//:root/usr/share/qemu/qboot.rom
# targets, passed as $(location ...) by qemu_test.bzl/qemu_cc_test.bzl --
# never a host PATH lookup or a default path. So are the mkfs tools for the
# scratch disks (R3): --mke2fs, --mke2fs-conf (MKE2FS_CONFIG), --mkfs-xfs and
# --mkfs-btrfs, in front of the other flags. The kernel is Alpine's
# linux-virt, whose drivers are modules: --modules <cpio.gz> (step 24.2), the
# archive mkmodules.py builds for the test, is appended to the shared
# initramfs, and guest/init loads what it lists. Two modes:
#
#   run-qemu.sh --unit --qemu <qemu-system-x86_64> --qboot <qboot.rom> \
#       <bzImage> <initramfs.cpio.gz> [disk-spec...]
#       For qemu_cc_test (test/qemu/qemu_cc_test.bzl): boots the guest,
#       which runs /test/run (guest/init's unit-test branch) and prints
#       DCFS-TEST-EXIT=<rc>. Exit 0 iff that line says rc=0.
#
#   run-qemu.sh --qemu <qemu-system-x86_64> --qboot <qboot.rom> \
#       <bzImage> <initramfs.cpio.gz> <dcfs_test-basename> [disk-spec...]
#       For qemu_test (test/qemu/qemu_test.bzl): boots the guest, which
#       runs /tests/<dcfs_test-basename> (guest/init's e2e branch) and
#       prints ALL-TESTS-PASSED or TEST-FAILED. Exit 0 iff the former.
#       --expect-kernel-failure <dcfs_test-basename> (step 23.7; e2e only,
#       and the name must be the test's own): the one guest whose job is to
#       reproduce a kernel bug (casefold_tune_oops_test). See "A kernel
#       failure" below.
#
# Each disk-spec is <device>:<fstype>:<size>[:<mkfs options>], e.g.
# vdb:ext4:256M, where <device> is a /dev/vd<letter> name. The optional
# fourth field (ext4 only; step 23.7) is words handed to mke2fs after
# `-t ext4`, e.g. "vdb:ext4:256M:-O casefold -E encoding=utf8" for a
# casefold-capable filesystem (no colons in the options). Disks are attached to QEMU in
# <letter> order (vda, vdb, ...); any skipped letter gets a small
# unformatted filler drive, so the guest kernel enumerates the requested
# disk at exactly /dev/vd<letter>.
#
# Fast boot (step 5.1b): QEMU's microvm machine, direct kernel boot, no
# PC/ISA legacy devices we don't need (PIT/PIC/option ROMs), virtio-mmio
# disks (microvm has no PCI). Firmware is qboot (QBOOT below): qboot uses
# the kernel's PVH entry point directly when the kernel supports it (see
# the test kernel's CONFIG_PVH=y, which Alpine's has) for an effectively firmware-less boot,
# and falls back to the normal Linux/x86 real-mode boot protocol otherwise
# -- so this one firmware choice covers both cases with no detection logic
# needed here. (bios-microvm.bin, this host's other microvm firmware
# option, was tried and does NOT work with -x-option-roms=off: it relies
# on an option ROM for the -kernel/-initrd hand-off, so with option ROMs
# disabled it falls through to its BIOS boot-device probing and reports
# "No bootable device". qboot has no such dependency.)
#
# rtc=on, NOT rtc=off: measured while building this, rtc=off adds
# something like 5 SECONDS to boot, not saves time. Without a CMOS RTC
# device, arch/x86/kernel/rtc.c's boot-time wall-clock read
# (mc146818_get_cmos_time, called from read_persistent_clock64()
# regardless of any Kconfig -- it is not gated by CONFIG_RTC_CLASS or
# CONFIG_RTC_DRV_CMOS) spins reading port 0x70/0x71 and finds nothing
# there, burning several seconds in retry loops (observed twice per boot:
# once from the early wall-clock read, once from the rtc_cmos platform
# driver's own later, separate probe). rtc=on costs nothing (there's no
# guest driver polling it) and skips both stalls entirely.
#
# acpi=off, deliberately, even though it costs nothing to leave QEMU's
# default (acpi=auto, which resolves to on): microvm advertises
# virtio-mmio devices to the guest one of two ways -- an ACPI DSDT
# device (when ACPI is on) or a `virtio_mmio.device=` kernel command-line
# parameter added automatically to the -append string (when ACPI is off;
# this is what CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES parses; the option list in
# third_party/linux/required_options.txt, checked against Alpine's linux-virt
# by //third_party/linux:kernel_config_test, has it). The first test kernel
# here had no ACPI at all and so never found its disks with acpi=on/auto,
# confirmed experimentally (verified with `info qtree` over the QEMU monitor:
# the virtio-blk-device is correctly attached to a virtio-mmio transport
# either way, but without acpi=off the guest has no way to learn the
# transport's MMIO address and /dev/vd* never appears). Alpine's kernel does
# have ACPI; acpi=off stays because it is what the boot was measured with and
# leaves nothing to enumerate.
set -eu

# Step 4.4: the QEMU binary and qboot ROM are Bazel-built targets
# (@alpine_qemu//:qemu_system_x86_64, @alpine_qemu//:root/usr/share/qemu/qboot.rom) passed
# in explicitly as --qemu/--qboot by qemu_test.bzl/qemu_cc_test.bzl -- no
# host PATH lookup, no DCFS_QBOOT-style override, no default. See
# test/qemu/README.md.
QEMU_BIN=""
QBOOT=""

# R3 (L5): the scratch disks are formatted by the pinned, Bazel-built mkfs
# tools (//third_party/{e2fsprogs,xfsprogs,btrfs-progs}) passed in explicitly,
# like QEMU: no host mkfs.*, no host /etc/mke2fs.conf (MKE2FS_CONFIG is the
# checked-in //third_party/e2fsprogs:mke2fs.conf).
MKE2FS_BIN=""
MKE2FS_CONF=""
MKFS_XFS_BIN=""
MKFS_BTRFS_BIN=""

UNIT=0
ROOTFS=""
# --mem <MiB>: guest RAM, overriding the default below. qemu_test and
# qemu_cc_test always pass it (the per-test allowance, larger for the
# sanitizer builds; see test/qemu/qemu_test.bzl). $DCFS_MEM (e.g. `bazel test
# --test_env=DCFS_MEM=2048`) overrides both, to measure a test's real peak
# with room to spare or to see how a too-small guest fails.
MEM_OVERRIDE=""
# --modules <cpio.gz>: the kernel modules this test declared (step 24.2),
# appended to the initramfs given below (the kernel unpacks concatenated
# archives into one).
MODULES=""
# --expect-kernel-failure <script>: see the kernel-failure check below.
EXPECT_KERNEL_FAILURE=""
# Step 7.2, coverage (only under `bazel coverage`, which sets COVERAGE_DIR;
# test/qemu/coverage.bzl passes these only then): the LLVM tools and the
# instrumented binaries (--cov-object, repeatable). The guest then writes the
# .profraw files its processes wrote to an extra disk (guest/init,
# dump_profraw), and after the run they become COVERAGE_DIR/<name>.dat
# (cov-lcov.sh), which Bazel's lcov merger picks up. A guest that dies
# (power-cut tests, SIGKILLed daemons) writes no profile: that is not a
# failure, just no coverage from that process.
COV_SCRIPT=""
COV_PROFDATA=""
COV_LLVM_COV=""
COV_OBJECTS=""
while :; do
	case "${1:-}" in
	--unit)
		UNIT=1
		shift
		;;
	--rootfs)
		ROOTFS=$2
		shift 2
		;;
	--mem)
		MEM_OVERRIDE=$2
		shift 2
		;;
	--modules)
		MODULES=$2
		shift 2
		;;
	--expect-kernel-failure)
		EXPECT_KERNEL_FAILURE=$2
		shift 2
		;;
	--cov-script)
		COV_SCRIPT=$2
		shift 2
		;;
	--cov-llvm-profdata)
		COV_PROFDATA=$2
		shift 2
		;;
	--cov-llvm-cov)
		COV_LLVM_COV=$2
		shift 2
		;;
	--cov-object)
		COV_OBJECTS="$COV_OBJECTS $2"
		shift 2
		;;
	--qemu)
		QEMU_BIN=$2
		shift 2
		;;
	--qboot)
		QBOOT=$2
		shift 2
		;;
	--mke2fs)
		MKE2FS_BIN=$2
		shift 2
		;;
	--mke2fs-conf)
		MKE2FS_CONF=$2
		shift 2
		;;
	--mkfs-xfs)
		MKFS_XFS_BIN=$2
		shift 2
		;;
	--mkfs-btrfs)
		MKFS_BTRFS_BIN=$2
		shift 2
		;;
	*)
		break
		;;
	esac
done

if [ -z "$QEMU_BIN" ] || [ -z "$QBOOT" ]; then
	echo "run-qemu.sh: --qemu <qemu-system-x86_64> and --qboot <qboot.rom> are required" >&2
	echo "(they must be the @alpine_qemu targets, not a host lookup)" >&2
	exit 1
fi

if [ -z "$MKE2FS_BIN" ] || [ -z "$MKE2FS_CONF" ] || [ -z "$MKFS_XFS_BIN" ] ||
	[ -z "$MKFS_BTRFS_BIN" ]; then
	echo "run-qemu.sh: --mke2fs, --mke2fs-conf, --mkfs-xfs and --mkfs-btrfs are required" >&2
	echo "(the @alpine_fstools wrapper targets, not a host lookup)" >&2
	exit 1
fi

for tool in "$MKE2FS_BIN" "$MKE2FS_CONF" "$MKFS_XFS_BIN" "$MKFS_BTRFS_BIN"; do
	case "$tool" in
	/usr/* | /bin/* | /sbin/*)
		echo "run-qemu.sh: ERROR: '$tool' looks like a host path," \
			"not a Bazel-built //third_party target" >&2
		exit 1
		;;
	esac
done

KERNEL=$1
INITRD=$2
shift 2
if [ "$UNIT" -eq 0 ]; then
	DCFS_TEST=$1
	shift
fi

# The opt-in is deliberate: it names the guest script, which must be this
# test's own, and it exists only for an e2e test.
if [ -n "$EXPECT_KERNEL_FAILURE" ] &&
	{ [ "$UNIT" -eq 1 ] || [ "$EXPECT_KERNEL_FAILURE" != "$DCFS_TEST" ]; }; then
	echo "run-qemu.sh: --expect-kernel-failure must name this test's guest script" \
		"(an e2e test; got '$EXPECT_KERNEL_FAILURE', test '${DCFS_TEST:-<unit>}')" >&2
	exit 1
fi

WORKDIR="${TEST_TMPDIR:-$(mktemp -d)}"
LOG="${TEST_UNDECLARED_OUTPUTS_DIR:-$WORKDIR}/serial.log"

if [ -n "$MODULES" ]; then
	cat "$INITRD" "$MODULES" >"$WORKDIR/initramfs-with-modules.cpio.gz"
	INITRD="$WORKDIR/initramfs-with-modules.cpio.gz"
fi

# Record the mkfs tools, for anyone auditing a serial log.
{
	echo "run-qemu.sh: mke2fs: $MKE2FS_BIN"
	echo "run-qemu.sh: mke2fs.conf: $MKE2FS_CONF"
	echo "run-qemu.sh: mkfs.xfs: $MKFS_XFS_BIN"
	echo "run-qemu.sh: mkfs.btrfs: $MKFS_BTRFS_BIN"
} >>"$LOG"

# --- lay out the disks in letter order, one -drive/-device pair per index -
specs_file="$WORKDIR/disk-specs"
: >"$specs_file"
for spec in "$@"; do
	dev=${spec%%:*}
	rest=${spec#*:}
	fstype=${rest%%:*}
	size=${rest#*:}
	opts=
	case "$size" in
	*:*)
		opts=${size#*:}
		size=${size%%:*}
		;;
	esac
	if [ -n "$opts" ] && [ "$fstype" != ext4 ]; then
		echo "run-qemu.sh: mkfs options are only supported for ext4 ('$spec')" >&2
		exit 1
	fi
	letter=${dev#vd}
	index=$(($(printf '%d' "'$letter") - $(printf '%d' "'a")))
	echo "$index:$dev:$fstype:$size:$opts" >>"$specs_file"
done

max_index=-1
if [ -s "$specs_file" ]; then
	max_index=$(cut -d: -f1 "$specs_file" | sort -n | tail -1)
fi

drive_args=""
idx=0
while [ "$idx" -le "$max_index" ]; do
	line=$(grep "^$idx:" "$specs_file" || true)
	if [ -n "$line" ]; then
		dev=$(echo "$line" | cut -d: -f2)
		fstype=$(echo "$line" | cut -d: -f3)
		size=$(echo "$line" | cut -d: -f4)
		opts=$(echo "$line" | cut -d: -f5-)
		img="$WORKDIR/$dev.img"
		truncate -s "$size" "$img"
		case "$fstype" in
		# shellcheck disable=SC2086 # opts is a deliberate list of words
		ext4) MKE2FS_CONFIG="$MKE2FS_CONF" "$MKE2FS_BIN" -q -F -t ext4 $opts "$img" ;;
		btrfs) "$MKFS_BTRFS_BIN" -q -f "$img" ;;
		xfs) "$MKFS_XFS_BIN" -q -f "$img" ;;
		*)
			echo "run-qemu.sh: unknown fstype '$fstype'" >&2
			exit 1
			;;
		esac
	else
		dev="filler$idx"
		img="$WORKDIR/filler-$idx.img"
		truncate -s 1M "$img"
	fi
	drive_args="$drive_args -drive id=$dev,file=$img,format=raw,if=none -device virtio-blk-device,drive=$dev"
	idx=$((idx + 1))
done

# --- optional Debian rootfs disk, always the next letter after the last
# requested disk-spec (so e.g. disks vdb,vdc + a rootfs gives vdd) --------
rootfs_append=""
if [ -n "$ROOTFS" ]; then
	rootfs_index=$((max_index + 1))
	rootfs_letter=$(awk -v i="$rootfs_index" 'BEGIN{printf "%c", 97+i}')
	rootfs_dev="vd$rootfs_letter"
	rootfs_img="$WORKDIR/rootfs.img"
	# Copy rather than attach the source image directly: the guest chroots
	# into and writes through this filesystem, and the source (now a Bazel
	# output, //third_party/debian:rootfs -- see test/qemu/BUILD.bazel's
	# nfs_test) must stay pristine for the next test run. A ~1 GiB copy is
	# cheap next to the rest of this test. Bazel marks its own outputs
	# read-only (0555) on purpose; `cp` preserves that onto $rootfs_img, so
	# it needs an explicit +w or QEMU's -drive (no readonly=on) fails with
	# "Permission denied" opening it.
	cp "$ROOTFS" "$rootfs_img"
	chmod u+w "$rootfs_img"
	drive_args="$drive_args -drive id=$rootfs_dev,file=$rootfs_img,format=raw,if=none -device virtio-blk-device,drive=$rootfs_dev"
	rootfs_append=" dcfs_rootfs=/dev/$rootfs_dev"
fi

# --- optional coverage disk (step 7.2): the next letter after the last disk
# and the rootfs; a raw file the guest writes a tar of its profiles to
# (guest/init, dump_profraw: the serial console moves about 8 KB/s, too slow
# for profiles of a few MB) -------------------------------------------------
COVDISK_IMG=""
cov_append=""
if [ -n "${COVERAGE_DIR:-}" ] && [ -n "$COV_OBJECTS" ] &&
	[ -n "$COV_PROFDATA" ] && [ -n "$COV_LLVM_COV" ] && [ -n "$COV_SCRIPT" ]; then
	cov_index=$((max_index + 1))
	if [ -n "$ROOTFS" ]; then
		cov_index=$((cov_index + 1))
	fi
	cov_letter=$(awk -v i="$cov_index" 'BEGIN{printf "%c", 97+i}')
	COVDISK_IMG="$WORKDIR/coverage.img"
	truncate -s 256M "$COVDISK_IMG"
	drive_args="$drive_args -drive id=covdisk,file=$COVDISK_IMG,format=raw,if=none -device virtio-blk-device,drive=covdisk"
	cov_append=" dcfs_cov=/dev/vd$cov_letter"
fi

# --- boot ------------------------------------------------------------
# KVM only when /dev/kvm is usable by this user; otherwise plain TCG
# (2-12x slower, see the timeouts below).
# DCFS_FORCE_TCG=1 (e.g. bazel test --test_env=DCFS_FORCE_TCG=1) forces TCG
# even where /dev/kvm is usable, to test the fallback.
if [ -w /dev/kvm ] && [ -z "${DCFS_FORCE_TCG:-}" ]; then
	ACCEL=kvm
	CPU=host
	# KVM guests get the TSC and LAPIC timer frequencies from kvmclock/CPUID
	# and need no legacy timer hardware.
	LEGACY_TIMERS=off
else
	echo "run-qemu.sh: using tcg (no usable /dev/kvm, or DCFS_FORCE_TCG set)" | tee -a "$LOG"
	ACCEL=tcg
	CPU=max
	# Step 5.1: under TCG the guest has no kvmclock/CPUID frequency
	# information, so with pit=off,pic=off (the KVM setting) it can find no
	# reference to calibrate the TSC against ("tsc: Unable to calibrate
	# against PIT / No reference (HPET/PMTIMER) available"), then waits
	# forever for a timer tick that never comes while it calibrates the
	# LAPIC timer: the console goes silent right after "Marking TSC
	# unstable" and the VM spins at 100% CPU until the timeout (measured:
	# boot_test never finishes in 900 s). pit=on AND pic=on gives it the
	# 8254 and 8259 it expects; pit=on alone is not enough (the PIT's IRQ0
	# is wired through the PIC), nor is pic=on alone (kernel panic, "IO-APIC
	# + timer doesn't work"). Boot then takes about 5 s instead of never.
	LEGACY_TIMERS=on
fi

# Default QEMU timeouts, from measurements (step 5.1; Bazel's own test
# timeout, which these sit under, is the outer limit). Slowest of each kind,
# KVM / TCG, in seconds:
#   unit tests: 3.8 / 14 (backing_test)           -> 60 / 300
#   e2e:        215 / 257 (nfs_test; pjdfstest 611 / 3502 on a host at load
#               15-25 shared with other Bazel runs, qemu's own CPU time 2660)
#                                                 -> 1800 / 7200
# Both are 3-20x the measurement: the machines these run on are shared.
# Bazel's TEST_TIMEOUT (seconds, from the target's `timeout`) is the real
# limit, though, and a fixed default under it killed destroy_test on a loaded
# host (1800 s of a 3600 s eternal): the guest gets TEST_TIMEOUT less 60 s
# for the teardown and the log collection (half of it, if that is under
# 60 s), and the default above only when Bazel did not set it (a manual
# run). An explicit TIMEOUT beats both.
if [ "$ACCEL" = kvm ]; then
	UNIT_TIMEOUT=60
	E2E_TIMEOUT=1800
else
	UNIT_TIMEOUT=300
	E2E_TIMEOUT=7200
fi
case "${TEST_TIMEOUT:-}" in
'' | *[!0-9]*) ;;
*)
	E2E_TIMEOUT=$((TEST_TIMEOUT > 120 ? TEST_TIMEOUT - 60 : TEST_TIMEOUT / 2))
	;;
esac
if [ "$UNIT" -eq 1 ]; then
	TIMEOUT_SECS="${TIMEOUT:-$UNIT_TIMEOUT}"
else
	TIMEOUT_SECS="${TIMEOUT:-$E2E_TIMEOUT}"
fi
echo "run-qemu.sh: guest timeout $TIMEOUT_SECS s (TEST_TIMEOUT ${TEST_TIMEOUT:-unset}, TIMEOUT ${TIMEOUT:-unset})"

# Defaults, the smallest class's allowance (step 6.2: measured peak
# MemTotal-MemAvailable of 35 MiB for the unit tests and 102 MiB for the
# lightest e2e tests, plain build; see test/qemu/README.md "Guest memory").
if [ "$UNIT" -eq 1 ]; then
	MEM="${DCFS_MEM:-${MEM_OVERRIDE:-192}}"
	SMP=1
else
	MEM="${DCFS_MEM:-${MEM_OVERRIDE:-256}}"
	SMP=2
fi

append="console=ttyS0 reboot=t panic=-1 loglevel=3 rdinit=/init dcfs_accel=$ACCEL"
if [ "$UNIT" -eq 0 ]; then
	append="$append dcfs_test=$DCFS_TEST"
fi
append="$append$rootfs_append"
COVERAGE=0
if [ -n "$COVDISK_IMG" ]; then
	COVERAGE=1
	append="$append$cov_append"
fi

# Record exactly which binaries this run used, for anyone auditing a
# serial log (and for the harness check below): a Bazel-fetched path is
# a wrapper under an Alpine repository's directory (".../wrappers/..."),
# never "/usr/...".
echo "run-qemu.sh: qemu binary: $QEMU_BIN ($("$QEMU_BIN" --version 2>&1 | head -1))" >>"$LOG"
echo "run-qemu.sh: qboot rom: $QBOOT" >>"$LOG"
case "$QEMU_BIN" in
/usr/*| /bin/*)
	echo "run-qemu.sh: ERROR: qemu binary '$QEMU_BIN' looks like a host path," \
		"not a Bazel-built target" >&2
	exit 1
	;;
esac
case "$QBOOT" in
/usr/*| /bin/*)
	echo "run-qemu.sh: ERROR: qboot rom '$QBOOT' looks like a host path," \
		"not the Bazel-built @alpine_qemu//:root/usr/share/qemu/qboot.rom target" >&2
	exit 1
	;;
esac

start=$(date +%s.%N)
echo "run-qemu.sh: qemu start $start" >>"$LOG"
# shellcheck disable=SC2086 # drive_args is a deliberately unquoted list of flags
timeout "$TIMEOUT_SECS" "$QEMU_BIN" \
	-M microvm,x-option-roms=off,pit=$LEGACY_TIMERS,pic=$LEGACY_TIMERS,rtc=on,isa-serial=on,acpi=off \
	-bios "$QBOOT" \
	-nodefaults -no-user-config -nographic -no-reboot \
	-serial stdio \
	-accel "$ACCEL" -cpu "$CPU" \
	-m "$MEM" -smp "$SMP" \
	-kernel "$KERNEL" \
	-initrd "$INITRD" \
	-append "$append" \
	$drive_args \
	2>&1 | tee -a "$LOG" || true
end=$(date +%s.%N)
echo "run-qemu.sh: qemu end $end" >>"$LOG"

# Step 7.2: the profiles the guest wrote to the coverage disk (a tar, see
# guest/init's dump_profraw) become this test's lcov file in COVERAGE_DIR.
if [ "$COVERAGE" -eq 1 ]; then
	rawdir="$WORKDIR/profraw"
	mkdir -p "$rawdir"
	tar -x -C "$rawdir" -f "$COVDISK_IMG" 2>/dev/null ||
		echo "run-qemu.sh: no profiles on the coverage disk (no instrumented process exited normally)"
	rm -f "$COVDISK_IMG"
	# shellcheck disable=SC2086 # COV_OBJECTS is a list of paths
	"$COV_SCRIPT" "$COV_PROFDATA" "$COV_LLVM_COV" "$rawdir" \
		"$COVERAGE_DIR/qemu-$(printf '%s' "${TEST_TARGET:-test}" | tr -c 'A-Za-z0-9_.-' _).dat" $COV_OBJECTS || {
		echo "run-qemu.sh: ERROR: the profiles could not be turned into lcov; the test fails rather than report less coverage" >&2
		exit 1
	}
	# The test's own lcov, kept beside the serial log for inspection.
	cp "$COVERAGE_DIR"/qemu-*.dat "${TEST_UNDECLARED_OUTPUTS_DIR:-$WORKDIR}/" 2>/dev/null || true
fi

echo
# Step 6.2: a guest that ran out of memory says so, whatever else failed: the
# OOM killer's lines (guest/init's mem_report prints them as MEM-OOM:) or a
# panic from having nothing left to kill. The fix is a bigger `mem=` on the
# qemu_test (test/qemu/qemu_test.bzl), not a debugging session on whichever
# check happened to lose its process.
if grep -q -E "^MEM-OOM:|System is deadlocked on memory|Out of memory and no killable" "$LOG"; then
	echo "run-qemu.sh: ERROR: the guest ran out of memory (-m $MEM MiB); the lines:" >&2
	grep -E "^MEM-OOM:|System is deadlocked on memory|Out of memory and no killable" "$LOG" | head -n 10 >&2
	echo "run-qemu.sh: raise this test's mem= (test/qemu/qemu_test.bzl, README.md)" >&2
	echo "== RESULT: FAIL (guest out of memory; see $LOG) =="
	exit 1
fi
# Step 6.2: guest/init prints one `MEM ...` line (the guest memory sampler's
# extremes) before the verdict. Without it the guest never got that far, or
# the sampler or the init changes it rides on are broken, so it cannot pass:
# the memory allowances (`mem=` in test/qemu/*.bzl) are only as good as that
# line. A guest whose RAM is too small for its own initramfs dies in the
# kernel, before init, with one of these lines.
# (A truncated unpack can still leave an init that runs, so that line fails a
# run whatever else the log holds.)
BOOT_DIED="Initramfs unpacking failed|Unable to mount root fs|Kernel panic"
if grep -q "Initramfs unpacking failed" "$LOG" || ! grep -q "^MEM total=" "$LOG"; then
	if grep -q -E "$BOOT_DIED" "$LOG"; then
		echo "run-qemu.sh: ERROR: the guest died while booting; the lines:" >&2
		grep -E "$BOOT_DIED" "$LOG" | head -n 5 >&2
		echo "run-qemu.sh: -m $MEM MiB is probably too little for the initramfs (the unpacked" \
			"archive lives in RAM); raise this test's mem= (test/qemu/README.md)" >&2
	else
		echo "run-qemu.sh: no MEM line in the serial log (guest/init's memory sampler)" >&2
	fi
	echo "== RESULT: FAIL (see $LOG) =="
	exit 1
fi
# The guest's headroom, from that line: a lot of tests would pass or fail the
# same with 10% more or less memory, but one that gets within 10% of MemTotal
# has no room for a slow host's bigger page cache or a few more processes, and
# when it fails anyway the lack of memory is the first suspect (ENOMEM and
# ENOSPC on tmpfs leave no OOM-killer line to find). tmpfs, /tmp here, is
# capped at half of MemTotal, so 40% of MemTotal in Shmem (the initramfs
# counts too) is as close to that cap as 10% is to running out.
HEADROOM_LOW=0
eval "$(grep -a "^MEM total=" "$LOG" | tail -n 1 | awk '{
	for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
	printf "MEM_TOTAL_MIB=%d MEM_MIN_AVAIL_MIB=%d MEM_PEAK_USED_MIB=%d HEADROOM_LOW=%d\n",
		v["total"] / 1024, v["min_avail"] / 1024, v["peak_used"] / 1024,
		(v["min_avail"] * 10 < v["total"] || v["peak_shmem"] * 5 > v["total"] * 2)
	printf "RECLAIM_SCANS=%d\n", v["reclaim_scans"]
}')"
echo "run-qemu.sh: guest memory: -m $MEM, MemTotal $MEM_TOTAL_MIB MiB, peak in use $MEM_PEAK_USED_MIB MiB, lowest MemAvailable $MEM_MIN_AVAIL_MIB MiB"
if [ "$HEADROOM_LOW" -eq 1 ]; then
	echo "run-qemu.sh: WARNING: the guest came within 10% of running out of memory (or" \
		"40% of MemTotal in tmpfs, which is capped at 50%); if this run failed, that is" \
		"the first suspect: raise this test's mem= (test/qemu/README.md, \"Guest memory\")" >&2
fi
# Step 23.7: reclaim. MemAvailable counts the reclaimable caches (cached
# inodes, dentries, pages) as available, so a guest can be evicting them
# while the headroom above looks fine; a test that depends on something
# staying cached (destroy_test: 100,000 pinned inodes) is invalid then.
# guest/init counts the pages kswapd and direct reclaim scanned.
if [ "${RECLAIM_SCANS:-0}" -gt 0 ]; then
	echo "run-qemu.sh: WARNING: the guest reclaimed memory ($RECLAIM_SCANS pages scanned):" \
		"tests that depend on cached inodes or pages may be invalid; raise this test's" \
		"mem= (test/qemu/README.md, \"Guest memory\")" >&2
fi
# Step 23.7: a kernel that oopsed, hit a BUG or a WARNING, or panicked is a
# failure whatever the test's own checks said (an ext4 casefold oops went
# unnoticed for a release because the test still passed). The console's
# loglevel=3 shows only the worst of these; guest/init copies the kernel
# log's matching lines to the end of the serial log as KERNEL-OOPS: lines
# (mem_report), so the warnings and call traces are here too. The patterns
# start a kernel message (after the console's timestamp, if any), so the
# same words in a test's output do not match (absl's own userspace "WARNING:
# All log messages before absl::InitializeLog()..." does not: a kernel warning
# says "WARNING: CPU:", and the other real kernel warnings (guest/init lists
# them; hardware-vulnerability advisories are not among them) arrive as a
# KERNEL-OOPS: line).
KERNEL_FAIL='^KERNEL-OOPS:|(^|[] ])(BUG:|Oops[: ]|kernel BUG at|WARNING: CPU:|Call Trace:|Kernel panic)'
# A kernel failure under --expect-kernel-failure (step 23.7): the guest
# script is a reproducer of a kernel bug (guest/casefold_tune_oops.sh, a
# DISABLED_ check) and the oops is what it demonstrates. Tolerated only if
# the guest reported it itself ("would FAIL (kernel: ..."), and only an oops
# (not a WARNING, a BUG at, or a panic); the verdict then follows the other
# checks. A fixed kernel logs nothing and passes too.
KERNEL_OOPS_ONLY='BUG: (kernel NULL pointer dereference|unable to handle)|Oops[: ]|Call Trace:|general protection fault'
if [ -n "$EXPECT_KERNEL_FAILURE" ] && grep -q -a -E "$KERNEL_FAIL" "$LOG"; then
	unexpected=$(grep -a -E "$KERNEL_FAIL" "$LOG" | grep -a -v -E "$KERNEL_OOPS_ONLY" | head -n 1 || true)
	if [ -z "$unexpected" ] && grep -q -a "would FAIL (kernel: " "$LOG"; then
		echo "run-qemu.sh: the guest's kernel oops is expected (--expect-kernel-failure $EXPECT_KERNEL_FAILURE)"
		KERNEL_FAIL=
	fi
fi
if [ -n "$KERNEL_FAIL" ] && grep -q -a -E "$KERNEL_FAIL" "$LOG"; then
	echo "run-qemu.sh: ERROR: the guest kernel logged a failure (an oops, BUG, WARNING or panic); the first such line:" >&2
	grep -a -m 1 -E "$KERNEL_FAIL" "$LOG" >&2
	echo "run-qemu.sh: the lines around it are in $LOG (KERNEL-OOPS: lines are the kernel log's)" >&2
	echo "== RESULT: FAIL (kernel failure in the guest; see $LOG) =="
	exit 1
fi
if [ "$UNIT" -eq 1 ]; then
	if grep -q "^DCFS-TEST-EXIT=0" "$LOG"; then
		echo "== RESULT: PASS ($(awk "BEGIN{printf \"%.3f\", $end-$start}")s) =="
		exit 0
	else
		echo "== RESULT: FAIL (see $LOG) =="
		exit 1
	fi
else
	if grep -q "^ALL-TESTS-PASSED" "$LOG" && ! grep -q "^TEST-FAILED" "$LOG"; then
		echo "== RESULT: PASS =="
		exit 0
	else
		echo "== RESULT: FAIL (see $LOG) =="
		exit 1
	fi
fi
