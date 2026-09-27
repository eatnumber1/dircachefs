#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Boot the dcfs QEMU guest for one qemu_test target (see
# test/qemu/qemu_test.bzl) and report the verdict. Exit 0 iff the guest
# printed ALL-TESTS-PASSED and did not print TEST-FAILED.
#
# Usage: run-qemu.sh <bzImage> <initramfs.cpio.gz> <dcfs_test-basename> \
#            [disk-spec...]
#
# Each disk-spec is <device>:<fstype>:<size>, e.g. vdb:ext4:256M, where
# <device> is a /dev/vd<letter> name. Disks are attached to QEMU in
# <letter> order (vda, vdb, ...); any skipped letter gets a small
# unformatted filler drive, so the guest kernel enumerates the requested
# disk at exactly /dev/vd<letter>.
set -eu

KERNEL=$1
INITRD=$2
DCFS_TEST=$3
shift 3

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

WORKDIR="${TEST_TMPDIR:-$(mktemp -d)}"
LOG="${TEST_UNDECLARED_OUTPUTS_DIR:-$WORKDIR}/serial.log"
TIMEOUT_SECS="${TIMEOUT:-1200}" # 20 minutes: TCG (no KVM) is slow.

# --- lay out the disks in letter order, one -drive per index -------------
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
		img="$WORKDIR/filler-$idx.img"
		truncate -s 1M "$img"
	fi
	drive_args="$drive_args -drive file=$img,format=raw,if=virtio"
	idx=$((idx + 1))
done

# --- boot ------------------------------------------------------------
# shellcheck disable=SC2086 # drive_args is a deliberately unquoted list of flags
timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
	-accel kvm:tcg \
	-m 1024 -smp 2 \
	-nographic -no-reboot \
	-kernel "$KERNEL" \
	-initrd "$INITRD" \
	-append "console=ttyS0 rdinit=/init panic=-1 loglevel=4 dcfs_test=$DCFS_TEST" \
	$drive_args \
	2>&1 | tee "$LOG" || true

echo
if grep -q "^ALL-TESTS-PASSED" "$LOG" && ! grep -q "^TEST-FAILED" "$LOG"; then
	echo "== RESULT: PASS =="
	exit 0
else
	echo "== RESULT: FAIL (see $LOG) =="
	exit 1
fi
