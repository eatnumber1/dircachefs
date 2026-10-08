#!/bin/sh
# dcfs step 11.5: an instant crash of the BACKING filesystem, the shutdown
# ioctl xfstests' godown uses (docs/plan/phases/11-crash-stress-failure-
# testing.md, 11.5; design.md, "The write-through protocol", "A power loss
# or kernel crash", "Sync points"). FS_IOC_SHUTDOWN (`testutil shutdown`)
# makes the filesystem fail every operation that reaches it from that
# instant, while dcfs keeps running over it; it comes back only when it is
# unmounted and mounted again. ext4 and xfs have three flavours: "default"
# freezes first (everything is durable), "logflush" commits the journal,
# "nologflush" commits nothing (what was not durable is lost, as in a power
# cut). btrfs has the ioctl only from Linux 6.19, behind
# CONFIG_BTRFS_EXPERIMENTAL; where it is missing (ENOTTY) the crash is the
# backing disk turned into dm-error ("dead", guest/fault_lib.sh) and a
# syncfs, which makes btrfs abort its transaction: nothing more reaches the
# disk, as with nologflush.
#
# 1. One run per flavour, over its own directory: completed mutations not yet
#    synced (a create and its data, a mkdir, a rename, a setxattr: their
#    rows are dirty), and a create held in phase 1 (the cache filesystem
#    frozen) when the backing crashes. After the crash: the held create and
#    new mutations fail; a file's contents cannot be read; nothing is
#    served that was not served before the crash; the daemon lives (the
#    checking build: a broken invariant aborts it). A clean shutdown over the
#    crashed backing cannot sync it, so the dirty set survives; after the
#    backing is remounted and dcfs restarted, everything served matches the
#    backing. nologflush must have lost the unsynced create (else the case
#    proves nothing); default and logflush must have kept it.
# 2. A create held in phase 2 (the backing filesystem frozen) when it
#    crashes: it fails, and is neither served nor on the backing afterwards.
# 3. A crash while dcfs is idle (clean: the dirty set is empty), then a
#    restart without remounting. The shutdown cannot sync the crashed
#    filesystem, so the run does not end clean. Whether dcfs starts is
#    recorded (no_remount_start: xfs fails the open of --source); if it does,
#    it serves nothing new, and a create through it fails (the crashed
#    filesystem refuses it). After the remount, everything
#    served matches the backing.
# 4. A daemon crash, a restart over the live backing, a listing, then the
#    backing crashes (nologflush). Recovery forgets the dirty rows and the
#    listing re-reads the directory from the backing filesystem, whose
#    change is not durable yet; the crash takes it back. The rows must still
#    be dirty (step 12.6: recovery keeps them until a sync point), so that
#    after the remount the next start forgets them again and everything
#    served matches the backing.
# 5. As 3, with an unsynced create's dirty rows: after the remount the
#    create is lost and not served, whatever the start without a remount
#    re-read.
#
# ext4 is mounted with commit=60, so its journal commits only when asked
# within a run (xfs pushes its log every 30 s, btrfs commits every 30 s):
# "not yet durable" is then a fact, not a race with the 5 s commit.
#
# Run as /tests/fault_shutdown.sh by guest/init when booted with
# dcfs_test=fault_shutdown.sh.
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
BG_PID=""
DAEMON_PID=""
MOUNTED=0
# "ioctl" or "dead": how this filesystem is crashed (crash_backing).
CRASH_HOW=ioctl

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

echo "fault_shutdown.sh: kernel $(uname -r)"
require_commands umount sync find stat grep

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

# start [flags]: a new run of dcfs (no periodic sync point), its log kept for
# the failure dump.
start() {
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	fd_start "$LOG" --sync_interval_sec=3600 "$@"
}

# stop NAME: SIGTERM (a clean shutdown: FinishRun's sync point), up to 20 s
# for the daemon to exit and the mount to go.
stop() {
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	st_n=0
	while kill -0 "$DAEMON_PID" 2>/dev/null && [ "$st_n" -lt 100 ]; do
		sleep 0.2
		st_n=$((st_n + 1))
	done
	if kill -0 "$DAEMON_PID" 2>/dev/null; then
		fail "$1-stops" "the daemon did not exit within 20 s of SIGTERM"
		kill -KILL "$DAEMON_PID" 2>/dev/null || true
	fi
	wait "$DAEMON_PID" 2>/dev/null
	echo "fault_shutdown.sh: $1: the daemon exited with $?"
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		fail "$1-unmounted" "$MNT is still mounted after the daemon exited"
		umount -l "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
}

# mount_backing: the backing filesystem on its dm device (ext4: commit=60).
mount_backing() {
	if [ "${FSTYPE:-}" = ext4 ]; then
		mount -o commit=60,errors=remount-ro "$(fault_dev "$FD_BACK")" "$SRC"
	else
		mount "$(fault_dev "$FD_BACK")" "$SRC"
	fi
}

# remount_backing NAME: the daemon is gone; the crashed backing filesystem is
# unmounted, its disk healthy again, and mounted (its journal replays).
remount_backing() {
	umount "$SRC" || fail "$1-umount-backing" "cannot unmount the crashed backing filesystem"
	fault_mode "$FD_BACK" healthy || fail "$1-heal" "fault_mode healthy failed"
	mount_backing || fail "$1-mount-backing" "cannot mount the backing filesystem again"
}

# crash_backing FLAVOUR: the instant crash (the shutdown ioctl, or the disk
# dead). On btrfs without the ioctl the first call switches CRASH_HOW.
crash_backing() {
	if [ "$CRASH_HOW" = ioctl ]; then
		cb_out=$("$TESTUTIL" shutdown "$SRC" "$1" 2>&1) && return 0
		if [ "$FSTYPE" = btrfs ] && [ "$cb_out" = "ERR ENOTTY" ]; then
			echo "fault_shutdown.sh: btrfs has no FS_IOC_SHUTDOWN in this kernel ($cb_out): the crash is the disk turned into dm-error"
			CRASH_HOW=dead
		else
			echo "fault_shutdown.sh: testutil shutdown $1: $cb_out"
			return 1
		fi
	fi
	fault_mode "$FD_BACK" dead
}

# settle_crash: with the disk dead, btrfs fails only once it writes: a
# change made directly on it and a syncfs make it meet the dead disk and
# abort its transaction (it then refuses writes, and reads what it has in
# memory).
settle_crash() {
	[ "$CRASH_HOW" = dead ] || return 0
	touch "$SRC/.crash-trigger" 2>/dev/null
	sync -f "$SRC" 2>/dev/null || true
}

# crashed NAME: the backing filesystem refuses a write made directly on it.
crashed() {
	if touch "$SRC/.crash-probe" 2>/dev/null; then
		fail "$1-crash-took-effect" "a direct create on the backing filesystem succeeded after the crash"
		rm -f "$SRC/.crash-probe"
	else
		pass "$1-crash-took-effect"
	fi
}

# wait_bg NAME: up to 20 s for the background command (BG_PID) to return;
# sets BG_RC to its exit status (or 255 if it was still running, a FAIL).
wait_bg() {
	wb_n=0
	while kill -0 "$BG_PID" 2>/dev/null && [ "$wb_n" -lt 100 ]; do
		sleep 0.2
		wb_n=$((wb_n + 1))
	done
	if kill -0 "$BG_PID" 2>/dev/null; then
		fail "$1" "the held mutation had not returned 20 s after the crash; the daemon is in $(fd_where "$DAEMON_PID"), state $(grep '^State' "/proc/$DAEMON_PID/status" 2>/dev/null)"
		kill -KILL "$BG_PID" 2>/dev/null || true
		BG_RC=255
	else
		wait "$BG_PID"
		BG_RC=$?
	fi
	BG_PID=""
}

# served_names DIR OUT: the names dcfs serves under $MNT/DIR, sorted.
served_names() {
	(cd "$MNT" && find "$1" 2>/dev/null | sort) >"$2"
}

# serves_nothing_new NAME DIR: every name served under DIR now was served
# before the crash (/tmp/pre.names).
serves_nothing_new() {
	served_names "$2" /tmp/during.names
	snn_new=$(grep -vxF -f /tmp/pre.names /tmp/during.names)
	if [ -z "$snn_new" ]; then
		pass "$1"
	else
		fail "$1" "served after the crash, never before: $(echo "$snn_new" | tr '\n' ' ')"
	fi
}

# must_fail NAME DESCRIPTION CMD...: the command fails (it reaches the
# crashed backing filesystem); its error is printed.
must_fail() {
	mf_name=$1
	mf_what=$2
	shift 2
	if mf_out=$("$@" 2>&1); then
		fail "$mf_name" "$mf_what succeeded over the crashed backing filesystem"
	else
		echo "fault_shutdown.sh: $mf_what: $mf_out"
		pass "$mf_name"
	fi
}

# read_fails_or_right NAME FILE: reading FILE's contents (a passthrough
# open: the backing filesystem answers) fails after a shutdown. A dead disk
# under btrfs leaves it reading what it has in memory, so there the read
# may also return the right contents ("keep"), never anything else.
read_fails_or_right() {
	if rf_out=$(cat "$2" 2>&1); then
		if [ "$CRASH_HOW" = dead ] && [ "$rf_out" = keep ]; then
			echo "fault_shutdown.sh: $1: read from btrfs's memory: $rf_out"
			pass "$1-fails-or-right"
		else
			fail "$1-fails-or-right" "cat succeeded over the crashed backing filesystem: $rf_out"
		fi
	else
		echo "fault_shutdown.sh: $1: $rf_out"
		pass "$1-fails-or-right"
	fi
}

# no_remount_start NAME DIR: a start over the crashed backing filesystem,
# not remounted, after a run that could not shut down cleanly (its sync
# point failed). xfs fails the open of --source, so dcfs refuses; ext4 and
# an aborted btrfs let it start. Started, it must serve nothing new under
# DIR (a create through it fails: the crashed filesystem refuses it); its
# recovery keeps the dirty set until a sync
# point that succeeds (step 12.6), so nothing it re-reads from the crashed
# filesystem's memory outlives the remount (checked after it).
no_remount_start() {
	if start; then
		echo "fault_shutdown.sh: $1: dcfs started over the crashed backing filesystem, not remounted"
		pass "$1-no-remount-start-recorded"
		serves_nothing_new "$1-no-remount-serves-nothing-new" "$2"
		must_fail "$1-no-remount-create-fails" "touch $2/b/new" touch "$MNT/$2/b/new"
		ls -R "$MNT/$2" >/dev/null 2>&1
		stop "$1-no-remount"
	else
		echo "fault_shutdown.sh: $1: dcfs refused to start: $(grep -v -E '^(dcfs/|===| *$)' "$LOG" | tail -1)"
		DAEMON_PID=""
		umount -l "$MNT" 2>/dev/null || true
		pass "$1-no-remount-start-recorded"
	fi
}

# served_equals_backing NAME
served_equals_backing() {
	if fd_same_as_backing; then
		pass "$1"
	else
		fail "$1" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
}

# dirty_set_survived NAME: the last start recovered dirty rows: the clean
# shutdown over the crashed filesystem could not sync it, so it kept them.
dirty_set_survived() {
	if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
		pass "$1-dirty-set-survived"
	else
		fail "$1-dirty-set-survived" "nothing recovered: $(grep -i cleanly "$LOG")"
	fi
}

# restart_after_remount NAME: stop, remount the backing, start again.
restart_after_remount() {
	stop "$1"
	remount_backing "$1"
	if start; then
		pass "$1-restart"
	else
		fail "$1-restart" "daemon did not mount within 10s"
		exit "$FAILED"
	fi
	echo "fault_shutdown.sh: $1: the restart recovered $(fd_recovered "$LOG") dirty rows"
}

# --- 1. completed and held mutations, one run per flavour -------------------

# flavour_run FLAVOUR: over $SRC/FLAVOUR (a, b, c, d, each with "keep").
flavour_run() {
	f=$1
	t=$SRC/$f
	m=$MNT/$f
	ls -R "$m" >/dev/null
	echo made >"$m/a/done"
	mkdir "$m/a/dir"
	mv "$m/b/keep" "$m/b/moved"
	"$TESTUTIL" setxattr "$m/b/moved" user.k v >/dev/null
	if [ -e "$t/a/done" ] && [ -d "$t/a/dir" ] && [ -e "$t/b/moved" ]; then
		pass "$f-mutations-done"
	else
		fail "$f-mutations-done" "a/done, a/dir or b/moved missing on the backing filesystem"
	fi
	served_names "$f" /tmp/pre.names
	fd_freeze cache || fail "$f-freeze-cache" "FIFREEZE of the cache filesystem failed"
	touch "$m/c/p1" 2>/dev/null &
	BG_PID=$!
	if fd_blocked "$DAEMON_PID" "$CACHE_DIR"; then
		pass "$f-held-in-phase1"
	else
		fail "$f-held-in-phase1" "the daemon never blocked on the frozen cache filesystem"
	fi
	crash_backing "$f" || fail "$f-crash" "the crash failed"
	settle_crash
	crashed "$f"
	fd_thaw cache || fail "$f-thaw-cache" "FITHAW of the cache filesystem failed"
	wait_bg "$f-held-create-returns"
	if [ "$BG_RC" -ne 0 ]; then
		pass "$f-held-create-fails"
	else
		fail "$f-held-create-fails" "the create held in phase 1 succeeded over the crashed backing filesystem"
	fi
	if alive; then pass "$f-daemon-alive"; else fail "$f-daemon-alive" "the daemon died"; fi
	must_fail "$f-create-fails" "touch c/after" touch "$m/c/after"
	must_fail "$f-mkdir-fails" "mkdir a/newdir" mkdir "$m/a/newdir"
	read_fails_or_right "$f-read" "$m/d/keep"
	# The clean entries' cached attributes may still be served (the backing
	# filesystem holds them durably), or not: never anything else.
	clean=$(stat -c %s "$m/d/keep" 2>&1)
	echo "fault_shutdown.sh: $f: stat d/keep after the crash: $clean"
	case "$clean" in
	5 | *"I/O error"* | *"Input/output error"*) pass "$f-clean-entry-right-or-error" ;;
	*) fail "$f-clean-entry-right-or-error" "stat d/keep: $clean (want 5 or an I/O error)" ;;
	esac
	serves_nothing_new "$f-serves-nothing-new" "$f"
	if alive; then pass "$f-daemon-alive-2"; else fail "$f-daemon-alive-2" "the daemon died"; fi

	restart_after_remount "$f"
	dirty_set_survived "$f"
	served_equals_backing "$f-served-equals-backing"
	if [ ! -e "$t/c/p1" ] && [ ! -e "$t/c/after" ] && [ ! -e "$t/a/newdir" ]; then
		pass "$f-failed-mutations-absent"
	else
		fail "$f-failed-mutations-absent" "c: $(ls "$t/c" | tr '\n' ' ') a: $(ls "$t/a" | tr '\n' ' ')"
	fi
	case "$f" in
	nologflush | dead)
		if [ ! -e "$t/a/done" ]; then
			pass "$f-unsynced-lost"
		else
			fail "$f-unsynced-lost" "a/done survived a crash that commits nothing: the cache-ahead case was not exercised"
		fi
		;;
	*)
		if [ -e "$t/a/done" ] && [ -e "$t/b/moved" ]; then
			pass "$f-flushed-kept"
		else
			fail "$f-flushed-kept" "a/done or b/moved lost by a crash that flushes the journal"
		fi
		;;
	esac
	if touch "$m/c/healed" && [ -e "$t/c/healed" ] && [ -e "$m/c/healed" ]; then
		pass "$f-mutates-after-remount"
	else
		fail "$f-mutates-after-remount" "touch c/healed after the remount"
	fi
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
FSTYPE=$(backing_fstype "$SRC")
echo "fault_shutdown.sh: backing filesystem $FSTYPE"
if [ "$FSTYPE" = ext4 ]; then
	mount -o remount,commit=60,errors=remount-ro "$SRC" ||
		fail setup-commit "remount with commit=60,errors=remount-ro failed"
fi
for top in default logflush nologflush phase2 idle recrash dirty ro; do
	for d in a b c d; do
		mkdir -p "$SRC/$top/$d"
		echo keep >"$SRC/$top/$d/keep"
	done
done
sync

if start; then pass mount; else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

if [ "$FSTYPE" = btrfs ]; then
	# The first crash finds out whether btrfs has the ioctl; without it, one
	# run ("dead") stands for all three flavours.
	flavour_run nologflush
	if [ "$CRASH_HOW" = ioctl ]; then
		flavour_run logflush
		flavour_run default
	fi
else
	flavour_run default
	flavour_run logflush
	flavour_run nologflush
fi

# --- 2. a create held in phase 2 ---------------------------------------------

phase2_run() {
	ls -R "$MNT/phase2" >/dev/null
	served_names phase2 /tmp/pre.names
	fd_freeze back || fail phase2-freeze "FIFREEZE of the backing filesystem failed"
	touch "$MNT/phase2/c/p2" 2>/dev/null &
	BG_PID=$!
	if fd_blocked "$DAEMON_PID" "$SRC"; then
		pass phase2-held
	else
		fail phase2-held "the daemon never blocked on the frozen backing filesystem"
	fi
	crash_backing nologflush || fail phase2-crash "the crash failed"
	thawed=$(fd_thaw back 2>&1) || fail phase2-thaw "FITHAW of the crashed backing filesystem: $thawed"
	settle_crash
	crashed phase2
	wait_bg phase2-held-create-returns
	held_rc=$BG_RC
	if [ "$CRASH_HOW" = ioctl ]; then
		if [ "$held_rc" -ne 0 ]; then
			pass phase2-held-create-fails
		else
			fail phase2-held-create-fails "the create held in phase 2 succeeded over the crashed backing filesystem"
		fi
	else
		# A dead disk fails a btrfs create only once btrfs writes: it may have
		# succeeded in memory, to be lost.
		echo "fault_shutdown.sh: phase2: the held create (dead disk) exited $held_rc"
	fi
	if alive; then pass phase2-daemon-alive; else fail phase2-daemon-alive "the daemon died"; fi
	restart_after_remount phase2
	served_equals_backing phase2-served-equals-backing
	if [ ! -e "$SRC/phase2/c/p2" ]; then
		pass phase2-create-absent
	else
		fail phase2-create-absent "c/p2 is on the backing filesystem"
	fi
}

if [ "$FSTYPE" = xfs ]; then
	# xfs refuses FITHAW once it is shut down (EIO), so it stays frozen and
	# the held create, the daemon and the unmount wait for ever.
	skip phase2 "a shut-down xfs refuses FITHAW (EIO, Linux 6.18) and stays frozen: a create held by the freeze never returns"
else
	phase2_run
fi

# --- 3. a crash while idle, then a restart without remounting ----------------

stop idle-clean
if start; then pass idle-clean-start; else
	fail idle-clean-start "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ "$(fd_recovered "$LOG")" -eq 0 ] && ! grep -q "did not shut down cleanly" "$LOG"; then
	pass idle-clean-shutdown-before
else
	fail idle-clean-shutdown-before "the run before did not shut down cleanly: $(grep -i cleanly "$LOG")"
fi
ls -R "$MNT" >/dev/null
served_equals_backing idle-before
served_names idle /tmp/pre.names
crash_backing nologflush || fail idle-crash "the crash failed"
settle_crash
crashed idle
must_fail idle-create-fails "touch idle/a/new" touch "$MNT/idle/a/new"
read_fails_or_right idle-read "$MNT/idle/a/keep"
serves_nothing_new idle-serves-nothing-new idle
if alive; then pass idle-daemon-alive; else fail idle-daemon-alive "the daemon died"; fi
stop idle
# The clean shutdown could not sync the crashed backing filesystem, so the
# run did not end clean.
echo "fault_shutdown.sh: idle: the shutdown said: $(grep -i syncfs "$LOG" | tail -1)"
no_remount_start idle idle
remount_backing idle
if start; then pass idle-restart; else
	fail idle-restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
served_equals_backing idle-served-equals-backing
if touch "$MNT/idle/a/healed" && [ -e "$SRC/idle/a/healed" ]; then
	pass idle-mutates-after-remount
else
	fail idle-mutates-after-remount "touch idle/a/healed after the remount"
fi

# --- 4. a daemon crash, a refill, then the backing crashes -------------------

ls "$MNT/recrash/a" >/dev/null
touch "$MNT/recrash/a/x"
fd_crash
if start; then pass recrash-daemon-restart; else
	fail recrash-daemon-restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ "$(fd_recovered "$LOG")" -ge 1 ] && [ -e "$MNT/recrash/a/x" ] &&
	[ "$(ls "$MNT/recrash/a" | tr '\n' ' ')" = "keep x " ]; then
	pass recrash-refilled
else
	fail recrash-refilled "recovered $(fd_recovered "$LOG"); recrash/a: $(ls "$MNT/recrash/a" 2>&1 | tr '\n' ' ')"
fi
crash_backing nologflush || fail recrash-crash "the crash failed"
settle_crash
crashed recrash
restart_after_remount recrash
dirty_set_survived recrash
if [ ! -e "$SRC/recrash/a/x" ]; then
	pass recrash-unsynced-lost
else
	fail recrash-unsynced-lost "recrash/a/x survived a crash that commits nothing: the case was not exercised"
fi
served_equals_backing recrash-served-equals-backing

# --- 5. a crash with dirty rows, then a restart without remounting ----------

ls "$MNT/dirty/a" >/dev/null
touch "$MNT/dirty/a/y"
served_names dirty /tmp/pre.names
crash_backing nologflush || fail dirty-crash "the crash failed"
settle_crash
crashed dirty
stop dirty
no_remount_start dirty dirty
remount_backing dirty
if start; then pass dirty-restart; else
	fail dirty-restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
dirty_set_survived dirty
if [ ! -e "$SRC/dirty/a/y" ]; then
	pass dirty-unsynced-lost
else
	fail dirty-unsynced-lost "dirty/a/y survived a crash that commits nothing: the case was not exercised"
fi
served_equals_backing dirty-served-equals-backing

# --- 6. a backing filesystem forced read-only by its own error ---------------

# The backing disk's writes fail; the filesystem meets the error at a
# syncfs and goes read-only by itself (ext4 errors=remount-ro, btrfs's
# transaction abort; xfs shuts down instead). A read-only superblock makes
# syncfs succeed (sync_filesystem returns at once) and the write error is
# reported once per open file, so only the first sync point after it
# fails: the next one must not clear the dirty set either.
stop ro-before
if start --sync_interval_sec=1; then pass ro-start; else
	fail ro-start "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/ro/a" >/dev/null
# A mkdir, not a create: a create's RELEASE reaches dcfs asynchronously and
# could run a sync point (making z durable) before the disk fails.
mkdir "$MNT/ro/a/z"
fault_mode "$FD_BACK" error-writes || fail ro-inject "fault_mode failed"
touch "$SRC/.ro-trigger" 2>/dev/null
sync -f "$SRC" 2>/dev/null
sleep 1
echo "fault_shutdown.sh: ro: the backing mount is now: $(grep " $SRC " /proc/mounts)"
crashed ro
# Two requests, each past the interval: two sync points.
sleep 1.5
ls "$MNT/ro/a" >/dev/null 2>&1
sleep 1.5
ls "$MNT/ro/a" >/dev/null 2>&1
echo "fault_shutdown.sh: ro: sync points: $(grep -c 'sync of the backing filesystems failed' "$LOG") failed"
stop ro
# Not remounted: a superblock read-only under a read-write mount went
# read-only by itself; its memory may hold what its disk never gets, and
# dcfs must refuse it (ext4 marks it "emergency_ro"; xfs shuts down
# instead and fails the open of --source).
forced=$(awk -v src="$SRC" '$5 == src {
	for (i = 7; $i != "-"; i++) {}
	print ($6 !~ /(^|,)ro(,|$)/ && $(i + 3) ~ /(^|,)(emergency_)?ro(,|$)/) ? "yes" : "no"
}' /proc/self/mountinfo)
echo "fault_shutdown.sh: ro: superblock forced read-only: $forced ($(grep " $SRC " /proc/self/mountinfo))"
if start; then
	if [ "$forced" = yes ]; then
		fail ro-no-remount-refused "dcfs started over a backing filesystem forced read-only"
	else
		pass ro-no-remount-refused
	fi
	stop ro-no-remount
else
	DAEMON_PID=""
	umount -l "$MNT" 2>/dev/null || true
	echo "fault_shutdown.sh: ro: dcfs refused to start: $(grep -v -E '^(dcfs/|===| *$)' "$LOG" | tail -1)"
	if [ "$forced" = no ] || grep -q "read-only" "$LOG"; then
		pass ro-no-remount-refused
	else
		fail ro-no-remount-refused "dcfs did not start, for another reason"
	fi
fi
remount_backing ro
if start; then pass ro-restart; else
	fail ro-restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
	pass ro-dirty-set-survived
else
	fail ro-dirty-set-survived "a sync point over the read-only filesystem cleared the dirty set: $(grep -i cleanly "$LOG")"
fi
if [ ! -e "$SRC/ro/a/z" ]; then
	pass ro-unsynced-lost
else
	fail ro-unsynced-lost "ro/a/z survived: the case was not exercised"
fi
served_equals_backing ro-served-equals-backing
if [ -e "$SRC/ro/a/z" ] || [ ! -e "$MNT/ro/a/z" ]; then
	pass ro-lost-name-not-served
else
	fail ro-lost-name-not-served "ro/a/z is served, the backing filesystem lost it"
fi

# Self-check of the comparison: a name added behind dcfs's back, after it
# listed the directory, makes it fail.
ls "$MNT/idle/d" >/dev/null
touch "$SRC/idle/d/behind-its-back"
if fd_same_as_backing; then
	fail comparison-detects-divergence "a name only the backing filesystem has went unnoticed"
else
	pass comparison-detects-divergence
fi
rm "$SRC/idle/d/behind-its-back"

require_no_reclaim no-reclaim
exit "$FAILED"
