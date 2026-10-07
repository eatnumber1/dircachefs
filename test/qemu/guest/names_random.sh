#!/bin/sh
# dcfs step 9: names of random bytes (any byte but NUL and '/', 1-255 bytes),
# seeded, created through dcfs and directly on the backing filesystem, and
# compared both ways. RANDOM_COUNT names in all, half in each direction: 1000
# by default (RANDOM_COUNT overrides it).
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
RANDOM_COUNT=${RANDOM_COUNT:-1000}
PER_DIRECTION=$((RANDOM_COUNT / 2))

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
		# Did the daemon die (a sanitizer report or abort is in its stderr
		# above), or did the guest kill it (the OOM killer's lines are in dmesg)?
		if [ -n "$DAEMON_PID" ] && ! kill -0 "$DAEMON_PID" 2>/dev/null; then
			wait "$DAEMON_PID"
			echo "--- dcfs (pid $DAEMON_PID) is no longer running; exit status $? (137 = SIGKILL, i.e. the guest's OOM killer) ---"
		fi
		echo "--- guest memory ---"
		grep -E '^(MemTotal|MemFree|MemAvailable|Cached|Slab|SReclaimable):' /proc/meminfo
		echo "--- dmesg: OOM lines ---"
		dmesg 2>/dev/null | grep -i -E 'oom|out of memory|killed process' | tail -n 10
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

echo "names_random.sh: $RANDOM_COUNT random names, $PER_DIRECTION per direction"

# mem_report WHEN: dcfs's resident-set peak and the guest's available memory,
# so a run that dies has the numbers that show whether memory was the cause.
mem_report() {
	hwm=""
	rss=""
	if [ -n "$DAEMON_PID" ] && [ -r "/proc/$DAEMON_PID/status" ]; then
		while read -r key val _; do
			case "$key" in
			VmHWM:) hwm=$val ;;
			VmRSS:) rss=$val ;;
			esac
		done <"/proc/$DAEMON_PID/status"
	fi
	avail=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo)
	echo "names_random.sh: memory $1: dcfs VmRSS=${rss:-gone} KiB VmHWM=${hwm:-gone} KiB, guest MemAvailable=$avail KiB"
}

same_dump() {
	name=$1
	a=$($TESTUTIL names-dump "$2" | md5sum)
	b=$($TESTUTIL names-dump "$3" | md5sum)
	if [ "$a" = "$b" ]; then
		pass "$name"
	else
		fail "$name" "dump of $2 differs from $3"
		$TESTUTIL names-dump "$2" | head -c 3000000 >/tmp/dump-a.txt
		$TESTUTIL names-dump "$3" | head -c 3000000 >/tmp/dump-b.txt
		command diff /tmp/dump-a.txt /tmp/dump-b.txt | head -10
	fi
}

mount /dev/vdb /src
mkdir -p /cache /mnt

# Direction 1 setup: names created directly on the backing filesystem, before
# dcfs ever lists it (dcfs reads them from the backing filesystem).
mkdir /src/direct
if out=$($TESTUTIL names-random /src/direct 7 "$PER_DIRECTION" 2>&1); then
	pass backing-create
else
	fail backing-create "$out"
fi
sync

if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

mem_report mounted

# Direction 1: seen through dcfs.
same_dump direct-seen-through-dcfs "$MNT/direct" /src/direct
mem_report after-direction-1

# Direction 2: created through dcfs, seen on the backing filesystem.
mkdir "$MNT/viadcfs"
if out=$($TESTUTIL names-random "$MNT/viadcfs" 11 "$PER_DIRECTION" 2>&1); then
	pass dcfs-create
else
	fail dcfs-create "$out"
fi
same_dump dcfs-created-matches-backing "$MNT/viadcfs" /src/viadcfs
mem_report after-direction-2

exit "$FAILED"
fi

mem_report mounted

# Direction 1: seen through dcfs.
same_dump direct-seen-through-dcfs "$MNT/direct" /src/direct
mem_report after-direction-1

# Direction 2: created through dcfs, seen on the backing filesystem.
mkdir "$MNT/viadcfs"
if out=$($TESTUTIL names-random "$MNT/viadcfs" 11 "$PER_DIRECTION" 2>&1); then
	pass dcfs-create
else
	fail dcfs-create "$out"
fi
same_dump dcfs-created-matches-backing "$MNT/viadcfs" /src/viadcfs
mem_report after-direction-2

# The same names, direct and through dcfs, give the same tree.
mkdir /src/ref
$TESTUTIL names-random /src/ref 11 "$RANDOM_COUNT" >/dev/null 2>&1
same_dump dcfs-created-matches-reference "$MNT/viadcfs" /src/ref

exit "$FAILED"
