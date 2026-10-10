#!/bin/sh
# dcfs step 23.8: access times are the backing filesystem's own.
#
# Reads go through passthrough, so dcfs never sees them; the backing
# filesystem stamps the file's atime by its mount's rule (relatime here) and
# its own flags. While dcfs holds a backing descriptor for a file it serves
# the file's attributes from a statx of that descriptor (at FLUSH, at
# RELEASE, and for a stat while the file is open), so after any kind of read
# a stat through dcfs reports exactly what the backing filesystem has, and an
# open that reads nothing changes nothing. Directories and symlinks are read
# from the cache, never from the backing filesystem: dcfs stamps the atime the
# relatime rule gives in its database, never on the backing filesystem, and
# it survives a restart of dcfs. (A power cut after a read: guest/
# fault_power.sh's atime scenario.)
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
	exec 3<&- 2>/dev/null
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
require_commands stat date cat dd readlink ls

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

expect_ne() {
	if [ "$2" != "$3" ]; then
		pass "$1"
	else
		fail "$1" "both are '$2'"
	fi
}

# atime PATH: PATH's access time to the nanosecond (a symlink's own).
atime() { stat -c %x "$1"; }

# same_atime NAME FILE: what dcfs serves for FILE is the backing file's atime
# exactly (dcfs's first: a stat on the backing filesystem changes nothing).
same_atime() {
	sa_served=$(atime "$MNT/$2")
	expect_eq "$1" "$(atime "$SRC/$2")" "$sa_served"
}

mount /dev/vdb /src
grep " /src " /proc/mounts
now=$(date +%s)
old=$((now - 172800))
older=$((now - 259200))
# Every file's atime two days old and its mtime three: any read moves it.
for f in nothing read held flag onoatime mmap lsattr cached; do
	echo "$f" >"$SRC/$f"
	"$TESTUTIL" utimes2 "$SRC/$f" "$old" "$older"
done
# chattr +A: the file's own noatime flag (FS_NOATIME_FL).
"$TESTUTIL" setflags "$SRC/flag" 80
mkdir "$SRC/d"
echo x >"$SRC/d/x"
ln -s nothing "$SRC/l"
sync

case "$(atime "$SRC/read")" in
*.?????????*) pass atime-has-nanoseconds ;;
*) fail atime-has-nanoseconds "stat -c %x prints '$(atime "$SRC/read")'" ;;
esac

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls -l "$MNT" >/dev/null

# --- regular files: the backing filesystem's atime, exactly -----------------

# An open that reads nothing.
before=$(atime "$SRC/nothing")
: <"$MNT/nothing"
expect_eq open-no-read-backing "$before" "$(atime "$SRC/nothing")"
same_atime open-no-read-served nothing

# A read, then a stat.
before=$(atime "$SRC/read")
cat "$MNT/read" >/dev/null
same_atime read-then-stat read
expect_ne read-moved-it "$before" "$(atime "$SRC/read")"

# A reader that keeps the file open: a stat while it is held.
exec 3<"$MNT/held"
before=$(atime "$SRC/held")
same_atime held-before-read held
dd bs=1 count=1 <&3 >/dev/null 2>&1
same_atime held-after-read held
expect_ne held-read-moved-it "$before" "$(atime "$SRC/held")"
exec 3<&-
same_atime held-after-close held

# chattr +A: the read leaves the atime.
before=$(atime "$SRC/flag")
cat "$MNT/flag" >/dev/null
expect_eq noatime-flag-backing "$before" "$(atime "$SRC/flag")"
same_atime noatime-flag-served flag

# O_NOATIME: the read leaves it too.
before=$(atime "$SRC/onoatime")
"$TESTUTIL" catnoatime "$MNT/onoatime" >/dev/null
expect_eq o-noatime-backing "$before" "$(atime "$SRC/onoatime")"
same_atime o-noatime-served onoatime

# A read through a mapping, closed before the read.
before=$(atime "$SRC/mmap")
"$TESTUTIL" mmapread "$MNT/mmap" >/dev/null
same_atime mmap-read mmap
expect_ne mmap-moved-it "$before" "$(atime "$SRC/mmap")"

# lsattr: its open and the private open the kernel makes for the ioctl
# (FUSE's fileattr_get), neither of which reads.
before=$(atime "$SRC/lsattr")
"$TESTUTIL" getflags "$MNT/lsattr" >/dev/null
expect_eq lsattr-backing "$before" "$(atime "$SRC/lsattr")"
same_atime lsattr-served lsattr

# Once the file is closed its stat is answered from the cache, with no
# backing read.
cat "$MNT/cached" >/dev/null
drop_caches_quiesced
sectors_before=$(sectors_read vdb)
served=$(atime "$MNT/cached")
expect_eq stat-from-cache "$sectors_before" "$(sectors_read vdb)"
expect_eq cached-is-backing "$(atime "$SRC/cached")" "$served"

# --- directories and symlinks: stamped in the cache only -------------------
#
# Their reads are served from the cache. The first listing of d was dcfs's
# own population (a read on the backing filesystem); an atime set through
# dcfs puts both back two days, then a listing from the cache stamps the
# relatime rule's "now" in dcfs's database and leaves the backing alone.
ls "$MNT/d" >/dev/null
"$TESTUTIL" utimes2 "$MNT/d" "$old" "$older"
"$TESTUTIL" utimens "$MNT/l" "$old" 0
d_backing=$(atime "$SRC/d")
l_backing=$(atime "$SRC/l")
ls "$MNT/d" >/dev/null
readlink "$MNT/l" >/dev/null
close_to dir-listing-stamps "$(date +%s)" "$(stat -c %X "$MNT/d")"
close_to symlink-readlink-stamps "$(date +%s)" "$(stat -c %X "$MNT/l")"
expect_eq dir-backing-untouched "$d_backing" "$(atime "$SRC/d")"
expect_eq symlink-backing-untouched "$l_backing" "$(atime "$SRC/l")"
# Within the day, after the change time: a second listing changes nothing.
d_stamp=$(atime "$MNT/d")
l_stamp=$(atime "$MNT/l")
sleep 2
ls "$MNT/d" >/dev/null
readlink "$MNT/l" >/dev/null
expect_eq dir-second-listing "$d_stamp" "$(atime "$MNT/d")"
expect_eq symlink-second-readlink "$l_stamp" "$(atime "$MNT/l")"

# --- a restart of dcfs keeps all of it -------------------------------------

restart_daemon restart "$LOG" || exit "$FAILED"
expect_eq dir-stamp-survives-restart "$d_stamp" "$(atime "$MNT/d")"
expect_eq symlink-stamp-survives-restart "$l_stamp" "$(atime "$MNT/l")"
for f in nothing read held flag onoatime mmap lsattr; do
	same_atime "after-restart-$f" "$f"
done

require_no_reclaim no-reclaim
exit "$FAILED"
