#!/bin/sh
# dcfs step 3.5 acceptance test: daemon lifecycle and CLI.
#
# Exercises main.cc's command-line validation, startup ordering (source
# opened before the cache database is touched, a foreign cache database is
# refused with an actionable message), -o passthrough (spelled --fuse_opt),
# clean SIGTERM shutdown (WAL checkpointed, mount gone, exit 0), mounting
# dcfs back over its own --source, and restarting against a previously-used
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
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }
skip() { echo "TEST $1 SKIP ($2)"; }

DCFS=/bin/dcfs
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

# Starts the daemon in the background, logging its stderr to $1 and
# waiting up to 10s for a new entry to appear in /proc/mounts for
# mountpoint $2 (the remaining arguments are dcfs's own argv, i.e.
# everything after the program name). Leaves DAEMON_PID set either way;
# returns nonzero (without waiting further) if the mount didn't appear and
# the daemon has already exited.
start_daemon() {
	log=$1
	target=$2
	shift 2
	before=$(mount_count "$target")
	DAEMON_LOG="$log"
	"$DCFS" "$@" >"$log" 2>&1 &
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
	find "$1" -exec stat -c '%i %A %h %U %G %s %N' {} + | sort
}

# --- usage: no arguments at all ------------------------------------------

OUT=$("$DCFS" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q -- '--source'; then
	pass usage
else
	fail usage "rc=$RC out=$OUT"
fi

# --- missing-source: --source points nowhere ------------------------------

OUT=$("$DCFS" --source=/nonexistent --cache_db=/cache/missing.db "$MNT" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q -- '--source=/nonexistent'; then
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

# --- source-not-dir: --source is a regular file ----------------------------

echo not-a-directory >/tmp/regular-file
OUT=$("$DCFS" --source=/tmp/regular-file --cache_db=/cache/notdir.db "$MNT" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q -- '--source=/tmp/regular-file'; then
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
if start_daemon /tmp/foreign1.log "$MNT" --source="$SRC" --cache_db="$DB_FOREIGN" "$MNT"; then
	pass foreign-db-first-mount
	stop_daemon
	if [ "$DAEMON_RC" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass foreign-db-first-stop
	else
		fail foreign-db-first-stop "rc=$DAEMON_RC mount_count=$(mount_count "$MNT")"
	fi

	OUT=$("$DCFS" --source=/tmp/other --cache_db="$DB_FOREIGN" "$MNT" 2>&1)
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
if start_daemon /tmp/fuseopt-good.log "$MNT" --source="$SRC" --cache_db="$DB_FUSEOPT" \
		--fuse_opt=max_read=65536 "$MNT"; then
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

OUT=$("$DCFS" --source="$SRC" --cache_db="$DB_FUSEOPT" \
	--fuse_opt=bogus_option_xyz "$MNT" 2>&1)
RC=$?
if [ "$RC" -ne 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
	pass fuse-opt-bad-rejected
else
	fail fuse-opt-bad-rejected "rc=$RC mount_count=$(mount_count "$MNT") out=$OUT"
	umount "$MNT" 2>/dev/null || true
fi

# --- sigterm-clean: SIGTERM unmounts, exits 0, and checkpoints the WAL ---

DB_SIGTERM=/cache/sigterm.db
if start_daemon /tmp/sigterm.log "$MNT" --source="$SRC" --cache_db="$DB_SIGTERM" "$MNT"; then
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

# --- mount-over-source: mountpoint IS --source ----------------------------

BASE_SRC_COUNT=$(mount_count "$SRC") # 1: vdb, before any overmount.
stat_snapshot "$SRC" >/tmp/before_overmount.txt

DB_OVER=/cache/overmount.db
if start_daemon /tmp/overmount.log "$SRC" --source="$SRC" --cache_db="$DB_OVER" "$SRC"; then
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

if start_daemon /tmp/restart.log "$MNT" --source="$SRC" --cache_db="$DB_OVER" "$MNT"; then
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
	if start_daemon /tmp/uring.log "$MNT" --source="$SRC" --cache_db=/cache/uring.db "$MNT"; then
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

# --- other-source-dir-refused: same filesystem, different --source dir ----
#
# The cache database belongs to one source directory, not just one
# filesystem (audit-crash F4): restarting it with --source pointing at
# another directory on the same filesystem must be refused, not serve the
# first directory's cached tree under the second's name.
mkdir -p "$SRC/root_a" "$SRC/root_b"
echo a >"$SRC/root_a/only_in_a"
echo b >"$SRC/root_b/only_in_b"
DB_ROOTS=/cache/roots.db
if start_daemon /tmp/roots1.log "$MNT" --source="$SRC/root_a" --cache_db="$DB_ROOTS" "$MNT"; then
	ls "$MNT" >/dev/null
	stop_daemon
	if start_daemon /tmp/roots2.log "$MNT" --source="$SRC/root_b" --cache_db="$DB_ROOTS" "$MNT"; then
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

exit "$FAILED"
