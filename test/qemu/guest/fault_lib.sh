# Fault injection over a disk with device-mapper (step 11.1): the helper the
# failure tests (guest/fault_*.sh, Phase 11) source after lib.sh.
#
# A disk is wrapped once, in a dm device that starts healthy; the tests then
# switch its table live, at a point of their choosing, to make it fail "from
# now on", drop what is written to it, or recover. Mount the filesystem on
# fault_dev NAME, never on the raw disk, so the switch reaches it.
#
#   fault_wrap NAME DEV      DEV (/dev/vdb) as /dev/mapper/NAME, healthy
#   fault_dev NAME           /dev/mapper/NAME
#   fault_mode NAME MODE     the table NAME has from now on, MODE one of
#       healthy        every I/O reaches DEV (a linear table)
#       error-writes   writes fail with EIO, reads work (dm-flakey error_writes)
#       drop-writes    writes complete and are lost: a power cut for DEV,
#                      once the filesystem is unmounted and remounted with
#                      fault_mode NAME healthy (dm-flakey drop_writes)
#       error-reads    reads fail with EIO, writes work (dm-flakey error_reads)
#       error-io       reads and writes fail (dm-flakey error_reads and
#                      error_writes)
#       dead           every I/O fails: no DEV behind it (dm-error)
#   fault_unwrap NAME        removes the device (unmount it first)
#
# A flakey table is `flakey DEV 0 0 1 <n> <features>`: the up interval is 0,
# so the device is always in its down interval and the features apply to
# every I/O. Switching is `dmsetup suspend --nolockfs`, load, resume: the
# filesystem is not frozen (it would flush, and a mounted filesystem is the
# point), I/O in flight completes first and I/O issued meanwhile waits for the
# resume.
#
# Errors surface later than the table switch: a filesystem's writes go to its
# page cache and journal, so `sync`, an fsync or the journal's own commit (5 s
# on ext4) is what meets the error. A test that wants the error at a point
# forces that point (`sync`, `drop_caches` for reads).
#
# The modules (modules = ["dm_flakey"] on the test target; dm-error is part of
# dm-mod) and /sbin/dmsetup (@alpine_dmsetup) come from the initramfs.

DMSETUP=${DMSETUP:-/sbin/dmsetup}

# fault_dev NAME
fault_dev() { echo "/dev/mapper/$1"; }

# fault_sectors DEV: the size of DEV in 512-byte sectors.
fault_sectors() {
	fs_dev=${1#/dev/}
	cat "/sys/class/block/$fs_dev/size"
}

# fault_table NAME DEV MODE: the dm table text for MODE over DEV.
fault_table() {
	ft_name=$1
	ft_dev=$2
	ft_sectors=$(fault_sectors "$ft_dev") || return 1
	case "$3" in
	healthy) echo "0 $ft_sectors linear $ft_dev 0" ;;
	error-writes) echo "0 $ft_sectors flakey $ft_dev 0 0 1 1 error_writes" ;;
	drop-writes) echo "0 $ft_sectors flakey $ft_dev 0 0 1 1 drop_writes" ;;
	error-reads) echo "0 $ft_sectors flakey $ft_dev 0 0 1 1 error_reads" ;;
	error-io) echo "0 $ft_sectors flakey $ft_dev 0 0 1 2 error_reads error_writes" ;;
	dead) echo "0 $ft_sectors error" ;;
	*)
		echo "fault_lib: $ft_name: unknown mode $3" >&2
		return 1
		;;
	esac
}

# fault_wrap NAME DEV
fault_wrap() {
	fw_table=$(fault_table "$1" "$2" healthy) || return 1
	FAULT_DEV_OF="$FAULT_DEV_OF $1=$2"
	"$DMSETUP" create --noudevsync "$1" --table "$fw_table"
}

# fault_underlying NAME: the DEV NAME was wrapped over.
fault_underlying() {
	for fu_pair in $FAULT_DEV_OF; do
		if [ "${fu_pair%%=*}" = "$1" ]; then
			echo "${fu_pair#*=}"
			return 0
		fi
	done
	echo "fault_lib: $1 was never wrapped" >&2
	return 1
}

# fault_mode NAME MODE
fault_mode() {
	fm_dev=$(fault_underlying "$1") || return 1
	fm_table=$(fault_table "$1" "$fm_dev" "$2") || return 1
	"$DMSETUP" suspend --nolockfs --noudevsync "$1" || return 1
	if "$DMSETUP" load "$1" --table "$fm_table"; then
		"$DMSETUP" resume --noudevsync "$1"
	else
		"$DMSETUP" resume --noudevsync "$1"
		return 1
	fi
}

# fault_unwrap NAME
fault_unwrap() {
	"$DMSETUP" remove --noudevsync "$1"
}
