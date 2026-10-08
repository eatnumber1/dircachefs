#!/bin/sh
# dcfs step 3.5 acceptance test: daemon lifecycle and CLI.
#
# Exercises main.cc's command-line validation, startup ordering (source
# opened before the cache database is touched, a foreign cache database is
# refused with an actionable message), -o passthrough (spelled --fuse_opt),
# clean SIGTERM shutdown (WAL checkpointed, mount gone, exit 0), mounting
# dcfs back over its own SOURCE, and restarting against a previously-used
# cache database.
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions.
#
# Run as /tests/lifecycle.sh by guest/init when booted with
# dcfs_test=lifecycle.sh; prints one "TEST ... PASS/FAIL/SKIP" line per
# check and exits nonzero if any check failed. init turns that into the
# final ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
. "$(dirname "$0")/lib.sh"

MOUNT_DCFS=/sbin/mount.dcfs
SRC=/src
MNT=/mnt

DAEMON_PID=""
DAEMON_LOG=""

# Runs on every exit (including one `set -e` triggers on an unguarded
# failing command -- guest/init invokes this script with plain `sh`, not
# `sh -e`, but every command below is still guarded (`|| true`) so a
# failure while cleaning up never masks, or cuts short, the cleanup
# itself). Dumps the daemon's log and force-kills/unmounts anything a
# failed check left behind.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		if [ -n "$DAEMON_LOG" ]; then
			echo "--- dcfs stderr (last daemon started) ---"
			cat "$DAEMON_LOG" 2>/dev/null
		fi
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	umount "$MNT" 2>/dev/null || true
	umount "$SRC" 2>/dev/null || true
}
trap cleanup EXIT

echo "lifecycle.sh: kernel $(uname -r)"

# --- helpers -------------------------------------------------------------

# Number of /proc/mounts lines whose mountpoint field is exactly $1. Used
# instead of a plain "is it mounted" grep because a couple of checks below
# mount dcfs *over* a directory ($SRC) that is already a mount point
# (vdb): what matters there is a new stacked entry appearing/disappearing,
# not whether $SRC matches at all.
mount_count() {
	awk -v mnt="$1" '$2 == mnt { n++ } END { print n + 0 }' /proc/mounts
}

# run_dcfs SOURCE DB MOUNTPOINT [--flag[=value]...]: the foreground
# dcfs.fstype=none form of mount.dcfs (phase 15), each flag a dcfs.<flag>
# option.
run_dcfs() {
	rd_source=$1
	rd_db=$2
	rd_mnt=$3
	shift 3
	rd_opts="dcfs.fstype=none,dcfs.cache_db=$rd_db,dcfs.foreground"
	for rd_flag in "$@"; do
		rd_opts="$rd_opts,dcfs.${rd_flag#--}"
	done
	# exec: always called in a subshell, which then is dcfs (so that
	# $! of a backgrounded call is its pid).
	exec "$MOUNT_DCFS" -o "$rd_opts" "$rd_source" "$rd_mnt"
}

# Starts the daemon in the background, logging its stderr to $1 and
# waiting up to 10s for a new entry to appear in /proc/mounts for
# mountpoint $2 (the remaining arguments are run_dcfs's: SOURCE DB
# MOUNTPOINT and flags). Leaves DAEMON_PID set either way;
# returns nonzero (without waiting further) if the mount didn't appear and
# the daemon has already exited.
start_daemon() {
	log=$1
	target=$2
	shift 2
	before=$(mount_count "$target")
	DAEMON_LOG="$log"
	run_dcfs "$@" >"$log" 2>&1 &
	DAEMON_PID=$!
	i=0
	while [ "$i" -lt 10 ]; do
		after=$(mount_count "$target")
		if [ "$after" -gt "$before" ]; then
			return 0
		fi
		if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
			wait "$DAEMON_PID" 2>/dev/null
			DAEMON_PID=""
			return 1
		fi
		i=$((i + 1))
		sleep 1
	done
	return 1
}

# Sends SIGTERM to the running daemon and waits for it to exit, storing its
# exit status in DAEMON_RC. Callers check mount_count themselves afterward
# (the expected post-count differs: 0 for a plain mountpoint, but back to
# whatever it was before for a mount-over-source target, which stays
# mounted -- just with one fewer stacked entry).
stop_daemon() {
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID"
	DAEMON_RC=$?
	DAEMON_PID=""
}

mkdir -p /cache /mnt /tmp/other

# --- shared backing tree: vdb (ext4) mounted at /src ---------------------

mount /dev/vdb /src

populate_tree() {
	root=$1
	mkdir -p "$root/d1/d2"
	i=0
	while [ "$i" -lt 10 ]; do
		echo "content $i in $root" >"$root/file_$i.txt"
		i=$((i + 1))
	done
	echo "deep content" >"$root/d1/d2/deep.txt"
	ln -s file_0.txt "$root/link_to_file"
}
populate_tree "$SRC"

stat_snapshot() {
	find "$1" -exec stat -c '%i %A %h %u %g %s %N' {} + | sort
}

# --- usage: no arguments at all ------------------------------------------

OUT=$("$MOUNT_DCFS" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q 'usage: mount.dcfs SOURCE MOUNTPOINT'; then
	pass usage
else
	fail usage "rc=$RC out=$OUT"
fi

# --- missing-source: SOURCE points nowhere ------------------------------

OUT=$(run_dcfs /nonexistent /cache/missing.db "$MNT" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q 'SOURCE /nonexistent'; then
	pass missing-source
else
	fail missing-source "rc=$RC out=$OUT"
fi
if [ "$(mount_count "$MNT")" -eq 0 ]; then
	pass missing-source-no-mount
else
	fail missing-source-no-mount "$MNT unexpectedly mounted"
	umount "$MNT" 2>/dev/null || true
fi

# --- source-not-dir: SOURCE is a regular file ----------------------------

echo not-a-directory >/tmp/regular-file
OUT=$(run_dcfs /tmp/regular-file /cache/notdir.db "$MNT" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q 'SOURCE /tmp/regular-file'; then
	pass source-not-dir
else
	fail source-not-dir "rc=$RC out=$OUT"
fi
if [ "$(mount_count "$MNT")" -eq 0 ]; then
	pass source-not-dir-no-mount
else
	fail source-not-dir-no-mount "$MNT unexpectedly mounted"
	umount "$MNT" 2>/dev/null || true
fi

# --- foreign-db: same cache db, source on a different filesystem ---------

DB_FOREIGN=/cache/foreign.db
if start_daemon /tmp/foreign1.log "$MNT" "$SRC" "$DB_FOREIGN" "$MNT"; then
	pass foreign-db-first-mount
	stop_daemon
	if [ "$DAEMON_RC" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass foreign-db-first-stop
	else
		fail foreign-db-first-stop "rc=$DAEMON_RC mount_count=$(mount_count "$MNT")"
	fi

	OUT=$(run_dcfs /tmp/other "$DB_FOREIGN" "$MNT" 2>&1)
	RC=$?
	if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q 'created for filesystem'; then
		pass foreign-db-refused
	else
		fail foreign-db-refused "rc=$RC out=$OUT"
	fi
	if [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass foreign-db-no-mount
	else
		fail foreign-db-no-mount "$MNT unexpectedly mounted"
		umount "$MNT" 2>/dev/null || true
	fi
else
	fail foreign-db-first-mount "daemon did not mount within 10s"
fi

# --- fuse-opt: --fuse_opt passthrough, good and bad options ---------------

DB_FUSEOPT=/cache/fuseopt.db
if start_daemon /tmp/fuseopt-good.log "$MNT" "$SRC" "$DB_FUSEOPT" "$MNT" \
		--fuse_opt=max_read=65536; then
	pass fuse-opt-good-mount
	if awk -v mnt="$MNT" '$2 == mnt { print $4 }' /proc/mounts | grep -q 'max_read=65536'; then
		pass fuse-opt-good-visible
	else
		fail fuse-opt-good-visible "max_read=65536 not in /proc/mounts options"
	fi
	stop_daemon
else
	fail fuse-opt-good-mount "daemon did not mount within 10s"
fi

# Under `timeout`: should a regression mount, the foreground daemon would
# otherwise keep the command substitution waiting until the guest's timeout.
# A refusal exits 1 (fuse_session_new failed); a run cut short by `timeout`
# exits otherwise, and fails.
OUT=$(timeout 10 "$MOUNT_DCFS" -o "dcfs.fstype=none,dcfs.cache_db=$DB_FUSEOPT,dcfs.foreground,dcfs.fuse_opt=bogus_option_xyz" "$SRC" "$MNT" 2>&1)
RC=$?
if [ "$RC" -eq 1 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
	pass fuse-opt-bad-rejected
else
	fail fuse-opt-bad-rejected "rc=$RC mount_count=$(mount_count "$MNT") out=$OUT"
	umount "$MNT" 2>/dev/null || true
fi

# default_permissions is dcfs's own and required: naming it is a usage
# error (exit 1, before anything is opened), not passed through twice.
OUT=$(timeout 10 "$MOUNT_DCFS" -o "dcfs.fstype=none,dcfs.cache_db=$DB_FUSEOPT,dcfs.foreground,dcfs.fuse_opt=suid,dcfs.fuse_opt=default_permissions" "$SRC" "$MNT" 2>&1)
RC=$?
if [ "$RC" -eq 1 ] && [ "$(mount_count "$MNT")" -eq 0 ] &&
	printf '%s\n' "$OUT" | grep -q "default_permissions is redundant"; then
	pass fuse-opt-default-permissions-rejected
else
	fail fuse-opt-default-permissions-rejected "rc=$RC mount_count=$(mount_count "$MNT") out=$OUT"
	umount "$MNT" 2>/dev/null || true
fi

# --- sigterm-clean: SIGTERM unmounts, exits 0, and checkpoints the WAL ---

DB_SIGTERM=/cache/sigterm.db
if start_daemon /tmp/sigterm.log "$MNT" "$SRC" "$DB_SIGTERM" "$MNT"; then
	pass sigterm-mount
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID"
	DAEMON_RC=$?
	DAEMON_PID=""
	if [ "$DAEMON_RC" -eq 0 ]; then
		pass sigterm-exit-zero
	else
		fail sigterm-exit-zero "exit status $DAEMON_RC"
	fi
	if [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass sigterm-unmounted
	else
		fail sigterm-unmounted "$MNT still mounted"
		umount "$MNT" 2>/dev/null || true
	fi
	WAL="$DB_SIGTERM-wal"
	if [ ! -e "$WAL" ]; then
		pass sigterm-wal-checkpointed
	else
		wal_size=$(stat -c %s "$WAL")
		if [ "$wal_size" -eq 0 ]; then
			pass sigterm-wal-checkpointed
		else
			fail sigterm-wal-checkpointed "$WAL is $wal_size bytes"
		fi
	fi
else
	fail sigterm-mount "daemon did not mount within 10s"
fi

# --- info-lines: the default threshold hides the lifecycle narrative -------
# (docs/design.md "Logging"): the SIGTERM run above ran at the default
# --stderrthreshold (WARNING) and must show none of it; a run with
# --stderrthreshold=0 shows the start line and the clean shutdown.

if grep -q 'starting: source=' /tmp/sigterm.log || grep -q 'shutdown: clean' /tmp/sigterm.log; then
	fail info-hidden-by-default "$(cat /tmp/sigterm.log)"
else
	pass info-hidden-by-default
fi
if start_daemon /tmp/info.log "$MNT" "$SRC" /cache/info.db "$MNT" --stderrthreshold=0; then
	stop_daemon
	if grep -q 'starting: source=/src cache_db=/cache/info.db mountpoint=/mnt' /tmp/info.log; then
		pass info-start-line
	else
		fail info-start-line "$(cat /tmp/info.log)"
	fi
	if grep -q 'shutdown: clean' /tmp/info.log; then
		pass info-shutdown-clean
	else
		fail info-shutdown-clean "$(cat /tmp/info.log)"
	fi
else
	fail info-mount "daemon did not mount within 10s"
fi

# --- mount-over-source: mountpoint IS SOURCE ----------------------------

BASE_SRC_COUNT=$(mount_count "$SRC") # 1: vdb, before any overmount.
stat_snapshot "$SRC" >/tmp/before_overmount.txt

DB_OVER=/cache/overmount.db
if start_daemon /tmp/overmount.log "$SRC" "$SRC" "$DB_OVER" "$SRC"; then
	pass mount-over-source-mount

	stat_snapshot "$SRC" >/tmp/after_overmount.txt
	set -- $(md5sum /tmp/before_overmount.txt)
	before_sum=$1
	set -- $(md5sum /tmp/after_overmount.txt)
	after_sum=$1
	if [ "$before_sum" = "$after_sum" ]; then
		pass mount-over-source-listing-matches
	else
		fail mount-over-source-listing-matches "stat listing changed across the overmount (before=$before_sum after=$after_sum)"
	fi

	if OUT=$(cat "$SRC/file_0.txt" 2>&1); then
		if [ "$OUT" = "content 0 in $SRC" ]; then
			pass mount-over-source-cat
		else
			fail mount-over-source-cat "unexpected content: $OUT"
		fi
	else
		if printf '%s' "$OUT" | grep -qi 'not implemented'; then
			skip mount-over-source-cat "Open/Read not implemented yet (ENOSYS): $OUT"
		else
			fail mount-over-source-cat "$OUT"
		fi
	fi

	stop_daemon
	if [ "$DAEMON_RC" -eq 0 ] && [ "$(mount_count "$SRC")" -eq "$BASE_SRC_COUNT" ]; then
		pass mount-over-source-unmount
	else
		fail mount-over-source-unmount "rc=$DAEMON_RC mount_count=$(mount_count "$SRC") want=$BASE_SRC_COUNT"
	fi

	if [ -f "$SRC/file_0.txt" ]; then
		pass mount-over-source-original-visible
	else
		fail mount-over-source-original-visible "$SRC/file_0.txt missing after unmount"
	fi
else
	fail mount-over-source-mount "daemon did not mount within 10s"
fi

# --- restart-same-db: reuse the overmount test's cache db, mounted at /mnt

if start_daemon /tmp/restart.log "$MNT" "$SRC" "$DB_OVER" "$MNT"; then
	pass restart-same-db-mount
	if find "$MNT" -exec stat -c '%i %n' {} + >/dev/null 2>/tmp/restart-find.err; then
		pass restart-same-db-listing
	else
		fail restart-same-db-listing "$(cat /tmp/restart-find.err)"
	fi
	stop_daemon
	if [ "$DAEMON_RC" -ne 0 ]; then
		fail restart-same-db-stop "exit status $DAEMON_RC"
	fi
else
	fail restart-same-db-mount "daemon did not mount within 10s"
fi

# --- io-uring-refused: FUSE-over-io_uring must not make dcfs multi-threaded
#
# libfuse turns on FUSE-over-io_uring by itself when FUSE_URING_ENABLE=1 is
# in the daemon's environment (or -o io_uring is passed) and the kernel
# offers it, and then runs the handlers on one thread per CPU. dcfs is
# single-threaded by design (audit-races F4), so it must refuse.

URING_PARAM=/sys/module/fuse/parameters/enable_uring
if [ -w "$URING_PARAM" ] && echo Y >"$URING_PARAM" 2>/dev/null; then
	export FUSE_URING_ENABLE=1
	if start_daemon /tmp/uring.log "$MNT" "$SRC" /cache/uring.db "$MNT"; then
		unset FUSE_URING_ENABLE
		ls -l "$MNT" >/dev/null 2>&1
		stat "$MNT/file_0.txt" >/dev/null 2>&1
		threads=$(ls "/proc/$DAEMON_PID/task" | wc -l)
		if [ "$threads" -eq 1 ]; then
			pass io-uring-single-threaded
		else
			fail io-uring-single-threaded "dcfs has $threads threads"
		fi
		stop_daemon
	else
		unset FUSE_URING_ENABLE
		fail io-uring-mount "daemon did not mount within 10s"
	fi
	echo N >"$URING_PARAM" 2>/dev/null || true
else
	skip io-uring-single-threaded "kernel has no fuse enable_uring parameter"
fi

# --- other-source-dir-refused: same filesystem, different SOURCE dir ----
#
# The cache database belongs to one source directory, not just one
# filesystem (audit-crash F4): restarting it with SOURCE pointing at
# another directory on the same filesystem must be refused, not serve the
# first directory's cached tree under the second's name.
mkdir -p "$SRC/root_a" "$SRC/root_b"
echo a >"$SRC/root_a/only_in_a"
echo b >"$SRC/root_b/only_in_b"
DB_ROOTS=/cache/roots.db
if start_daemon /tmp/roots1.log "$MNT" "$SRC/root_a" "$DB_ROOTS" "$MNT"; then
	ls "$MNT" >/dev/null
	stop_daemon
	if start_daemon /tmp/roots2.log "$MNT" "$SRC/root_b" "$DB_ROOTS" "$MNT"; then
		fail other-source-dir-refused "mounted; listing: $(ls "$MNT" 2>&1)"
		stop_daemon
	else
		pass other-source-dir-refused
		if grep -q 'different source directory' /tmp/roots2.log; then
			pass other-source-dir-message
		else
			fail other-source-dir-message "log: $(cat /tmp/roots2.log)"
		fi
	fi
else
	fail other-source-dir-first-mount "daemon did not mount within 10s"
fi

# --- db-in-use-refused: two daemons must not share one cache database ----
#
# Each daemon's three-phase mutations and in-memory state (writable opens,
# fill guards) assume it is the only writer (audit-crash F7), so a second
# daemon started on a database another one is using must refuse to start.
DB_SHARED=/cache/shared.db
mkdir -p /tmp/mnt2
if start_daemon /tmp/shared1.log "$MNT" "$SRC" "$DB_SHARED" "$MNT"; then
	FIRST_PID=$DAEMON_PID
	if start_daemon /tmp/shared2.log /tmp/mnt2 "$SRC" "$DB_SHARED" /tmp/mnt2; then
		fail db-in-use-refused "a second daemon mounted /tmp/mnt2 on the same database"
		stop_daemon
	else
		pass db-in-use-refused
		if grep -q 'in use' /tmp/shared2.log; then
			pass db-in-use-message
		else
			fail db-in-use-message "log: $(cat /tmp/shared2.log)"
		fi
	fi
	DAEMON_PID=$FIRST_PID
	stop_daemon
	# Once the first daemon is gone, the database is free again.
	if start_daemon /tmp/shared3.log "$MNT" "$SRC" "$DB_SHARED" "$MNT"; then
		pass db-free-after-exit
		stop_daemon
	else
		fail db-free-after-exit "log: $(cat /tmp/shared3.log)"
	fi
else
	fail db-in-use-first-mount "daemon did not mount within 10s"
fi

exit "$FAILED"
