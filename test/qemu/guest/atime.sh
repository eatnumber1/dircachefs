#!/bin/sh
# dcfs step 23.3: access times follow the backing mount's rule (relatime).
#
# Reads go through passthrough, so dcfs never sees them; the backing
# filesystem updates the file's atime for them. An open for reading makes
# dcfs record the atime the rule gives (older than mtime or ctime, or a day
# old: now), from the cache alone, so a stat after a read reports what the
# backing filesystem has (within the second between the open and the read)
# and costs no backing read; a second read within the day changes neither.
#
# Run as /tests/atime.sh by guest/init when booted with dcfs_test=atime.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		cat "$LOG" 2>/dev/null
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

echo "atime.sh: kernel $(uname -r)"

# close_to NAME A B: |A - B| <= 2 (seconds).
close_to() {
	d=$(($2 - $3))
	if [ "$d" -le 2 ] && [ "$d" -ge -2 ]; then
		pass "$1"
	else
		fail "$1" "$2 and $3 differ by $d s"
	fi
}

expect_eq() {
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1" "want '$2', got '$3'"
	fi
}

mount /dev/vdb /src
grep " /src " /proc/mounts
now=$(date +%s)
echo old >/src/old
echo recent >/src/recent
# old: atime and mtime two days ago. recent: mtime two hours ago, atime in
# an hour (after mtime and after the ctime that setting the times makes
# now, within the day: relatime leaves it).
"$TESTUTIL" utimes2 /src/old $((now - 172800)) $((now - 172800))
"$TESTUTIL" utimes2 /src/recent $((now + 3600)) $((now - 7200))
sync

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
stat "$MNT/old" "$MNT/recent" >/dev/null
recent_before=$(stat -c %X "$MNT/recent")

cat "$MNT/old" "$MNT/recent" >/dev/null
drop_caches_quiesced
before=$(sectors_read vdb)
old_mnt=$(stat -c %X "$MNT/old")
recent_mnt=$(stat -c %X "$MNT/recent")
after=$(sectors_read vdb)
expect_eq atime-from-cache "$before" "$after"
close_to atime-after-read "$(stat -c %X /src/old)" "$old_mnt"
close_to atime-is-now "$(date +%s)" "$old_mnt"
expect_eq atime-recent-kept "$recent_before" "$recent_mnt"
expect_eq atime-recent-backing "$(stat -c %X /src/recent)" "$recent_mnt"

# Within the day, after mtime: a second read changes neither.
sleep 2
cat "$MNT/old" >/dev/null
expect_eq atime-second-read "$old_mnt" "$(stat -c %X "$MNT/old")"
expect_eq atime-second-read-backing "$(stat -c %X /src/old)" \
	"$(stat -c %X "$MNT/old")"

exit "$FAILED"
