#!/bin/sh
# dcfs step 3.3 acceptance test: file contents served via FUSE passthrough
# (falling back to dcfs's own Read() when the kernel doesn't grant it).
#
# Builds small.txt (known content), a 64 MiB big.bin (from /dev/urandom),
# and a 200-file, 3-level tree on vdb; mounts dcfs over it; and checks:
# content read through dcfs matches the backing files (both a small cat
# and a 64 MiB md5sum); reading big.bin's *content* -- unlike metadata --
# does move vdb's block-read counter (a sanity check that this test can
# tell the difference); a second metadata-only pass causes no further
# reads; the CPU time dcfs's own process consumes while a 64 MiB read
# happens stays under 20 clock ticks (200ms), which is only possible if
# the kernel is reading the backing file directly rather than routing the
# data through us; a non-read-only open succeeds and its write lands on the
# backing filesystem (step 4.4 -- see write_test / guest/write.sh for the
# full write-through test; this file only keeps a minimal smoke check so
# passthrough opens of every access mode stay exercised here too); 200
# open/read/release cycles leave no fd leak in the daemon; and all of this
# still works after killing and restarting the daemon against the same
# cache database.
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions.
#
# Run as /tests/passthrough.sh by guest/init when booted with
# dcfs_test=passthrough.sh; prints one "TEST ... PASS/FAIL" line per check
# and exits nonzero if any check failed. init turns that into the final
# ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/bin/dcfs

# busybox on Ubuntu lacks the mountpoint applet; ask the kernel directly.
is_mounted() { grep -q " $1 " /proc/mounts; }
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log
SMALL_CONTENT="dcfs passthrough test file"

DAEMON_PID=""
MOUNTED=0

# See readonly.sh for why every command here is `|| true`-guarded: this
# must run to completion (and dump both daemon logs on any failure)
# whether the script is exiting via a tracked fail() or via `set -e`
# (imposed by init's `sh -e`) aborting on something unexpected.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (first run) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (second run) ---"
		cat "$LOG2" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "passthrough.sh: kernel $(uname -r)"

# --- helpers -----------------------------------------------------------

# Field 3 of /sys/block/<dev>/stat is the cumulative count of sectors read
# from that block device since boot -- see Documentation/ABI/stable/
# sysfs-block.
sectors_read() {
	read -r line <"/sys/block/$1/stat"
	set -- $line
	echo "$3"
}

# utime (field 14) + stime (field 15) of pid $1, in clock ticks -- see
# proc(5). dcfs's comm ("dcfs") has no spaces or parens, so splitting the
# line on plain whitespace is safe.
cpu_ticks() {
	awk '{print $14 + $15}' "/proc/$1/stat"
}

drop_caches() {
	sync
	echo 3 >/proc/sys/vm/drop_caches
}

# Starts the daemon, logging its stderr to $1, and waits up to 10s for the
# mount to appear. Returns nonzero (and leaves MOUNTED=0) if it doesn't.
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

# 200 files spread three levels deep under $1, for the open-many check.
populate_tree() {
	root=$1
	mkdir -p "$root/d1/d2/d3"
	i=0
	while [ "$i" -lt 200 ]; do
		case $((i % 3)) in
		0) f="$root/file_$i.txt" ;;
		1) f="$root/d1/file_$i.txt" ;;
		*) f="$root/d1/d2/d3/file_$i.txt" ;;
		esac
		echo "tree content $i" >"$f"
		i=$((i + 1))
	done
}

# --- build the backing tree ---------------------------------------------

mount /dev/vdb /src
printf '%s' "$SMALL_CONTENT" >"$SRC/small.txt"
dd if=/dev/urandom of="$SRC/big.bin" bs=1M count=64 2>/dev/null
populate_tree "$SRC/tree"
sync

set -- $(md5sum "$SRC/big.bin")
src_big_md5=$1

# --- mount dcfs ----------------------------------------------------------

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# --- content checks -------------------------------------------------------

content=$(cat "$MNT/small.txt")
if [ "$content" = "$SMALL_CONTENT" ]; then
	pass cat-matches
else
	fail cat-matches "got '$content'"
fi

set -- $(md5sum "$MNT/big.bin")
mnt_big_md5=$1
if [ "$mnt_big_md5" = "$src_big_md5" ]; then
	pass md5-matches
else
	fail md5-matches "src=$src_big_md5 mnt=$mnt_big_md5"
fi

# --- a content read, unlike metadata, must hit the backing device --------

drop_caches
before=$(sectors_read vdb)
cat "$MNT/big.bin" >/dev/null
after=$(sectors_read vdb)
delta=$((after - before))
if [ "$delta" -ge 100000 ]; then
	pass content-read-hits-disk
else
	fail content-read-hits-disk "sectors_read(vdb) delta=$delta, want >= 100000"
fi

# --- metadata must still come entirely from dcfs's cache ------------------

drop_caches
find "$MNT" -exec stat -c '%i %s %n' {} + >/tmp/stat_pass1.txt
before=$(sectors_read vdb)
find "$MNT" -exec stat -c '%i %s %n' {} + >/tmp/stat_pass2.txt
after=$(sectors_read vdb)
if [ "$after" = "$before" ]; then
	pass metadata-still-zero
else
	fail metadata-still-zero "sectors_read(vdb) $before -> $after between passes"
fi

# --- dcfs's own CPU time proves the kernel, not dcfs, moved the bytes ----

drop_caches
before_ticks=$(cpu_ticks "$DAEMON_PID")
dd if="$MNT/big.bin" of=/dev/null bs=1M 2>/dev/null
after_ticks=$(cpu_ticks "$DAEMON_PID")
tick_delta=$((after_ticks - before_ticks))
if [ "$tick_delta" -lt 20 ]; then
	pass passthrough-active
else
	echo "passthrough.sh: dcfs consumed $tick_delta clock ticks reading 64 MiB (want < 20)"
	fail passthrough-active "cpu ticks delta=$tick_delta"
fi

# --- step 4.4: writes are now allowed -- a non-read-only open must SUCCEED
# and land on the backing filesystem (this replaces the EROFS check step
# 3.3-4.3 had here, back when Open() refused anything but a read-only open;
# see write_test / guest/write.sh for the full write-through test) ----------

write_ok=0
if echo x >"$MNT/small.txt" 2>/dev/null; then write_ok=1; fi
after_write=$(cat "$SRC/small.txt")
if [ "$write_ok" -eq 1 ] && [ "$after_write" = x ]; then
	pass write-open-succeeds
else
	fail write-open-succeeds "write_ok=$write_ok content='$after_write'"
fi
# Restore small.txt (through /mnt, like the write that changed it -- dcfs
# has exclusive access to the backing tree, so this must not touch /src
# directly) so every check below it still sees the original content.
printf '%s' "$SMALL_CONTENT" >"$MNT/small.txt"

# --- 200 open/read/release cycles must not leak fds -----------------------

before_fds=$(ls "/proc/$DAEMON_PID/fd" | wc -l)
find "$MNT/tree" -type f -exec cat {} + >/dev/null
after_fds=$(ls "/proc/$DAEMON_PID/fd" | wc -l)
fd_delta=$((after_fds - before_fds))
if [ "$fd_delta" -le 2 ] && [ "$fd_delta" -ge -2 ]; then
	pass open-many
else
	fail open-many "fd count $before_fds -> $after_fds"
fi

# --- restart with the same cache db: content must still be readable ------

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if is_mounted "$MNT"; then
	# libfuse's signal handler should have unmounted on its own; fall back
	# to forcing it so the restart below isn't blocked by a stale mount.
	echo "passthrough.sh: /mnt still mounted after SIGTERM; forcing umount"
	umount "$MNT" 2>/dev/null || true
fi
if is_mounted "$MNT"; then
	fail restart-unmount "mountpoint still mounted after kill+umount"
	MOUNTED=1
else
	pass restart-unmount
	MOUNTED=0
fi

if start_daemon "$LOG2"; then
	pass restart-mount
else
	fail restart-mount "daemon did not remount within 10s"
	exit "$FAILED"
fi

content=$(cat "$MNT/small.txt")
if [ "$content" = "$SMALL_CONTENT" ]; then
	pass restart
else
	fail restart "got '$content'"
fi

exit "$FAILED"
