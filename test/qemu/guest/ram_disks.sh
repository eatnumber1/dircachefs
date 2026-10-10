#!/bin/sh
# Self-check of qemu_test's ram_disks option (step 26.17): the test's disks
# are in the guest's RAM (a loop device over a tmpfs file that guest/init made,
# formatted there), not virtio disks backed by files on the host, and a
# device-mapper target and a filesystem on top of them do what they do on a
# real disk. A harness that left the disks on the host would give the fault
# tests the host disk's latency again without a word.
#
# Run as /tests/ram_disks.sh by guest/init when booted with
# dcfs_test=ram_disks.sh; the target declares vdb (ext4, 64M) and vdc (xfs,
# 320M).
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

MNT=/mnt/ram
NAME=ramdisk
cleanup() {
	umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fault_unwrap "$NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "ram_disks.sh: kernel $(uname -r)"
require_commands blockdev readlink stat umount
mkdir -p "$MNT"

# check_ram_disk DEV FSTYPE BYTES: DEV is a block device whose kernel name is a
# loop device, attached to a file on a tmpfs, of the size and filesystem the
# target declared.
check_ram_disk() {
	crd_dev=$1
	crd_name=${crd_dev#/dev/}
	if [ ! -b "$crd_dev" ]; then
		fail "$crd_name-present" "$crd_dev is not a block device"
		return
	fi
	pass "$crd_name-present"
	crd_majmin=$(printf '%d:%d' "0x$(stat -L -c %t "$crd_dev")" "0x$(stat -L -c %T "$crd_dev")")
	crd_kernel=$(basename "$(readlink -f "/sys/dev/block/$crd_majmin")")
	case "$crd_kernel" in
	loop*) pass "$crd_name-is-loop" ;;
	*) fail "$crd_name-is-loop" "$crd_dev is the kernel's $crd_kernel, not a loop device" ;;
	esac
	crd_file=$(cat "/sys/block/$crd_kernel/loop/backing_file" 2>/dev/null)
	# tmpfs's statfs magic, 0x01021994.
	if [ -n "$crd_file" ] && [ "$(stat -f -c %t "$crd_file")" = 1021994 ]; then
		pass "$crd_name-backed-by-tmpfs"
	else
		fail "$crd_name-backed-by-tmpfs" "backing file '$crd_file' is not on a tmpfs (statfs type $(stat -f -c %t "$crd_file" 2>&1))"
	fi
	crd_size=$(blockdev --getsize64 "$crd_dev")
	if [ "$crd_size" = "$3" ]; then
		pass "$crd_name-size"
	else
		fail "$crd_name-size" "$crd_size bytes, declared $3"
	fi
	crd_want=$2
	crd_fs=$(fd_fstype "$crd_dev")
	if [ "$crd_fs" = "$crd_want" ]; then
		pass "$crd_name-filesystem"
	else
		fail "$crd_name-filesystem" "formatted as $crd_fs, declared $crd_want"
	fi
}

check_ram_disk /dev/vdb ext4 $((64 * 1024 * 1024))
check_ram_disk /dev/vdc xfs $((320 * 1024 * 1024))

# No virtio disk is the test's: the host attached no image for either.
if [ -e /sys/block/vdb ] || [ -e /sys/block/vdc ]; then
	fail no-virtio-disks "a virtio disk is named vdb or vdc"
else
	pass no-virtio-disks
fi

# The failure semantics over a RAM disk: a dm-flakey table that drops writes
# loses what was written after the switch and keeps what was synced before.
if fault_wrap "$NAME" /dev/vdb && mount "$(fault_dev "$NAME")" "$MNT"; then
	pass dm-mount
	echo kept >"$MNT/kept"
	sync
	fault_mode "$NAME" drop-writes || fail dm-drop-writes "fault_mode failed"
	echo lost >"$MNT/lost"
	sync
	umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fault_mode "$NAME" healthy || fail dm-healthy "fault_mode failed"
	if mount "$(fault_dev "$NAME")" "$MNT"; then
		if [ "$(cat "$MNT/kept" 2>&1)" = kept ] && [ ! -e "$MNT/lost" ]; then
			pass dm-drop-writes
		else
			fail dm-drop-writes "kept: '$(cat "$MNT/kept" 2>&1)', lost exists: $([ -e "$MNT/lost" ] && echo yes || echo no)"
		fi
	else
		fail dm-remount "cannot mount after the dropped writes"
	fi
else
	fail dm-mount "wrapping /dev/vdb or mounting it failed"
fi

exit "$FAILED"
