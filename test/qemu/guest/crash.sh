#!/bin/sh
# dcfs step 4.6 acceptance test: crash safety of passthrough writes, and
# out-of-band change detection.
#
# With FUSE passthrough the kernel writes straight to dcfs's backing fd, so
# dcfs never sees those writes. A writable open (or create) is therefore
# phase 1 of a write-through mutation: it marks the inode's cached
# attributes unknown before the open is replied to, and they stay unknown
# (served by a statx of the already-open backing fd) until the last
# writable open is released. This test kills the daemon with SIGKILL while
# a file opened for appending, and a file just created, both have written
# data and are still open -- with no close() of any descriptor for them
# since, so no FLUSH/RELEASE ever told dcfs about the writes (hence
# `testutil writehold`, not shell redirections) -- restarts it against the same cache database,
# and requires the sizes/mtimes it then reports to match the backing
# files -- before step 4.6 it served the pre-write attributes as current.
# A negative control shows the live daemon already reports a still-open
# file's grown size (the open-fd statx path), and a warm pass shows files
# nobody wrote to are still served with zero backing reads.
#
# It then changes a backing file out of band (unsupported, but detected
# when a syscall dcfs makes anyway reveals it): opening it through dcfs
# must show the new content, log exactly one "out-of-band" WARNING, and
# update the cached size. Finally, a batch of ordinary mutations made
# through dcfs, followed by opens of everything they touched, must log no
# "out-of-band" warning at all (no false positives from dcfs's own writes).
#
# Uses only busybox applets/options plus //tools:testutil for xattrs.
#
# Run as /tests/crash.sh by guest/init when booted with dcfs_test=crash.sh;
# prints one "TEST ... PASS/FAIL" line per check and exits nonzero if any
# check failed. init turns that into the final ALL-TESTS-PASSED /
# TEST-FAILED verdict.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
FHTEST=/bin/fhtest

is_mounted() { grep -q " $1 " /proc/mounts; }
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0
HOLDERS=""
HOLD_N=0

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	for pid in $HOLDERS; do
		kill -KILL "$pid" 2>/dev/null || true
	done
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (first run) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (second run) ---"
		cat "$LOG2" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "crash.sh: kernel $(uname -r)"

# Field 3 of /sys/block/<dev>/stat: cumulative sectors read since boot.
sectors_read() {
	read -r line <"/sys/block/$1/stat"
	set -- $line
	echo "$3"
}

drop_caches() {
	sync
	echo 3 >/proc/sys/vm/drop_caches
}

start_daemon() {
	"$DCFS" --source="$SRC" --cache_db="$DB" "$MNT" >"$1" 2>&1 &
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

# hold PATH append|create NBYTES: starts `testutil writehold` in the
# background (its pid in HOLDER_PID) and waits until it has written.
hold() {
	HOLD_N=$((HOLD_N + 1))
	out=/tmp/hold.$HOLD_N
	"$TESTUTIL" writehold "$1" "$2" "$3" >"$out" 2>&1 &
	HOLDER_PID=$!
	HOLDERS="$HOLDERS $HOLDER_PID"
	i=0
	while [ "$i" -lt 10 ]; do
		grep -q READY "$out" 2>/dev/null && return 0
		kill -0 "$HOLDER_PID" 2>/dev/null || break
		i=$((i + 1))
		sleep 1
	done
	echo "crash.sh: writehold $* did not get ready: $(cat "$out")"
	return 1
}

# unhold PID: kills a writehold (closing its fd).
unhold() {
	kill -KILL "$1" 2>/dev/null || true
	wait "$1" 2>/dev/null || true
}

# out_of_band_count LOG: the number of "out-of-band" lines in LOG.
out_of_band_count() {
	grep -c "out-of-band" "$1" 2>/dev/null || true
}

# check_same NAME FILE: FILE's size and mtime (seconds) through /mnt equal
# the backing file's.
check_same() {
	src_s=$(stat -c %s "$SRC/$2")
	mnt_s=$(stat -c %s "$MNT/$2")
	src_y=$(stat -c %Y "$SRC/$2")
	mnt_y=$(stat -c %Y "$MNT/$2")
	if [ "$src_s" = "$mnt_s" ]; then
		pass "$1-size"
	else
		fail "$1-size" "src=$src_s mnt=$mnt_s"
	fi
	if [ "$src_y" = "$mnt_y" ]; then
		pass "$1-mtime"
	else
		fail "$1-mtime" "src=$src_y mnt=$mnt_y"
	fi
}

# --- build the backing tree (before dcfs ever sees it) ---------------------

mount /dev/vdb /src
head -c 1000 /dev/zero >/src/f
echo hello >/src/h
mkdir /src/d
mkdir /src/e
echo one >/src/d/u1
echo two >/src/d/u2
echo three >/src/u3
# Old mtimes, so a write "now" is guaranteed to change %Y.
touch -d "2001-09-09 01:46:40" /src/f /src/h
sync

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# Populate the cache: every name, every attribute, all marked current.
find "$MNT" -exec stat -c '%s %Y' {} + >/dev/null
check_same warm-before f

# --- crash with writes outstanding ------------------------------------------

# A writable open of an existing file and a create, both written through and
# both still open (never closed, so never flushed) when the daemon dies.
hold "$MNT/f" append 4096 || fail crash-hold-f "writehold failed"
HOLD_F=$HOLDER_PID
hold "$MNT/g" create 6 || fail crash-hold-g "writehold failed"
HOLD_G=$HOLDER_PID
if [ "$(stat -c %s "$SRC/f")" = 5096 ] && [ "$(stat -c %s "$SRC/g")" = 6 ]; then
	pass crash-writes-landed-src
else
	fail crash-writes-landed-src "src sizes f=$(stat -c %s "$SRC/f") g=$(stat -c %s "$SRC/g")"
fi

# Audit F6: an NFS-style handle of directory e, and a create in e just
# before the crash, so that recovery forgets e's own dentry (e is dirty):
# reconnecting the handle afterwards needs e's ".." from dcfs.
E_HANDLE=$("$FHTEST" handle "$MNT/e") || E_HANDLE=""
: >"$MNT/e/x"

kill -KILL "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
umount -l "$MNT" 2>/dev/null || true
unhold "$HOLD_F"
unhold "$HOLD_G"
MOUNTED=0
if is_mounted "$MNT"; then
	fail crash-unmount "mountpoint still mounted after kill -9 + umount -l"
	exit "$FAILED"
fi
pass crash-unmount

n=$(out_of_band_count "$LOG1")
if [ "$n" = 0 ]; then
	pass crash-no-out-of-band-first-run
else
	fail crash-no-out-of-band-first-run "$n out-of-band warnings"
fi

if start_daemon "$LOG2"; then
	pass crash-restart
else
	fail crash-restart "daemon did not remount within 10s"
	exit "$FAILED"
fi

# Opening e by handle: the kernel reconnects the disconnected directory
# through LOOKUP(e, ".."), which must resolve although e's own dentry is
# unknown after recovery (then read() of a directory fails EISDIR: it was
# opened).
# Before 4.13 this was ENOENT from dcfs, ESTALE to the caller.
set -- $E_HANDLE
res=$("$FHTEST" open "$MNT" "$1" "$3" 2>&1) || true
# (fhtest names only some errnos; others print strerror.)
if [ "$res" = "ERR EISDIR" ] || [ "$res" = "ERR Is a directory" ]; then
	pass crash-dir-handle-reconnects
else
	fail crash-dir-handle-reconnects "handle '$E_HANDLE' -> '$res' (want ERR EISDIR)"
fi

# The attributes cached before the crash must not be served as current.
check_same crash-open-file f
check_same crash-created-file g
if [ "$(cat "$MNT/g")" = xxxxxx ]; then
	pass crash-created-file-content
else
	fail crash-created-file-content "got '$(cat "$MNT/g")'"
fi

# --- negative control: a live writer's growth is visible while still open ---

# Nothing has flushed or released the writer's open when the size is asked
# for, so only a statx of the open backing fd can answer correctly.
hold "$MNT/f" append 100 || fail live-hold "writehold failed"
open_s=$(stat -c %s "$MNT/f")
src_s=$(stat -c %s "$SRC/f")
if [ "$open_s" = 5196 ] && [ "$open_s" = "$src_s" ]; then
	pass live-open-size
else
	fail live-open-size "mnt=$open_s src=$src_s (want 5196)"
fi
unhold "$HOLDER_PID"
check_same live-closed f

# --- untouched files are still served warm (zero backing reads) -------------

drop_caches
before=$(sectors_read vdb)
warm_ok=1
[ "$(stat -c %s "$MNT/d/u1")" = 4 ] || warm_ok=0
[ "$(stat -c %s "$MNT/d/u2")" = 4 ] || warm_ok=0
[ "$(stat -c %s "$MNT/u3")" = 6 ] || warm_ok=0
ls -l "$MNT" "$MNT/d" >/dev/null || warm_ok=0
after=$(sectors_read vdb)
if [ "$warm_ok" = 1 ] && [ "$before" = "$after" ]; then
	pass warm-untouched
else
	fail warm-untouched "ok=$warm_ok sectors_read(vdb) $before -> $after"
fi

# --- out-of-band change: detected on open, logged once, adopted -------------

[ "$(stat -c %s "$MNT/h")" = 6 ] || fail out-of-band-before "h is not 6 bytes"
echo extra >>"$SRC/h"
got=$(cat "$MNT/h")
want=$(printf 'hello\nextra')
if [ "$got" = "$want" ]; then
	pass out-of-band-content
else
	fail out-of-band-content "got '$got'"
fi
n=$(out_of_band_count "$LOG2")
if [ "$n" = 1 ]; then
	pass out-of-band-logged-once
else
	fail out-of-band-logged-once "$n out-of-band warnings, want 1"
fi
grep "out-of-band" "$LOG2" || true
# The kernel's own attribute cache is not invalidated (see the README's
# "Coherence"); once it lets go of the inode, the size it relearns from
# dcfs's cache is the new one.
drop_caches
if [ "$(stat -c %s "$MNT/h")" = 12 ]; then
	pass out-of-band-size-adopted
else
	fail out-of-band-size-adopted "got $(stat -c %s "$MNT/h"), want 12"
fi

# --- no false positives from dcfs's own mutations ---------------------------

mkdir "$MNT/d2"
echo a >"$MNT/d2/a"
ln "$MNT/d2/a" "$MNT/d2/b"
mv "$MNT/d2/b" "$MNT/c"
chmod 600 "$MNT/d2/a"
"$TESTUTIL" setxattr "$MNT/d2/a" user.x 1 >/dev/null
"$TESTUTIL" setxattr "$MNT/d" user.y 2 >/dev/null
"$TESTUTIL" removexattr "$MNT/d2/a" user.x >/dev/null
echo more >>"$MNT/d2/a"
touch -d "2001-09-09 01:46:40" "$MNT/d/u1"
mv "$MNT/d/u2" "$MNT/d2/u2"
mkdir "$MNT/d/sub"
mv "$MNT/d/sub" "$MNT/d2/sub"
rm "$MNT/c"
rmdir "$MNT/d2/sub"
# Opens (OpenNode + its identity/attribute check) of everything touched:
# file contents, and a create in each directory (which opens its parent).
drop_caches
find "$MNT" -type f -exec cat {} + >/dev/null
touch "$MNT/z" "$MNT/d/z" "$MNT/d2/z"
rm "$MNT/d/z"
n=$(out_of_band_count "$LOG2")
if [ "$n" = 1 ]; then
	pass no-false-positives
else
	fail no-false-positives "$n out-of-band warnings, want only the 1 from h"
fi

exit "$FAILED"
