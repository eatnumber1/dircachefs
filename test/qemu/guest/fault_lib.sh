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
#   identity_take MNT SRC DIR FILE   append a record to FILE for DIR (under the
#                                    mount MNT) and everything under it: the
#                                    handle dcfs gives it (name_to_handle_at on
#                                    the mount: a lookup), what that handle
#                                    opens now (inode number and mode), and the
#                                    same two for the backing filesystem's own
#                                    handle of the same name under SRC
#   identity_check MNT SRC FILE      open every handle again (open_by_handle_at
#                                    on MNT and, for the backing handle, on SRC;
#                                    all the answers are collected before
#                                    anything else is read) and print one line
#                                    per violation (nothing: the oracle holds):
#                                    - the same object: dcfs's answer has the
#                                      recorded inode number and file type AND
#                                      the backing handle of the object still
#                                      opens on the backing filesystem with the
#                                      recorded inode and type (a handle carries
#                                      the generation, so a recycled inode
#                                      number is not the object);
#                                    - or ESTALE (the object may be gone; a
#                                      note on standard error, and in
#                                      FILE.estale, when the backing filesystem
#                                      still has it);
#                                    - anything else is a violation, ENOENT and
#                                      EIO included.
#                                    FILE.checked counts the handles checked.
#
# Event-based: no waits. Names are plain words (the tests' trees); the records
# are lines of space-separated words. The primitives are testutil's handle-save
# and handle-stat (the names 23.11's fault_power `born` scenario uses).
IDENT_TMP=${IDENT_TMP:-/tmp/identity}

# identity_save PATH TAG: "TYPE HEX" of PATH's handle, "-" if it has none.
identity_save() {
	if "${TESTUTIL:-/bin/testutil}" handle-save "$1" "$IDENT_TMP.$2" >/dev/null; then
		cat "$IDENT_TMP.$2"
	else
		echo "- -"
	fi
}

# identity_open DIR TYPE HEX: testutil's answer ("OK SIZE INO MODE", "ERR NAME").
identity_open() {
	echo "$2 $3" >"$IDENT_TMP.open"
	"${TESTUTIL:-/bin/testutil}" handle-stat "$1" "$IDENT_TMP.open"
}

identity_take() {
	it_mnt=$1
	it_src=$2
	it_file=$4
	find "$3" | while IFS= read -r it_path; do
		it_m=$(identity_save "$it_path" m)
		it_b=$(identity_save "$it_src${it_path#"$it_mnt"}" b)
		# shellcheck disable=SC2086 # "TYPE HEX"
		set -- $it_m
		it_mans=$(identity_open "$it_mnt" "$1" "$2")
		# shellcheck disable=SC2086
		set -- $it_b
		it_bans=$(identity_open "$it_src" "$1" "$2")
		# shellcheck disable=SC2086 # "OK SIZE INO MODE"
		set -- $it_mans
		it_mino=${3:-?}
		it_mmode=${4:-?}
		# shellcheck disable=SC2086
		set -- $it_bans
		echo "$it_path $it_m $it_mino $it_mmode $it_b ${3:-?} ${4:-?}"
	done >>"$it_file"
}

# identity_ftype MODE: the file-type bits of an octal MODE.
identity_ftype() { echo $(((0$1) & 0170000)); }

identity_check() {
	ic_mnt=$1
	ic_src=$2
	ic_file=$3
	: >"$ic_file.answers"
	# Every answer first, before anything walks a tree or reads a file.
	sort -u "$ic_file" | while IFS=' ' read -r ic_path ic_mtype ic_mhex ic_mino ic_mmode ic_btype ic_bhex ic_bino ic_bmode; do
		ic_ma=$(identity_open "$ic_mnt" "$ic_mtype" "$ic_mhex" | tr ' ' ,)
		ic_ba=$(identity_open "$ic_src" "$ic_btype" "$ic_bhex" | tr ' ' ,)
		echo "$ic_path $ic_mino $ic_mmode $ic_bino $ic_bmode ${ic_ma:-none} ${ic_ba:-none}"
	done >>"$ic_file.answers"
	wc -l <"$ic_file.answers" >"$ic_file.checked"
	: >"$ic_file.estale"
	while IFS=' ' read -r ic_path ic_mino ic_mmode ic_bino ic_bmode ic_ma ic_ba; do
		ic_rel=${ic_path#"$ic_mnt"/}
		ic_ma=$(echo "$ic_ma" | tr , ' ')
		ic_ba=$(echo "$ic_ba" | tr , ' ')
		# shellcheck disable=SC2086 # "OK SIZE INO MODE", or "ERR NAME"
		set -- $ic_ma
		case "${1:-}" in
		ERR)
			if [ "${2:-}" = ESTALE ]; then
				# shellcheck disable=SC2086
				set -- $ic_ba
				if [ "${1:-}" = OK ] && [ "${3:-}" = "$ic_bino" ]; then
					echo "note: the handle of $ic_rel failed ESTALE although the backing filesystem still has the object" >&2
					echo "$ic_rel" >>"$ic_file.estale"
				fi
			else
				echo "the handle of $ic_rel failed with ${2:-nothing}, want the same object or ESTALE"
			fi
			;;
		OK)
			ic_got_ino=${3:-}
			ic_got_mode=${4:-}
			# shellcheck disable=SC2086
			set -- $ic_ba
			ic_bok=${1:-}
			ic_bgot_ino=${3:-}
			ic_bgot_mode=${4:-0}
			if [ "$ic_mino" != '?' ] && [ "$ic_got_ino" != "$ic_mino" ]; then
				echo "the handle of $ic_rel opened a different object: inode $ic_got_ino, was $ic_mino"
			elif [ "$ic_mmode" != '?' ] && [ "$(identity_ftype "${ic_got_mode:-0}")" != "$(identity_ftype "$ic_mmode")" ]; then
				echo "the handle of $ic_rel opened an object of another type: mode $ic_got_mode, was $ic_mmode"
			elif [ "$ic_bino" = '?' ]; then
				echo "the handle of $ic_rel opened inode $ic_got_ino, but there is no record of the backing object to compare with"
			elif [ "$ic_bok" != OK ] || [ "$ic_bgot_ino" != "$ic_bino" ] ||
				[ "$(identity_ftype "$ic_bgot_mode")" != "$(identity_ftype "$ic_bmode")" ]; then
				echo "the handle of $ic_rel opened inode $ic_got_ino, but the backing filesystem's own handle of it answers '$ic_ba' (was inode $ic_bino): an answer for something gone"
			fi
			;;
		*)
			echo "the handle of $ic_rel answered '$ic_ma', want the same object or ESTALE"
			;;
		esac
	done <"$ic_file.answers"
}
