# shellcheck shell=sh
# RAM disks for the guest (step 26.17): a block device whose blocks are a
# sparse file in a tmpfs, made the one way both users make them. guest/init
# sources it to make the disks of a test that declared `ram_disks` (the
# dcfs_ramdisks= kernel command-line word, from run-qemu.sh --ram-disks), and
# guest/sqlite_durability.sh for the log, replay and copy-on-write devices
# it keeps beside its two virtio disks. brd, the kernel's RAM disk driver, is
# not in Alpine's linux-virt (CONFIG_BLK_DEV_RAM is off), so it is the loop
# driver (the `loop` module, which the test declares) over a tmpfs file; a loop
# device exists as soon as losetup returns, so nothing waits for it.
#
# Sizes are bytes, everywhere: run-qemu.sh turns the target's "320M" into
# 335544320 (its one parser), so the guest parses nothing that truncate might
# read differently.

# The mkfs tools, their loader and libraries: the archive run-qemu.sh appended
# (scripts/mkfstools.py). They are linked against musl, so they run through its
# loader, as @alpine_fstools' wrappers run them on the host.
RAM_DISK_FSTOOLS=/opt/fstools

# ram_disk_tmpfs DIR BYTES: a tmpfs on DIR (made if need be) that holds at most
# BYTES, so a RAM disk that fills up fills up as an image would, instead of
# taking the guest's memory.
ram_disk_tmpfs() {
	mkdir -p "$1" && mount -t tmpfs -o "size=$2" ramdisks "$1" || {
		echo "ramdisk_lib: cannot mount a tmpfs of $2 bytes on $1" >&2
		return 1
	}
}

# ram_disk_file FILE BYTES: FILE as a sparse file of BYTES.
ram_disk_file() {
	truncate -s "$2" "$1" || {
		echo "ramdisk_lib: cannot make $1 a sparse file of $2 bytes" >&2
		return 1
	}
}

# ram_disk_attach FILE: attaches FILE to the next free loop device and prints
# its name. The device says it is rotational, as a virtio disk does, so that
# what a filesystem decides from it (btrfs's ssd mode, ext4's allocator) is
# what it decides on the hosts' disks.
ram_disk_attach() {
	rd_dev=$(losetup -f) && losetup "$rd_dev" "$1" &&
		echo 1 >"/sys/block/${rd_dev#/dev/}/queue/rotational" && echo "$rd_dev"
}

# ram_disk_mkfs FSTYPE FILE: formats the file, as run-qemu.sh formats an image
# (the same tools, options and checked-in mke2fs profile, and the same kind of
# input: a regular file). On the loop device instead, mke2fs would see a block
# device that does not read as zeros after a discard, and with ext4 loaded
# leave the inode tables to the kernel's ext4lazyinit thread, which zeroes them
# after every mount: writes nothing in the test asked for.
ram_disk_mkfs() {
	rd_ld="$RAM_DISK_FSTOOLS/lib/ld-musl-x86_64.so.1"
	rd_libs="$RAM_DISK_FSTOOLS/usr/lib:$RAM_DISK_FSTOOLS/lib"
	case "$1" in
	ext4)
		env MKE2FS_CONFIG="$RAM_DISK_FSTOOLS/etc/mke2fs.conf" "$rd_ld" \
			--library-path "$rd_libs" "$RAM_DISK_FSTOOLS/sbin/mke2fs" -q -F -t ext4 "$2"
		;;
	xfs) "$rd_ld" --library-path "$rd_libs" "$RAM_DISK_FSTOOLS/sbin/mkfs.xfs" -q -f "$2" ;;
	btrfs) "$rd_ld" --library-path "$rd_libs" "$RAM_DISK_FSTOOLS/sbin/mkfs.btrfs" -q -f "$2" ;;
	*)
		echo "ramdisk_lib: unknown fstype '$1'" >&2
		return 1
		;;
	esac
}

# ram_disks_make SPECS: SPECS is dcfs_ramdisks='s value, comma-separated
# <device>:<fstype>:<bytes>. Makes each: a sparse file in a tmpfs that holds
# no more than all the disks together, FSTYPE made on the file, a loop device
# over it, and the node /dev/<device> (vdb) for the same block device, so that
# a test names its disks as it does with virtio ones. Returns nonzero, saying
# why, on the first failure.
ram_disks_make() {
	rd_total=0
	for rd_spec in $(echo "$1" | tr , ' '); do
		rd_total=$((rd_total + ${rd_spec##*:}))
	done
	ram_disk_tmpfs /ramdisks "$rd_total" || return 1
	for rd_spec in $(echo "$1" | tr , ' '); do
		rd_name=${rd_spec%%:*}
		rd_rest=${rd_spec#*:}
		rd_fstype=${rd_rest%%:*}
		rd_size=${rd_rest#*:}
		rd_img=/ramdisks/$rd_name.img
		# The name is taken when two specs share it, or when a virtio disk has
		# it: under `bazel coverage` run-qemu.sh attaches the profile disk as
		# the first virtio disk, /dev/vda (qemu_test refuses a RAM disk named
		# vda).
		if [ -e "/dev/$rd_name" ]; then
			echo "ramdisk_lib: /dev/$rd_name exists already (two specs of one name, or the coverage disk, /dev/vda)" >&2
			return 1
		fi
		ram_disk_file "$rd_img" "$rd_size" || return 1
		ram_disk_mkfs "$rd_fstype" "$rd_img" || {
			echo "ramdisk_lib: making $rd_fstype on $rd_img failed" >&2
			return 1
		}
		rd_loop=$(ram_disk_attach "$rd_img") || {
			echo "ramdisk_lib: cannot attach $rd_img to a loop device (or mark it rotational)" >&2
			return 1
		}
		# shellcheck disable=SC2046 # "major minor" is two words
		mknod "/dev/$rd_name" b $(tr : ' ' <"/sys/block/${rd_loop#/dev/}/dev") || {
			echo "ramdisk_lib: cannot make the node /dev/$rd_name for $rd_loop" >&2
			return 1
		}
		echo "init: RAM disk /dev/$rd_name: $rd_fstype, $rd_size bytes, $rd_loop over $rd_img"
	done
}
