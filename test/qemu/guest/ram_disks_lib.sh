# shellcheck shell=sh
# The verdicts of guest/ram_disks.sh (step 26.17) as a function of the values it
# reads from the guest, so that //test/qemu:ram_disks_lib_test can feed it good
# and bad values on the host: a check that nothing ever fails would pass every
# RAM disk, and until it is shown to fail it has proved nothing.

# ram_disk_judge KERNEL FILE MAGIC SIZE WANT_SIZE FS WANT_FS ITABLE LOGBS ROTATIONAL:
# one line per check, "<check> ok" or "<check> <why not>".
#   KERNEL      the kernel's name for the block device (sysfs): loopN
#   FILE        its backing file (loop/backing_file)
#   MAGIC       that file's statfs type, as `stat -f -c %t` prints it
#   SIZE        the device's size in bytes, WANT_SIZE the declared one
#   FS          the filesystem found on it, WANT_FS the declared one
#   ITABLE      ext4: group 0's bg_flags; LOGBS: ext4's s_log_block_size
#               ("-" for the others)
#   ROTATIONAL  queue/rotational of the device
ram_disk_judge() {
	rj_kernel=$1 rj_file=$2 rj_magic=$3 rj_size=$4 rj_want_size=$5
	rj_fs=$6 rj_want_fs=$7 rj_itable=$8 rj_logbs=$9
	shift 9
	rj_rotational=$1
	case "$rj_kernel" in
	loop*) echo "is-loop ok" ;;
	*) echo "is-loop the kernel's $rj_kernel, not a loop device" ;;
	esac
	# On the tmpfs guest/init made for the RAM disks (capped at their size, under
	# /ramdisks), not any file on any tmpfs. tmpfs's statfs magic is 0x01021994.
	case "$rj_file" in
	/ramdisks/*)
		if [ "$rj_magic" = 1021994 ]; then
			echo "backed-by-ramdisks-tmpfs ok"
		else
			echo "backed-by-ramdisks-tmpfs $rj_file is on statfs type $rj_magic, not a tmpfs"
		fi
		;;
	*) echo "backed-by-ramdisks-tmpfs backing file '$rj_file' is not under /ramdisks" ;;
	esac
	if [ "$rj_size" = "$rj_want_size" ]; then
		echo "size ok"
	else
		echo "size $rj_size bytes, declared $rj_want_size"
	fi
	if [ "$rj_fs" = "$rj_want_fs" ]; then
		echo "filesystem ok"
	else
		echo "filesystem formatted as $rj_fs, declared $rj_want_fs"
	fi
	if [ "$rj_rotational" = 1 ]; then
		echo "rotational ok"
	else
		echo "rotational queue/rotational is '$rj_rotational': a virtio disk says 1, and btrfs and ext4 choose by it"
	fi
	if [ "$rj_want_fs" = ext4 ]; then
		# Made on a file, as the host makes an image: every group's inode table
		# marked zeroed (the file's discard punched holes), so nothing writes
		# zeros after a mount; made on the block device it is left to the
		# kernel's ext4lazyinit thread, a spontaneous writer.
		if [ $((rj_itable & 4)) -ne 0 ]; then
			echo "itable-zeroed ok"
		else
			echo "itable-zeroed group 0's bg_flags is $rj_itable, without ITABLE_ZEROED (4): the kernel would zero the inode tables after the mount"
		fi
		# 4096-byte blocks (log 2) are the checked-in profile's, mke2fs's own
		# built-in profile gives a 64 MiB filesystem 1024-byte ones: the
		# profile reached mke2fs.
		if [ "$rj_logbs" = 2 ]; then
			echo "mke2fs-profile ok"
		else
			echo "mke2fs-profile s_log_block_size is $rj_logbs, not 2 (4096 bytes): mke2fs did not get the checked-in profile"
		fi
	fi
}
