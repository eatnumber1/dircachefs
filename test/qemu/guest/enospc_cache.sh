#!/bin/sh
# dcfs step 11.4 (b): the filesystem holding the CACHE DATABASE out of space
# (docs/plan/phases/11-crash-stress-failure-testing.md, 11.4; design.md,
# "The write-through protocol", "Sync points"). SQLite's "disk full"
# (SQLITE_FULL, sqlite.cc's MakeSqliteStatus: kResourceExhausted, ENOSPC)
# must make dcfs fail the request with ENOSPC, or stop serving, and never
# serve wrong data. The cache filesystem is filled (fd_fill) while dcfs
# runs, at three points:
#
# 1. Before a create: its phase 1 (the durable commit, which grows the WAL)
#    fails. The caller gets ENOSPC, the backing filesystem is untouched, the
#    name is not served. Once space is freed the next create works, and
#    after a restart everything served matches the backing filesystem.
# 2. During a create held in phase 2 (the backing filesystem frozen: phase 1
#    is durable): phase 3 cannot record the outcome. The create happened, so
#    it is replied as EEXIST (which makes the kernel drop the
#    negative entry of its LOOKUP before the create: any other error left
#    "no such file" cached for an hour, the bug this step fixed), and from
#    then on the name is served, or the request fails with ENOSPC: never
#    "no such file". The same for a rename, which is replied as done. The
#    dirty rows of phase 1 stay: after space is freed and a crash, the
#    restart recovers them and serves what the backing filesystem holds.
# 3. Before a periodic sync point: the backing filesystem is synced, and
#    clearing the dirty set fails. The request goes on (served from the
#    cache, right), the dirty set survives: after space is freed and a
#    crash, the restart recovers it.
#
# The cache disk is always ext4 (the backing filesystem's type does not
# matter here), so this is a plain small test.
#
# Run as /tests/enospc_cache.sh by guest/init when booted with
# dcfs_test=enospc_cache.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
FILL=$CACHE_DIR/fill
LOGS=""
RUN=0
BG_PID=""
DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	[ -n "$BG_PID" ] && kill -KILL "$BG_PID" 2>/dev/null || true
	fd_thaw back >/dev/null 2>&1 || true
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

echo "enospc_cache.sh: kernel $(uname -r)"
require_commands umount sync find stat grep dd

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

# start NAME [flags]: a new run of dcfs, its log kept for the failure dump.
start() {
	st_name=$1
	shift
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	if fd_start "$LOG" "$@"; then
		pass "$st_name"
	else
		fail "$st_name" "daemon did not mount within 10s"
		exit "$FAILED"
	fi
}

# stop: SIGTERM, a clean shutdown.
stop() {
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null
	echo "enospc_cache.sh: the daemon exited with $?"
	DAEMON_PID=""
	umount -l "$MNT" 2>/dev/null || true
}

# fill_cache / free_cache: the cache filesystem full, then not.
fill_cache() {
	mkdir -p "$FILL"
	fd_fill "$FILL"
}
free_cache() {
	rm -rf "$FILL"
	sync
}

# right_or_nospace NAME WANT CMD...: the command's output is WANT, or it
# fails with ENOSPC; the daemon having stopped serving also passes (it must
# not serve wrong data, and serving nothing is not wrong).
right_or_nospace() {
	ron_name=$1
	ron_want=$2
	shift 2
	if ron_out=$("$@" 2>&1); then
		ron_out=$(echo "$ron_out" | tr '\n' ' ')
		if [ "$ron_out" = "$ron_want" ]; then
			pass "$ron_name"
		else
			fail "$ron_name" "served '$ron_out', the backing filesystem holds '$ron_want'"
		fi
	else
		case "$ron_out" in
		*"No space left on device"*) pass "$ron_name" ;;
		*)
			if alive; then
				fail "$ron_name" "failed, not with ENOSPC: $ron_out"
			else
				echo "enospc_cache.sh: $ron_name: the daemon stopped serving"
				pass "$ron_name"
			fi
			;;
		esac
	fi
	echo "enospc_cache.sh: $ron_name: $ron_out"
}

# served_equals_backing NAME
served_equals_backing() {
	if fd_same_as_backing; then
		pass "$1"
	else
		fail "$1" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
}

# crash_and_restart NAME: SIGKILL (the dirty set kept as it is), and a new
# run over the same database.
crash_and_restart() {
	fd_crash
	start "$1-restart" --sync_interval_sec=3600
	echo "enospc_cache.sh: $1: the restart recovered $(fd_recovered "$LOG") dirty rows"
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
for d in d1 d2 d3; do
	mkdir "$SRC/$d"
	echo keep >"$SRC/$d/keep"
done
sync

# --- 1. phase 1 cannot commit ------------------------------------------------

start mount --sync_interval_sec=3600
ls -R "$MNT" >/dev/null
fill_cache
out=$(touch "$MNT/d1/new" 2>&1)
rc=$?
echo "enospc_cache.sh: phase1: create with the cache full: rc=$rc $out"
if [ "$rc" -ne 0 ]; then
	pass phase1-fails
	case "$out" in
	*"No space left on device"*) pass phase1-enospc ;;
	*)
		if alive; then
			fail phase1-enospc "failed, not with ENOSPC: $out"
		else
			echo "enospc_cache.sh: phase1: the daemon stopped serving"
		fi
		;;
	esac
else
	fail phase1-fails "the create succeeded with the cache filesystem full"
fi
if [ ! -e "$SRC/d1/new" ]; then
	pass phase1-backing-untouched
else
	fail phase1-backing-untouched "d1/new reached the backing filesystem although phase 1 failed"
fi
right_or_nospace phase1-listing "keep " ls "$MNT/d1"
free_cache
if alive; then
	if touch "$MNT/d1/after" && [ -e "$SRC/d1/after" ] && [ -e "$MNT/d1/after" ]; then
		pass phase1-mutates-after-free
	else
		fail phase1-mutates-after-free "touch d1/after once space was freed"
	fi
	stop
fi
start phase1-restart --sync_interval_sec=3600
served_equals_backing phase1-served-equals-backing

# --- 2. phase 3 cannot commit -------------------------------------------------

ls "$MNT/d2" >/dev/null
fd_freeze back || fail phase3-freeze "FIFREEZE of the backing filesystem failed"
touch "$MNT/d2/p3" 2>/tmp/p3.err &
BG_PID=$!
if fd_blocked "$DAEMON_PID" "$SRC"; then
	pass phase3-held-in-phase2
else
	fail phase3-held-in-phase2 "the daemon never blocked on the frozen backing filesystem"
fi
fill_cache
fd_thaw back || fail phase3-thaw "FITHAW of the backing filesystem failed"
wait "$BG_PID"
rc=$?
BG_PID=""
echo "enospc_cache.sh: phase3: the create said: rc=$rc $(cat /tmp/p3.err)"
# The create happened; it cannot be recorded, so it is replied as EEXIST
# (true now: the probe found it), never as another error: on any other
# error the kernel keeps the negative entry of the LOOKUP before the
# create.
if [ "$rc" -ne 0 ] && grep -q "File exists" /tmp/p3.err; then
	pass phase3-create-eexist
else
	fail phase3-create-eexist "the create that happened but could not be recorded was replied: rc=$rc $(cat /tmp/p3.err)"
fi
if grep -q "Could not complete a create that reached the backing" "$LOG"; then
	pass phase3-create-logged
else
	fail phase3-create-logged "no ERROR that the create could not be completed"
fi
if [ -e "$SRC/d2/p3" ]; then
	pass phase3-on-backing
else
	fail phase3-on-backing "d2/p3 is not on the backing filesystem: phase 2 did not run"
fi
# What the kernel answers now, from its own dentry cache or by asking dcfs:
# the file, or ENOSPC; never "no such file".
right_or_nospace phase3-kernel-name "$MNT/d2/p3 " ls -d "$MNT/d2/p3"

# A rename held in phase 2 while the cache fills: it happened, so it is
# replied as done (design.md, "Phase 3"), and the kernel moves its own
# dentries.
free_cache
fd_freeze back || fail phase3-rename-freeze "FIFREEZE of the backing filesystem failed"
mv "$MNT/d2/keep" "$MNT/d2/keep2" 2>/tmp/mv.err &
BG_PID=$!
if fd_blocked "$DAEMON_PID" "$SRC"; then
	pass phase3-rename-held-in-phase2
else
	fail phase3-rename-held-in-phase2 "the daemon never blocked on the frozen backing filesystem"
fi
fill_cache
fd_thaw back || fail phase3-rename-thaw "FITHAW of the backing filesystem failed"
wait "$BG_PID"
rc=$?
BG_PID=""
if [ "$rc" -eq 0 ] && [ -e "$SRC/d2/keep2" ] && [ ! -e "$SRC/d2/keep" ]; then
	pass phase3-rename-done
else
	fail phase3-rename-done "rc=$rc $(cat /tmp/mv.err); backing d2: $(ls "$SRC/d2" | tr '\n' ' ')"
fi
right_or_nospace phase3-rename-kernel-new "$MNT/d2/keep2 " ls -d "$MNT/d2/keep2"
if ls -d "$MNT/d2/keep" >/tmp/old.out 2>&1; then
	fail phase3-rename-kernel-old "the old name is still served: $(cat /tmp/old.out)"
else
	pass phase3-rename-kernel-old
fi

# And from dcfs itself, the kernel's caches dropped.
drop_caches
right_or_nospace phase3-name "$MNT/d2/p3 " ls -d "$MNT/d2/p3"
right_or_nospace phase3-listing "keep2 p3 " ls "$MNT/d2"
free_cache
crash_and_restart phase3
if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
	pass phase3-dirty-recovered
else
	fail phase3-dirty-recovered "phase 1 was durable, yet nothing was recovered: $(grep -i cleanly "$LOG")"
fi
served_equals_backing phase3-served-equals-backing

# --- 3. a sync point cannot clear the dirty set --------------------------------

stop
start sync-run --sync_interval_sec=2
ls "$MNT/d3" >/dev/null
touch "$MNT/d3/s1"
[ -e "$SRC/d3/s1" ] || fail sync-create "d3/s1 is not on the backing filesystem"
fill_cache
sleep 3
# The first request after the interval runs the sync point.
right_or_nospace sync-listing "keep s1 " ls "$MNT/d3"
echo "enospc_cache.sh: sync: the daemon's log: $(grep -i 'sync' "$LOG" | tail -2)"
free_cache
crash_and_restart sync
if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
	pass sync-dirty-set-survived
else
	fail sync-dirty-set-survived "the failed sync point cleared the dirty set: $(grep -i cleanly "$LOG")"
fi
served_equals_backing sync-served-equals-backing
if touch "$MNT/d3/after" && [ -e "$SRC/d3/after" ]; then
	pass sync-mutates-after-free
else
	fail sync-mutates-after-free "touch d3/after once space was freed"
fi

require_no_reclaim no-reclaim
exit "$FAILED"
