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
# SRC/DB/MNT before using start_daemon/restart_daemon. dcfs is run as
# $MOUNT_DCFS (default /sbin/mount.dcfs, a link to the dcfs binary: it
# dispatches on argv[0]).

pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }
skip() { echo "TEST $1 SKIP ($2)"; }
# require_commands NAME...: FAILs the run (check guest-commands) naming
# every NAME that is not a command here. A script whose command is missing
# would otherwise only print "<name>: not found" and carry on: the guests'
# busybox has no usleep, and 34 test logs said so for weeks while every
# quiesce_daemon returned at once.
require_commands() {
	rc_missing=""
	for rc_name in "$@"; do
		command -v "$rc_name" >/dev/null 2>&1 || rc_missing="$rc_missing $rc_name"
	done
	if [ -n "$rc_missing" ]; then
		fail guest-commands "not in this guest:$rc_missing"
	fi
}

# require_no_reclaim NAME: FAILs the check NAME if the guest's kernel has
# reclaimed memory (kswapd or direct reclaim scanned any pages) since
# guest/init started its sampler (RECLAIM_BASE), else passes it. For a test
# whose meaning depends on nothing being evicted (inodes held by dcfs's
# descriptors, a FORGET that must come from the test's own action): reclaim
# sends FORGETs and drops caches nobody asked for, and MemAvailable does not
# show it. Call it at the end, over the whole run; the fix is the test's
# `mem=` (test/qemu/README.md, "Guest memory"). Step 23.7: destroy_test
# lost 30% of its held descriptors to it at 832 MiB.
require_no_reclaim() {
	rnr_now=$(awk '/^pgscan_(kswapd|direct) / { p += $2 } END { print p + 0 }' /proc/vmstat)
	rnr_base=${RECLAIM_BASE%% *}
	if [ "$rnr_now" -eq "${rnr_base:-0}" ]; then
		pass "$1"
	else
		fail "$1" "the kernel reclaimed memory during the run ($((rnr_now - ${rnr_base:-0})) pages scanned): cached inodes and pages were evicted; raise this test's mem="
	fi
}

# dmesg_oom_lines / dmesg_kernel_failures: the kernel log's OOM-killer lines as
# MEM-OOM: lines and its failures (oops, BUG, WARNING, call trace, panic) as
# KERNEL-OOPS: lines, which the host's verdict (run-qemu.sh) fails a boot on.
# The console runs at loglevel=3 and shows only the worst of these, so a boot
# that is killed before guest/init's own scan at its end (the first boot of a
# power cut, guest/fault_power.sh) prints them itself before it is cut. The
# patterns are guest/init's (mem_report), which has them for the guests with no
# lib.sh; //test/qemu:run_qemu_verdict_test checks that the two agree.
dmesg_oom_lines() {
	dmesg 2>/dev/null | grep -i -E 'out of memory|oom-kill|killed process' |
		head -n 10 | sed 's/^/MEM-OOM: /'
}
dmesg_kernel_failures() {
	dmesg 2>/dev/null | grep -E 'BUG:|Oops|kernel BUG at|WARNING: CPU:|WARNING: at |------------\[ cut here \]------------|Call Trace:|Kernel panic|general protection fault' |
		head -n 20 | sed 's/^/KERNEL-OOPS: /'
}

# snapshot DIR [atime]: one line per entry under DIR (lost+found left out),
# sorted: "<path> <type> <size> <mode> <links>" and, for a regular file, the
# md5sum of its contents. What two trees must agree on to be the same tree:
# dcfs's served one and the backing filesystem's (guest/fault_power.sh,
# guest/fault_ace.sh, guest/fault_freeze.sh, guest/fault_recover.sh).
#
# With "atime" (step 23.8), also every entry's access time to the nanosecond,
# except a directory's or a symlink's: dcfs serves files' from the backing
# filesystem, and stamps directories' and symlinks' in its own database only
# (README "Limitations"), so those are the only two left out of "served
# equals backing". The access time is read after the file's md5sum, so that
# it is the one that read gave: the snapshot of the backing tree, taken
# first, makes the relatime update, and the served tree's read of the same
# file then changes nothing. (The other columns come first, before any read:
# a read through dcfs reopens the file and so re-reads its attributes, which
# would hide an out-of-band change the comparison's self-checks make.) Not
# for a snapshot compared across a power cut: its own reads move access times
# the cut may then lose.
snapshot() {
	(cd "$1" && find . -path ./lost+found -prune -o -print | sort |
		while IFS= read -r sn_p; do
			sn_line=$(stat -c '%n %F %s %a %h' "$sn_p" 2>&1) || sn_line="$sn_p stat failed: $sn_line"
			sn_md5=""
			if [ -f "$sn_p" ] && [ ! -L "$sn_p" ]; then
				sn_md5=" $(md5sum "$sn_p" 2>&1 | cut -d' ' -f1)"
			fi
			if [ "$2" = atime ] && [ ! -L "$sn_p" ] && [ ! -d "$sn_p" ]; then
				sn_line="$sn_line atime=$(stat -c %x "$sn_p" 2>&1)"
			fi
			echo "$sn_line$sn_md5"
		done)
}

# The commands the guest scripts run, as busybox applets of the
# initramfs (/bin/busybox), checked whenever a script sources this file
# there. The
# dcfs_rootfs= Debian chroot (nfs_test) is a different set (no sed: its
# packages are not Debian's essential set, third_party/debian/README.md),
# so its script names what it runs itself.
GUEST_COMMANDS="awk basename cat chgrp chmod chown chroot cmp cp cut date dd
diff dirname dmesg find grep head ln ls md5sum mkdir mkfifo mknod mount mv
readlink rm rmdir sed sleep sort stat sync tail timeout touch tr truncate
umount uname uniq wc"
if [ -x /bin/busybox ]; then
	# shellcheck disable=SC2086 # one word per command
	require_commands $GUEST_COMMANDS
fi

# disabled NAME REASON CHECK [ARGS...]: a check that fails for a reason
# outside dcfs (a kernel limitation) and is kept, googletest-style, so the
# day it starts passing shows in the log. Runs CHECK ARGS (exit status 0:
# pass; its output is the detail) and reports "TEST DISABLED_NAME DISABLED
# (REASON)" either way, never failing the run, then its would-be verdict on
# the next line: "would PASS" or "would FAIL (detail)".
disabled() {
	d_name=$1
	d_reason=$2
	shift 2
	if d_detail=$("$@" 2>&1); then
		d_verdict="would PASS"
	else
		d_verdict="would FAIL"
	fi
	echo "TEST DISABLED_$d_name DISABLED ($d_reason)"
	echo "  $d_verdict${d_detail:+ ($d_detail)}"
}

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

# quiesce_backing: drains any backing-filesystem work the PREVIOUS mutation
# left running in the background before a zero-backing-reads baseline is
# taken. (Moved here from rename.sh; every zero-reads baseline uses it via
# drop_caches_quiesced, review L12.)
#
# xfs defers the on-disk half of removing an inode (freeing its extents and
# AG metadata -- "inode inactivation"/inodegc) to a background workqueue so
# that unlink/rmdir/a replacing rename don't have to wait for it; under
# heavy host load that workqueue can still be mid-run (reading AG metadata
# from vdb) when the baseline sectors_read is taken moments later, so the
# read lands inside the measured window and looks like a cache miss it is
# not (confirmed by reading fs/xfs/xfs_icache.c: xfs_inodegc_stop(), the
# only thing that drains it, runs nowhere except freeze and unmount).
# The same freeze also covers xfs's log synchronously: otherwise
# xfs_log_worker (every fs.xfs.xfssyncd_centisecs, 30 s) writes two dummy
# superblock transactions to the device, one per tick, the first 30-60 s
# after the last write (idle.sh measured +2 writes at ~34 s and +2 at ~65 s
# after boot, with and without dcfs running, and nothing after that).
# FIFREEZE forces exactly that drain (xfs_fs_freeze -> xfs_fs_sync_fs's
# SB_FREEZE_PAGEFAULT stage -> xfs_inodegc_stop) synchronously before it
# returns; FITHAW re-enables it with nothing left queued, so it cannot
# introduce a read of its own. Both ioctls are plain VFS freeze_super/
# thaw_super, supported the same way on ext4 and btrfs, so this runs
# unconditionally rather than branching on backing_fstype. Called before
# the baseline, not between it and the check, so a backing read genuinely
# caused by serving the check itself still falls inside the window and is
# still caught.
#
# A freeze that fails leaves nothing frozen (ignored, as before); a thaw
# that fails after a successful freeze leaves $SRC frozen and the next
# mutation would hang until the harness timeout, so that is a FAIL.
quiesce_backing() {
	if "${TESTUTIL:-/bin/testutil}" fsfreeze "$SRC" freeze >/dev/null 2>&1; then
		if ! "${TESTUTIL:-/bin/testutil}" fsfreeze "$SRC" thaw >/dev/null 2>&1; then
			fail quiesce-thaw "thaw of $SRC failed after a successful freeze; $SRC may still be frozen"
		fi
	fi
}

# drop_caches_quiesced: drop_caches; quiesce_backing; drop_caches. The
# second drop catches an inode released by the first (FORGET, then dcfs
# closes a backing fd) whose cleanup would otherwise land after the
# quiesce, inside the measured window. With a daemon running (DAEMON_PID),
# it then waits for the daemon to go quiet: the last FORGET of a file
# written during the run makes dcfs re-read its attributes (step 23.1, a
# statx that may read the backing device once its caches are dropped), and
# that must land before a zero-reads baseline, not inside the window.
drop_caches_quiesced() {
	drop_caches
	quiesce_backing
	drop_caches
	if [ -n "${DAEMON_PID:-}" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		quiesce_daemon "$DAEMON_PID"
	fi
}

# --- daemon activity probes (write.sh, passthrough.sh) -------------------
#
# Shared so every "the kernel, not dcfs, moved the bytes" check measures the
# same way (step 6.2: counting dcfs's FUSE wakeups, not its CPU ticks).

# utime (field 14) + stime (field 15) of pid $1, in clock ticks -- see
# proc(5).
cpu_ticks() {
	awk '{print $14 + $15}' "/proc/$1/stat"
}

# daemon_wakeups PID: sets WAKEUPS to the voluntary context switches of the
# (single-threaded) daemon so far -- one per FUSE request it blocks waiting
# for (see the write-large-passthrough comment in write.sh).
daemon_wakeups() {
	WAKEUPS=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "/proc/$1/status")
}

# daemon_fd_count [PID]: the open-descriptor count of the daemon (PID, by
# default $DAEMON_PID) right now.
daemon_fd_count() {
	ls "/proc/${1:-$DAEMON_PID}/fd" 2>/dev/null | wc -l
}

# quiesce_daemon PID: waits (up to 10s) until the daemon has had no wakeup
# for 0.3s. drop_caches makes the kernel send a FORGET for every cached inode
# and dentry, asynchronously and in batches, and opening a file makes dcfs
# commit to its SQLite database; each wakeup would otherwise be counted
# against the write that happens to be running then.
quiesce_daemon() {
	daemon_wakeups "$1"
	q_last=$WAKEUPS
	q_stable=0
	q_n=0
	while [ "$q_stable" -lt 3 ] && [ "$q_n" -lt 100 ]; do
		sleep 0.1
		daemon_wakeups "$1"
		if [ "$WAKEUPS" = "$q_last" ]; then
			q_stable=$((q_stable + 1))
		else
			q_stable=0
			q_last=$WAKEUPS
		fi
		q_n=$((q_n + 1))
	done
}

# dcfs_options [--flag[=value]...]: the -o string of a foreground
# dcfs.fstype=none mount of $DB, with each flag as the dcfs.<flag> option
# (phase 15: the plain --source command line is gone).
dcfs_options() {
	do_opts="dcfs.fstype=none,dcfs.cache_db=$DB,dcfs.foreground"
	for do_flag in "$@"; do
		do_opts="$do_opts,dcfs.${do_flag#--}"
	done
	echo "$do_opts"
}

# start_daemon LOG [extra dcfs flags...]: starts dcfs against $SRC/$DB,
# mounted at $MNT, with each extra flag as a dcfs.<flag> mount option; waits
# up to 10s for the mount to appear. Sets DAEMON_PID and MOUNTED (1 if the
# mount appeared, 0 if the wait timed out) and returns 0/1 to match. The
# daemon stays in the foreground (dcfs.foreground), so DAEMON_PID is dcfs.
start_daemon() {
	log=$1
	shift
	"${MOUNT_DCFS:-/sbin/mount.dcfs}" -o "$(dcfs_options "$@")" "$SRC" "$MNT" >"$log" 2>&1 &
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
