# Shared helpers for the dcfs QEMU guest test scripts (test/qemu/guest/*.sh).
#
# Installed at /tests/lib.sh alongside every guest script (it matches the
# same guest/*.sh glob in test/qemu/BUILD.bazel's :initramfs genrule), but
# has no qemu_test target of its own, so it is never picked up as a test to
# run by itself; guest/init only ever runs the script named by dcfs_test=.
#
# Every guest script runs as /tests/<name>.sh (see guest/init), so
#   . "$(dirname "$0")/lib.sh"
# resolves the same way whether run directly or chrooted under a
# dcfs_rootfs= Debian tree (guest/init copies /tests/ into the chroot too).
#
# Callers are expected to set FAILED=0 before using pass/fail, and
# SRC/DB/MNT before using start_daemon/restart_daemon.

pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }
skip() { echo "TEST $1 SKIP ($2)"; }

is_mounted() { grep -q " $1 " /proc/mounts; }

# backing_fstype PATH: the filesystem type backing PATH -- "ext4", "xfs",
# "btrfs", or the raw hex magic (unrecognized) -- for guest scripts that run
# against all three backing filesystems (step 5.2: qemu_test_matrix) and
# need to branch on genuine per-filesystem semantics (generation/ACL/xattr/
# statx differences -- see README.md's "tested on" line and
# docs/conformance.md). Keyed on the statfs(2) magic number (busybox `stat
# -f -c %t`), not the type name `stat -f -c %T` prints: busybox/coreutils
# both print "ext2/ext3" for ext4's magic (0xef53 is shared by ext2/ext3/
# ext4; statfs(2) cannot tell them apart), which is useless for picking
# "ext4" back out by name.
backing_fstype() {
	case "$(stat -f -c %t "$1")" in
	ef53) echo ext4 ;;
	58465342) echo xfs ;;
	9123683e) echo btrfs ;;
	*) stat -f -c %t "$1" ;;
	esac
}

# sectors_read DEV: the "sectors read" counter from /sys/block/DEV/stat, to
# confirm a re-read was served from dcfs's own cache rather than the disk.
sectors_read() {
	read -r line <"/sys/block/$1/stat"
	set -- $line
	echo "$3"
}

drop_caches() {
	sync
	echo 3 >/proc/sys/vm/drop_caches
}

# start_daemon LOG [extra dcfs flags...]: starts dcfs against $SRC/$DB,
# mounted at $MNT, with any extra flags inserted before $MNT; waits up to
# 10s for the mount to appear. Sets DAEMON_PID and MOUNTED (1 if the mount
# appeared, 0 if the wait timed out) and returns 0/1 to match.
start_daemon() {
	log=$1
	shift
	"$DCFS" --source="$SRC" --cache_db="$DB" "$@" "$MNT" >"$log" 2>&1 &
	DAEMON_PID=$!
	MOUNTED=0
	i=0
	while [ "$i" -lt 10 ]; do
		if is_mounted "$MNT"; then
			MOUNTED=1
			return 0
		fi
		if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
			return 1
		fi
		i=$((i + 1))
		sleep 1
	done
	return 1
}

# restart_daemon NAME LOG: SIGTERMs the current daemon (DAEMON_PID), forces
# an unmount if it didn't clean up $MNT itself, then start_daemon's it back
# up with no extra flags; reports "$NAME-unmount"/"$NAME-mount".
restart_daemon() {
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		echo "$(basename "$0"): /mnt still mounted after SIGTERM; forcing umount"
		umount "$MNT" 2>/dev/null || true
	fi
	if is_mounted "$MNT"; then
		fail "$1-unmount" "mountpoint still mounted after kill+umount"
		MOUNTED=1
	else
		pass "$1-unmount"
		MOUNTED=0
	fi
	if start_daemon "$2"; then
		pass "$1-mount"
		return 0
	fi
	fail "$1-mount" "daemon did not remount within 10s"
	return 1
}
