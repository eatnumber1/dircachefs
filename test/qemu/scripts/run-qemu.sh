#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Boot the dcfs QEMU guest and report the verdict. --qemu and --qboot are
# mandatory in both modes (step 4.4): the Bazel-built
# //third_party/qemu:qemu_system_x86_64 and @qemu//:pc-bios/qboot.rom
# targets, passed as $(location ...) by qemu_test.bzl/qemu_cc_test.bzl --
# never a host PATH lookup or a default path. So are the mkfs tools for the
# scratch disks (R3): --mke2fs, --mke2fs-conf (MKE2FS_CONFIG), --mkfs-xfs and
# --mkfs-btrfs, in front of the other flags. Two modes:
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
#
# Each disk-spec is <device>:<fstype>:<size>, e.g. vdb:ext4:256M, where
# <device> is a /dev/vd<letter> name. Disks are attached to QEMU in
# <letter> order (vda, vdb, ...); any skipped letter gets a small
# unformatted filler drive, so the guest kernel enumerates the requested
# disk at exactly /dev/vd<letter>.
#
# Fast boot (step 5.1b): QEMU's microvm machine, direct kernel boot, no
# PC/ISA legacy devices we don't need (PIT/PIC/option ROMs), virtio-mmio
# disks (microvm has no PCI). Firmware is qboot (QBOOT below): qboot uses
# the kernel's PVH entry point directly when the kernel supports it (see
# the test kernel's CONFIG_PVH=y) for an effectively firmware-less boot,
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
# this is what CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES, set by
# third_party/linux/kernel.config, parses). The test kernel has no ACPI at all
# (CONFIG_ACPI=n, trimmed along with everything else it doesn't need), so
# with acpi=on/auto it never finds its disks -- confirmed experimentally
# (verified with `info qtree` over the QEMU monitor: the virtio-blk-device
# is correctly attached to a virtio-mmio transport either way, but without
# acpi=off the guest has no way to learn the transport's MMIO address and
# /dev/vd* never appears).
set -eu

# Step 4.4: the QEMU binary and qboot ROM are Bazel-built targets
# (//third_party/qemu:qemu_system_x86_64, @qemu//:pc-bios/qboot.rom) passed
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
# --mem <MiB>: guest RAM for an e2e guest, overriding the 1024 default (the
# slow names test needs more under ASan: its dcfs peaks near 720 MiB there).
MEM_OVERRIDE=""
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
	echo "(they must be the Bazel-built //third_party/qemu targets, not a host lookup)" >&2
	exit 1
fi

if [ -z "$MKE2FS_BIN" ] || [ -z "$MKE2FS_CONF" ] || [ -z "$MKFS_XFS_BIN" ] ||
	[ -z "$MKFS_BTRFS_BIN" ]; then
	echo "run-qemu.sh: --mke2fs, --mke2fs-conf, --mkfs-xfs and --mkfs-btrfs are required" >&2
	echo "(the Bazel-built //third_party/{e2fsprogs,xfsprogs,btrfs-progs} targets, not a host lookup)" >&2
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

WORKDIR="${TEST_TMPDIR:-$(mktemp -d)}"
LOG="${TEST_UNDECLARED_OUTPUTS_DIR:-$WORKDIR}/serial.log"

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
	letter=${dev#vd}
	index=$(($(printf '%d' "'$letter") - $(printf '%d' "'a")))
	echo "$index:$dev:$fstype:$size" >>"$specs_file"
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
		img="$WORKDIR/$dev.img"
		truncate -s "$size" "$img"
		case "$fstype" in
		ext4) MKE2FS_CONFIG="$MKE2FS_CONF" "$MKE2FS_BIN" -q -F -t ext4 "$img" ;;
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
if [ "$ACCEL" = kvm ]; then
	UNIT_TIMEOUT=60
	E2E_TIMEOUT=1800
else
	UNIT_TIMEOUT=300
	E2E_TIMEOUT=7200
fi
if [ "$UNIT" -eq 1 ]; then
	TIMEOUT_SECS="${TIMEOUT:-$UNIT_TIMEOUT}"
else
	TIMEOUT_SECS="${TIMEOUT:-$E2E_TIMEOUT}"
fi

if [ "$UNIT" -eq 1 ]; then
	MEM=256
	SMP=1
else
	MEM="${MEM_OVERRIDE:-1024}"
	SMP=2
fi

append="console=ttyS0 reboot=t panic=-1 loglevel=3 rdinit=/init dcfs_accel=$ACCEL"
if [ "$UNIT" -eq 0 ]; then
	append="$append dcfs_test=$DCFS_TEST"
fi
append="$append$rootfs_append"

# Record exactly which binaries this run used, for anyone auditing a
# serial log (and for the harness check below): a Bazel-built path looks
# like ".../bazel-out/k8-fastbuild/bin/third_party/qemu/..." or an
# external-repo path under ".../external/qemu+/...", never "/usr/...".
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
		"not the Bazel-built @qemu//:pc-bios/qboot.rom target" >&2
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

echo
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
