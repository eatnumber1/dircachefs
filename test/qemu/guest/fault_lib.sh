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
#   fault_window NAME UP DOWN  a flakey table that works for UP seconds from the
#       switch, then fails every read for DOWN seconds, then works for UP, and
#       so on (dm-flakey error_reads): a failure that ends by itself. Sets
#       FAULT_WINDOW_START to the uptime at the switch. The cycle repeats: the
#       caller switches back to healthy once it has seen the window end.
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

# fault_window NAME UP DOWN
fault_window() {
	fwn_dev=$(fault_underlying "$1") || return 1
	fwn_sectors=$(fault_sectors "$fwn_dev") || return 1
	fwn_table="0 $fwn_sectors flakey $fwn_dev 0 $2 $3 1 error_reads"
	"$DMSETUP" suspend --nolockfs --noudevsync "$1" || return 1
	if "$DMSETUP" load "$1" --table "$fwn_table"; then
		"$DMSETUP" resume --noudevsync "$1"
		FAULT_WINDOW_START=$(cut -d' ' -f1 /proc/uptime)
	else
		"$DMSETUP" resume --noudevsync "$1"
		return 1
	fi
}

# The identity oracle (step 26.14e, the minimal form of 11.7's). Every crash
# test's other oracle is path-shaped (walk and list after recovery); a row that
# says an object exists when it does not (23.11: a clean row of a file the
# backing filesystem no longer has) is reachable only through a nodeid or
# handle held across the cut. So before a cut or a crash take the handle of
# every object, and after the recovery require that each one opens to the same
# object or fails ESTALE, never answers for something gone.
#
#   identity_take MNT DIR FILE   append a record to FILE for DIR and everything
#                                under it (paths under the mount MNT): the
#                                handle (name_to_handle_at, `fhtest handle`)
#                                and what it opens now (`fhtest stat`: inode
#                                number and mode)
#   identity_check MNT SRC FILE  for every record of FILE, open the handle
#                                again (open_by_handle_at on MNT) and print
#                                one line per violation (nothing: the oracle
#                                holds):
#                                - the same object: the inode number and file
#                                  type are the recorded ones, and the object
#                                  has a name under MNT (find -inum) that the
#                                  backing filesystem has too, at the same
#                                  path under SRC;
#                                - or ESTALE (the object may be gone, and the
#                                  handle must say so);
#                                - anything else is a violation, ENOENT and EIO
#                                  included.
#
# Event-based: no waits; the file names are plain words (the tests' trees), as
# the records are lines of space-separated words.
FHTEST=${FHTEST:-/bin/fhtest}

identity_take() {
	it_mnt=$1
	it_dir=$2
	it_file=$3
	find "$it_dir" | while IFS= read -r it_path; do
		it_handle=$("$FHTEST" handle "$it_path") || continue
		case "$it_handle" in ERR*) continue ;; esac
		# shellcheck disable=SC2086 # "TYPE LEN HEX"
		set -- $it_handle
		it_type=$1
		it_hex=$3
		# shellcheck disable=SC2086 # "OK INO MODE PATH", or "ERR NAME"
		set -- $("$FHTEST" stat "$it_mnt" "$it_type" "$it_hex")
		case "$1" in
		OK) echo "$it_path $2 $3 $it_type $it_hex" ;;
		*) echo "$it_path ? ? $it_type $it_hex" ;;
		esac
	done >>"$it_file"
}

identity_check() {
	ic_mnt=$1
	ic_src=$2
	sort -u "$3" | while IFS=' ' read -r ic_path ic_ino ic_mode ic_type ic_hex; do
		ic_now=$("$FHTEST" stat "$ic_mnt" "$ic_type" "$ic_hex")
		# shellcheck disable=SC2086 # "OK INO MODE PATH", or "ERR NAME"
		set -- $ic_now
		ic_rel=${ic_path#"$ic_mnt"/}
		case "${1:-}" in
		ERR)
			[ "${2:-}" = ESTALE ] || echo "the handle of $ic_rel failed with ${2:-nothing}, want the same object or ESTALE"
			;;
		OK)
			if [ "$ic_ino" != ? ] && [ "${2:-}" != "$ic_ino" ]; then
				echo "the handle of $ic_rel opened a different object: inode ${2:-}, was $ic_ino"
			elif [ "$ic_mode" != ? ] && [ $(((0${3:-0}) & 0170000)) != $(((0$ic_mode) & 0170000)) ]; then
				echo "the handle of $ic_rel opened an object of another type: mode ${3:-}, was $ic_mode"
			else
				# What the handle opened must still be somewhere: a name under the
				# mount with that inode number (the path `fhtest stat` prints is
				# no help for a file: the kernel names a file's dentry only when
				# it is connected), and that name must be on the backing
				# filesystem too.
				ic_name=$(find "$ic_mnt" -inum "${2:-0}" 2>/dev/null | head -n 1)
				if [ -z "$ic_name" ]; then
					echo "the handle of $ic_rel opened inode ${2:-}, which has no name under $ic_mnt (an answer for something gone)"
				else
					ic_at=${ic_name#"$ic_mnt"}
					if [ ! -e "$ic_src$ic_at" ] && [ ! -L "$ic_src$ic_at" ]; then
						echo "the handle of $ic_rel opened ${ic_name#"$ic_mnt"/}, which the backing filesystem does not have (an answer for something gone)"
					fi
				fi
			fi
			;;
		*)
			echo "the handle of $ic_rel answered '$ic_now', want the same object or ESTALE"
			;;
		esac
	done
}
