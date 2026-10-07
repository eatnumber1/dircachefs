# dcfs over fault-injectable disks (step 11.1), for the failure tests
# guest/fault_*.sh. Sourced after lib.sh and fault_lib.sh.
#
# Two disks, each behind its own dm device (fault_lib.sh): the backing
# filesystem (/dev/vdb, mounted at $SRC) and the cache database's (/dev/vdc,
# mounted at /cache). The scripts set DCFS, SRC, MNT, DB (and use TESTUTIL).
#
#   fd_setup              wrap both disks, mount both filesystems
#   fd_start LOG [flags]  start dcfs (no periodic sync point unless given
#                         --sync_interval_sec), as lib.sh's start_daemon
#   fd_crash              SIGKILL the daemon, lazy-unmount $MNT: no sync
#                         point, no clean-shutdown mark
#   fd_cut                a power cut: both disks drop every write from now on
#   fd_restore            umount both filesystems (their writes still dropped
#                         or failing), switch both disks healthy, mount them
#                         again (the journals replay): what came back is what
#                         had reached the disks
#   fd_freeze/fd_thaw NAME   FIFREEZE/FITHAW of the filesystem on a disk, to
#                         hold the daemon in a phase (a mutation blocks in its
#                         first write to a frozen filesystem)
#   fd_blocked PID        waits until PID (or any of its threads) sleeps
#                         uninterruptibly: it is held by a freeze
#   fd_recovered LOG      the number the recovery WARNING of the daemon's log
#                         names ("recovered N dirty"), 0 if there is none
#
# Why a cut is "drop writes from now on" on both disks at once: a power cut
# is one instant; what reached each disk before it stays and nothing after
# does, and the page caches and the daemon are gone. Dropping writes keeps
# the daemon and the guest's page caches alive, which is why fd_restore
# unmounts first (the unmount's own writes are dropped too) and the next
# mount reads only what the disks hold.

FD_BACK=fbacking
FD_CACHE=fcache
FD_BACK_DEV=/dev/vdb
FD_CACHE_DEV=/dev/vdc
CACHE_DIR=/cache

fd_setup() {
	fault_wrap "$FD_BACK" "$FD_BACK_DEV" || return 1
	fault_wrap "$FD_CACHE" "$FD_CACHE_DEV" || return 1
	mkdir -p "$SRC" "$CACHE_DIR" "$MNT"
	mount "$(fault_dev "$FD_BACK")" "$SRC" || return 1
	mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR" || return 1
}

fd_start() {
	fd_log=$1
	shift
	start_daemon "$fd_log" "$@"
}

fd_crash() {
	kill -KILL "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	umount -l "$MNT" 2>/dev/null || true
	MOUNTED=0
}

fd_cut() {
	fault_mode "$FD_BACK" drop-writes || return 1
	fault_mode "$FD_CACHE" drop-writes || return 1
}

# fd_umount_disks: both filesystems, however they are doing; the daemon must
# be gone (it holds the backing filesystem's descriptors).
fd_umount_disks() {
	umount "$SRC" 2>/dev/null || umount -l "$SRC" 2>/dev/null || true
	umount "$CACHE_DIR" 2>/dev/null || umount -l "$CACHE_DIR" 2>/dev/null || true
}

fd_restore() {
	fd_umount_disks
	fault_mode "$FD_BACK" healthy || return 1
	fault_mode "$FD_CACHE" healthy || return 1
	mount "$(fault_dev "$FD_BACK")" "$SRC" || return 1
	mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR" || return 1
}

fd_freeze() {
	case "$1" in
	back) fd_dir=$SRC ;;
	cache) fd_dir=$CACHE_DIR ;;
	esac
	"$TESTUTIL" fsfreeze "$fd_dir" freeze
}

fd_thaw() {
	case "$1" in
	back) fd_dir=$SRC ;;
	cache) fd_dir=$CACHE_DIR ;;
	esac
	"$TESTUTIL" fsfreeze "$fd_dir" thaw
}

# fd_blocked PID [PREFIX]: up to 10 s for the process to be held by a freeze:
# in uninterruptible sleep (state D) in a syscall whose descriptor names a path
# under PREFIX (if given), on three looks 0.2 s apart. A single look is not
# enough: a syncfs or an fsync waiting for its I/O is in state D for a few
# milliseconds too.
fd_blocked() {
	fb_n=0
	fb_seen=0
	while [ "$fb_n" -lt 50 ]; do
		if grep -q '^State:.*D (disk sleep)' "/proc/$1/status" 2>/dev/null &&
			case "$(fd_where "$1")" in
			*"fd -> $2"*) true ;;
			*) false ;;
			esac; then
			fb_seen=$((fb_seen + 1))
			[ "$fb_seen" -ge 3 ] && return 0
		else
			fb_seen=0
		fi
		sleep 0.2
		fb_n=$((fb_n + 1))
	done
	return 1
}

# fd_where PID: what PID is blocked in, "<syscall number> -> <path of the
# descriptor it names>" (informational: which write a freeze is holding).
fd_where() {
	fw_sys=$(cut -d' ' -f1,2 "/proc/$1/syscall" 2>/dev/null)
	fw_fd=$(echo "$fw_sys" | cut -d' ' -f2)
	case "$fw_fd" in
	0x*) fw_fd=$((fw_fd)) ;;
	esac
	echo "syscall ${fw_sys%% *}, fd -> $(readlink "/proc/$1/fd/$fw_fd" 2>/dev/null)"
}

fd_recovered() {
	fr_n=$(sed -n 's/.*recovered \([0-9][0-9]*\) dirty.*/\1/p' "$1" | tail -1)
	echo "${fr_n:-0}"
}
