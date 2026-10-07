#!/bin/sh
# dcfs step 11.1: power cuts, on real disks (docs/plan/phases/11-crash-stress-
# failure-testing.md; design.md, "A power loss or kernel crash" and "Sync
# points"). guest/power.sh fabricates the state a power loss leaves; here the
# disks themselves lose it. Both disks (backing and cache) sit on dm devices
# (guest/fault_lib.sh); a cut is both switched to drop-writes at one instant
# (fd_cut): what had reached each disk stays, nothing after does. The daemon
# is then killed, both filesystems unmounted (their writes dropped) and
# mounted again healthy, and dcfs started over what the disks hold.
#
# A cut is placed in a mutation's phases with fsfreeze, which holds a mutation
# in its first write to a frozen filesystem:
#
#   before     the cut, then the create: everything is lost
#   phase1     the backing filesystem is frozen, so the create is held in
#              phase 2 with phase 1 durable in the cache: cut there, the
#              create is never durable on the backing filesystem
#   phase2     as phase1, then the cache filesystem is frozen and the backing
#              one thawed and synced: the create is durable on the backing
#              filesystem and the daemon is held in phase 3, before the
#              cache has it: cut there
#   ahead      the backing filesystem drops writes from the start, the cache
#              does not: two creates in different directories, whose phase 1
#              commits fsync the cache WAL, with the first create's phase 3
#              in it: the cache has what the backing filesystem lost
#              ("cache ahead", the case the dirty set is for)
#   syncpoint  the cache filesystem is frozen, and a request runs the
#              periodic sync point: the backing filesystem is synced, and the
#              daemon is held clearing the dirty set: cut there, the cache
#              keeps the dirty rows of a backing change that did survive
#
# After each, the invariant: every entry dcfs serves matches the backing
# filesystem exactly (type, size, mode, and the listing of every directory),
# and the recovery names the dirty rows that survived. A cache ahead of the
# backing filesystem (a name served that the backing filesystem lost) or behind
# it (a name not served that it kept) fails.
#
# Run as /tests/fault_power.sh by guest/init when booted with
# dcfs_test=fault_power.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
LOGS=""
RUN=0
TOUCH_PID=""

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	[ -n "$TOUCH_PID" ] && kill -KILL "$TOUCH_PID" 2>/dev/null || true
	fd_thaw back >/dev/null 2>&1 || true
	fd_thaw cache >/dev/null 2>&1 || true
	umount -l "$MNT" 2>/dev/null || true
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill -KILL "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	fd_umount_disks
	fault_unwrap "$FD_BACK" >/dev/null 2>&1 || true
	fault_unwrap "$FD_CACHE" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "fault_power.sh: kernel $(uname -r)"
require_commands umount sync find stat

DAEMON_PID=""
MOUNTED=0

# start [flags]: a new run of dcfs, its log kept for the failure dump.
start() {
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	fd_start "$LOG" --sync_interval_sec=3600 "$@"
}

# snapshot DIR: one line per entry under DIR, "<path> <type> <size> <mode>".
snapshot() {
	(cd "$1" && find . -path ./lost+found -prune -o -print | sort |
		while read -r p; do stat -c '%n %F %s %a' "$p"; done)
}

# same_as_backing: success if what the daemon serves is what the backing
# filesystem holds, entry by entry (the diff in /tmp/snap.diff if not).
same_as_backing() {
	drop_caches
	snapshot "$SRC" >/tmp/backing.snap
	snapshot "$MNT" >/tmp/served.snap 2>&1
	command diff /tmp/served.snap /tmp/backing.snap >/tmp/snap.diff 2>&1
}

# served_equals_backing NAME
served_equals_backing() {
	if same_as_backing; then
		pass "$1"
	else
		fail "$1" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
}

# after_cut NAME: the daemon is killed, the filesystems remounted from what
# the disks hold, and a new run started; leaves it running.
after_cut() {
	fd_crash
	fd_restore || fail "$1-restore" "remounting after the cut failed"
	if start; then
		pass "$1-restart"
	else
		fail "$1-restart" "daemon did not mount within 10s"
		exit "$FAILED"
	fi
}

# wait_touch: waits for the background `touch`.
wait_touch() {
	wait "$TOUCH_PID" 2>/dev/null || true
	TOUCH_PID=""
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
for d in d1 d2 d3 d4 d5; do
	mkdir "$SRC/$d"
	echo keep >"$SRC/$d/keep"
done
sync

if start; then pass mount; else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/d1" "$MNT/d2" "$MNT/d3" "$MNT/d4" "$MNT/d5" >/dev/null
fault_mode "$FD_BACK" healthy

# --- before: the cut, then the create ----------------------------------------

fd_cut || fail before-cut "fd_cut failed"
touch "$MNT/d1/before"
after_cut before
if [ ! -e "$SRC/d1/before" ]; then
	pass before-lost
else
	fail before-lost "d1/before survived a cut made before it"
fi
served_equals_backing before-served

# --- phase1: held in phase 2, phase 1 durable, then the cut -----------------

ls "$MNT/d2" >/dev/null
fd_freeze back || fail phase1-freeze "FIFREEZE of the backing filesystem failed"
touch "$MNT/d2/p1" &
TOUCH_PID=$!
if fd_blocked "$DAEMON_PID" "$SRC"; then
	echo "fault_power.sh: held: $(fd_where "$DAEMON_PID")"
	pass phase1-held
else
	fail phase1-held "the daemon never blocked on the frozen backing filesystem"
fi
fd_cut || fail phase1-cut "fd_cut failed"
fd_thaw back
wait_touch
after_cut phase1
if [ ! -e "$SRC/d2/p1" ]; then
	pass phase1-create-lost
else
	fail phase1-create-lost "d2/p1 survived a cut before its create was durable"
fi
if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
	pass phase1-dirty-recovered
else
	fail phase1-dirty-recovered "phase 1 was durable, yet no dirty row was recovered: $(grep -i cleanly "$LOG")"
fi
served_equals_backing phase1-served

# --- phase2: the create durable on the backing filesystem, phase 3 not ------

ls "$MNT/d3" >/dev/null
fd_freeze back || fail phase2-freeze "FIFREEZE of the backing filesystem failed"
touch "$MNT/d3/p2" &
TOUCH_PID=$!
fd_blocked "$DAEMON_PID" "$SRC" || fail phase2-held-1 "the daemon never blocked on the frozen backing filesystem"
fd_freeze cache || fail phase2-freeze-cache "FIFREEZE of the cache filesystem failed"
fd_thaw back
sleep 1
# The create ran (phase 2) and the daemon is held before phase 3.
if [ -e "$SRC/d3/p2" ] && kill -0 "$TOUCH_PID" 2>/dev/null; then
	pass phase2-held
else
	fail phase2-held "d3/p2 on the backing filesystem: $([ -e "$SRC/d3/p2" ] && echo yes || echo no); touch running: $(kill -0 "$TOUCH_PID" 2>/dev/null && echo yes || echo no)"
fi
# Make the create durable: a freeze flushes the backing filesystem's journal.
fd_freeze back && fd_thaw back
fd_cut || fail phase2-cut "fd_cut failed"
fd_thaw cache
wait_touch
after_cut phase2
if [ -e "$SRC/d3/p2" ]; then
	pass phase2-create-kept
else
	fail phase2-create-kept "d3/p2 was synced yet is gone"
fi
if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
	pass phase2-dirty-recovered
else
	fail phase2-dirty-recovered "no dirty row recovered: $(grep -i cleanly "$LOG")"
fi
served_equals_backing phase2-served
if [ -e "$MNT/d3/p2" ] && [ "$(ls "$MNT/d3" | tr '\n' ' ')" = "keep p2 " ]; then
	pass phase2-name-served
else
	fail phase2-name-served "d3: $(ls "$MNT/d3" 2>&1)"
fi

# --- ahead: the cache has what the backing filesystem lost ------------------

ls "$MNT/d4" "$MNT/d5" >/dev/null
fault_mode "$FD_BACK" drop-writes || fail ahead-cut-backing "fault_mode failed"
touch "$MNT/d4/a1"
# d5's phase 1 is a durable commit (the directory is not yet dirty): the WAL
# fsync takes d4/a1's phase 3 to the cache disk with it.
touch "$MNT/d5/a2"
fault_mode "$FD_CACHE" drop-writes || fail ahead-cut-cache "fault_mode failed"
after_cut ahead
if [ ! -e "$SRC/d4/a1" ] && [ ! -e "$SRC/d5/a2" ]; then
	pass ahead-creates-lost
else
	fail ahead-creates-lost "the backing filesystem kept $(ls "$SRC/d4" "$SRC/d5" | tr '\n' ' ') (it was cut before)"
fi
if [ "$(fd_recovered "$LOG")" -ge 2 ]; then
	pass ahead-dirty-recovered
else
	fail ahead-dirty-recovered "recovered $(fd_recovered "$LOG") dirty rows, want at least the two parents: $(grep -i cleanly "$LOG")"
fi
served_equals_backing ahead-served
if [ ! -e "$MNT/d4/a1" ] && [ ! -e "$MNT/d5/a2" ]; then
	pass ahead-names-not-served
else
	fail ahead-names-not-served "a name the backing filesystem lost is served (cache ahead)"
fi

# --- syncpoint: held clearing the dirty set, backing synced ------------------

fd_crash
fd_umount_disks
mount "$(fault_dev "$FD_BACK")" "$SRC"
mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR"
if start --sync_interval_sec=2; then pass sync-run-mount; else
	fail sync-run-mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/d1" "$MNT/d2" >/dev/null
touch "$MNT/d1/s1"
sleep 3
fd_freeze cache || fail sync-freeze-cache "FIFREEZE of the cache filesystem failed"
ls "$MNT/d1" >/tmp/ls-sync.out 2>&1 &
TOUCH_PID=$!
if fd_blocked "$DAEMON_PID" "$CACHE_DIR"; then
	echo "fault_power.sh: held: $(fd_where "$DAEMON_PID")"
	pass sync-held
else
	fail sync-held "the daemon was never held writing the cache database (or the request did not start a sync point)"
fi
fd_cut || fail sync-cut "fd_cut failed"
fd_thaw cache
wait_touch
after_cut syncpoint
if [ -e "$SRC/d1/s1" ]; then
	pass sync-backing-kept
else
	fail sync-backing-kept "d1/s1 was synced to the backing filesystem yet is gone"
fi
if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
	pass sync-dirty-set-survived
else
	fail sync-dirty-set-survived "the clearing of the dirty set was lost with the cut, yet no row was recovered: $(grep -i cleanly "$LOG")"
fi
served_equals_backing sync-served

# Self-check of the comparison: a name added behind dcfs's back, after it
# listed the directory, makes it fail. A comparison that never differs would
# make every check above pass for the wrong reason.
ls "$MNT/d1" >/dev/null
touch "$SRC/d1/behind-its-back"
if same_as_backing; then
	fail comparison-detects-divergence "a name only the backing filesystem has went unnoticed"
else
	pass comparison-detects-divergence
fi
rm "$SRC/d1/behind-its-back"

require_no_reclaim no-reclaim
exit "$FAILED"
