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

# The mkfs tools, their loader and libraries: the archive run-qemu.sh appended
# (scripts/mkfstools.py). They are linked against musl, so they run through its
# loader, as @alpine_fstools' wrappers run them on the host.
RAM_DISK_FSTOOLS=/opt/fstools

# ram_disk_attach FILE: attaches FILE to the next free loop device and prints
# its name.
ram_disk_attach() {
	rd_dev=$(losetup -f) && losetup "$rd_dev" "$1" && echo "$rd_dev"
}

# ram_disk_bytes SIZE: SIZE (a number with an optional K, M or G) in bytes.
ram_disk_bytes() {
	case "$1" in
	*K) echo $((${1%K} * 1024)) ;;
	*M) echo $((${1%M} * 1024 * 1024)) ;;
	*G) echo $((${1%G} * 1024 * 1024 * 1024)) ;;
	*) echo "$1" ;;
	esac
}

# ram_disk_mkfs FSTYPE DEV: formats DEV as run-qemu.sh formats an image (the
# same tools, the same options, the checked-in mke2fs profile).
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
# <device>:<fstype>:<size>. Makes each: a sparse file of SIZE in a tmpfs that
# holds no more than all the disks together would (a disk that fills up fills
# up, as an image would), a loop device over it, FSTYPE on it, and the node
# /dev/<device> (vdb) for the same block device, so that a test names its disks
# as it does with virtio ones. Returns nonzero, saying why, on the first failure.
ram_disks_make() {
	rd_total=0
	for rd_spec in $(echo "$1" | tr , ' '); do
		rd_size=${rd_spec#*:}
		rd_size=${rd_size#*:}
		rd_total=$((rd_total + $(ram_disk_bytes "$rd_size")))
	done
	mkdir -p /ramdisks &&
		mount -t tmpfs -o "size=$rd_total" ramdisks /ramdisks || {
		echo "ramdisk_lib: cannot mount a tmpfs of $rd_total bytes on /ramdisks" >&2
		return 1
	}
	for rd_spec in $(echo "$1" | tr , ' '); do
		rd_name=${rd_spec%%:*}
		rd_rest=${rd_spec#*:}
		rd_fstype=${rd_rest%%:*}
		rd_size=${rd_rest#*:}
		if [ -e "/dev/$rd_name" ]; then
			echo "ramdisk_lib: /dev/$rd_name exists already" >&2
			return 1
		fi
		truncate -s "$rd_size" "/ramdisks/$rd_name.img" || return 1
		rd_loop=$(ram_disk_attach "/ramdisks/$rd_name.img") || {
			echo "ramdisk_lib: cannot attach /ramdisks/$rd_name.img to a loop device" >&2
			return 1
		}
		ram_disk_mkfs "$rd_fstype" "$rd_loop" || {
			echo "ramdisk_lib: formatting $rd_loop as $rd_fstype failed" >&2
			return 1
		}
		# shellcheck disable=SC2046 # "major minor" is two words
		mknod "/dev/$rd_name" b $(tr : ' ' <"/sys/block/${rd_loop#/dev/}/dev") || return 1
		echo "init: RAM disk /dev/$rd_name: $rd_fstype, $rd_size, $rd_loop over /ramdisks/$rd_name.img"
	done
}
