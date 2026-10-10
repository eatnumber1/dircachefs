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
. "$(dirname "$0")/ram_disks_lib.sh"

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

# check_ram_disk DEV FSTYPE BYTES: reads from the guest what ram_disk_judge
# (guest/ram_disks_lib.sh) judges, for the block device DEV the target declared
# with that filesystem and size, and turns its lines into checks.
check_ram_disk() {
	crd_dev=$1
	crd_name=${crd_dev#/dev/}
	if [ ! -b "$crd_dev" ]; then
		fail "$crd_name-present" "$crd_dev is not a block device"
		return
	fi
	pass "$crd_name-present"
	crd_sys=$(block_sysfs "$crd_name")
	crd_kernel=$(basename "$(readlink -f "$crd_sys")")
	crd_file=$(cat "$crd_sys/loop/backing_file" 2>/dev/null)
	crd_magic=$(stat -f -c %t "$crd_file" 2>/dev/null)
	crd_itable=-
	crd_logbs=-
	if [ "$2" = ext4 ]; then
		# Read before anything mounts the disk. Group 0's descriptor is in block 1
		# (4096-byte blocks), bg_flags at +0x12; s_log_block_size is at +0x18 of the
		# superblock, which starts at byte 1024.
		crd_itable=$(($(dd if="$crd_dev" bs=1 skip=$((4096 + 18)) count=1 2>/dev/null | od -An -tu1)))
		crd_logbs=$(($(dd if="$crd_dev" bs=1 skip=$((1024 + 24)) count=1 2>/dev/null | od -An -tu1)))
	fi
	# Through a file, not a pipe: fail sets FAILED, which a pipe's subshell loses.
	ram_disk_judge "$crd_kernel" "$crd_file" "$crd_magic" "$(blockdev --getsize64 "$crd_dev")" "$3" \
		"$(fd_fstype "$crd_dev")" "$2" "$crd_itable" "$crd_logbs" \
		"$(cat "$crd_sys/queue/rotational" 2>/dev/null)" >/tmp/judged
	while read -r crd_check crd_why; do
		if [ "$crd_why" = ok ]; then
			pass "$crd_name-$crd_check"
		else
			fail "$crd_name-$crd_check" "$crd_why"
		fi
	done </tmp/judged
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

# lib.sh's size and counter helpers read the device, not the name sysfs does not
# have for it: the size is the declared one, and sectors_read gives the number of
# sectors read through the RAM disk (and, if a device had none, stops the
# script instead of letting two empty counters compare equal).
if [ "$(device_sectors /dev/vdb)" = $((64 * 1024 * 2)) ]; then
	pass device-sectors
else
	fail device-sectors "$(device_sectors /dev/vdb) sectors for a 64 MiB disk"
fi
case "$(sectors_read vdb)" in
'' | *[!0-9]*) fail sectors-read "sectors_read vdb gave '$(sectors_read vdb)', not a number" ;;
*) pass sectors-read ;;
esac
# In a shell of its own, as its failure stops the shell it runs in; the FAIL
# line it prints is kept out of this log, where it would fail the run.
sr_out=$(sh -c '. "$1"; x=$(sectors_read nonesuch); echo survived' sh "$(dirname "$0")/lib.sh" 2>&1)
case "$sr_out" in
*survived*) fail sectors-read-missing "the script went on after sectors_read of a device with no counter: $sr_out" ;;
*"no block statistics for nonesuch"*) pass sectors-read-missing ;;
*) fail sectors-read-missing "no message for a device with no counter: '$sr_out'" ;;
esac

exit "$FAILED"
