#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Boot the dcfs QEMU guest and report the verdict. Two modes:
#
#   run-qemu.sh --unit <bzImage> <initramfs.cpio.gz> [disk-spec...]
#       For qemu_cc_test (test/qemu/qemu_cc_test.bzl): boots the guest,
#       which runs /test/run (guest/init's unit-test branch) and prints
#       DCFS-TEST-EXIT=<rc>. Exit 0 iff that line says rc=0.
#
#   run-qemu.sh <bzImage> <initramfs.cpio.gz> <dcfs_test-basename> \
#       [disk-spec...]
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
# build-kernel.sh's CONFIG_PVH=y) for an effectively firmware-less boot,
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
# build-kernel.sh, parses). The test kernel has no ACPI at all
# (CONFIG_ACPI=n, trimmed along with everything else it doesn't need), so
# with acpi=on/auto it never finds its disks -- confirmed experimentally
# (verified with `info qtree` over the QEMU monitor: the virtio-blk-device
# is correctly attached to a virtio-mmio transport either way, but without
# acpi=off the guest has no way to learn the transport's MMIO address and
# /dev/vd* never appears).
set -eu
# Bazel runs tests with a minimal PATH that lacks the sbin directories
# where mkfs.* live.
export PATH="$PATH:/usr/sbin:/sbin"

QBOOT="${DCFS_QBOOT:-/usr/share/qemu/qboot.rom}"

UNIT=0
ROOTFS=""
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
	*)
		break
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

# kernel_image (test/qemu/kernel.bzl) writes a placeholder starting with
# this marker when $DCFS_KERNEL_BUILD has no built kernel yet.
MAGIC=$(dd if="$KERNEL" bs=1 count=20 2>/dev/null)
case "$MAGIC" in
DCFS-KERNEL-MISSING*)
	echo "dcfs QEMU test kernel is not built:"
	cat "$KERNEL"
	exit 1
	;;
esac

# kernel_image similarly writes a placeholder for the Debian NFS-test
# rootfs image (test/qemu/scripts/mkrootfs-debian.sh) when it hasn't been
# built yet.
if [ -n "$ROOTFS" ]; then
	ROOTFS_MAGIC=$(dd if="$ROOTFS" bs=1 count=20 2>/dev/null)
	case "$ROOTFS_MAGIC" in
	DCFS-ROOTFS-MISSING*)
		echo "dcfs Debian NFS-test rootfs image is not built:"
		cat "$ROOTFS"
		exit 1
		;;
	esac
fi

WORKDIR="${TEST_TMPDIR:-$(mktemp -d)}"
LOG="${TEST_UNDECLARED_OUTPUTS_DIR:-$WORKDIR}/serial.log"
if [ "$UNIT" -eq 1 ]; then
	TIMEOUT_SECS="${TIMEOUT:-60}" # unit tests boot in well under a second.
else
	TIMEOUT_SECS="${TIMEOUT:-1200}" # 20 minutes: TCG (no KVM) is slow.
fi

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
		ext4) mkfs.ext4 -q -F "$img" ;;
		btrfs) mkfs.btrfs -q -f "$img" ;;
		xfs) mkfs.xfs -q -f "$img" ;;
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
# (slow -- see TIMEOUT_SECS above).
if [ -w /dev/kvm ]; then
	ACCEL=kvm
	CPU=host
else
	echo "run-qemu.sh: /dev/kvm is not writable, falling back to tcg (slow)" | tee -a "$LOG"
	ACCEL=tcg
	CPU=max
fi

if [ "$UNIT" -eq 1 ]; then
	MEM=256
	SMP=1
else
	MEM=1024
	SMP=2
fi

append="console=ttyS0 reboot=t panic=-1 loglevel=3 rdinit=/init"
if [ "$UNIT" -eq 0 ]; then
	append="$append dcfs_test=$DCFS_TEST"
fi
append="$append$rootfs_append"

start=$(date +%s.%N)
echo "run-qemu.sh: qemu start $start" >>"$LOG"
# shellcheck disable=SC2086 # drive_args is a deliberately unquoted list of flags
timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
	-M microvm,x-option-roms=off,pit=off,pic=off,rtc=on,isa-serial=on,acpi=off \
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
