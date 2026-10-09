#!/bin/sh
# dcfs step 11.6: the backing filesystem frozen (FIFREEZE, `testutil fsfreeze`):
# what blocks, what does not, and what a freeze leaves behind.
#
# dcfs serves one request at a time (design.md, "Concurrency, today and with
# coroutines"), so what matters is whether the daemon's own thread is held:
#
#   no mutation held   every request dcfs can answer from its cache or by a
#                      read of the backing filesystem is answered (a freeze
#                      stops writes, not reads); a write through FUSE
#                      passthrough blocks its client, in the kernel, not the
#                      daemon;
#   a mutation held    the daemon is inside the mutation's backing syscall
#                      (state D), so the next request it would serve waits
#                      behind it, a read of cached state too: nothing is
#                      served until the thaw. The record is unknown meanwhile
#                      (phase 1), which the database shows (a read-only query
#                      of it while the daemon is held, `testutil sql`), and
#                      the checking build verified its invariants at the
#                      syscall. After the thaw everything completes and what
#                      is served is the backing filesystem's.
#
# A sync point during a freeze, and SIGTERM during one, are measured too.
#
# Every "blocked" check is "still running when the thaw happens, then
# completes after it". The lines `freeze-table:` are the measurements the
# README's table is made from.
#
# Run as /tests/fault_freeze.sh by guest/init when booted with
# dcfs_test=fault_freeze.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
DAEMON_PID=""
MOUNTED=0
PENDING=""
RUN=0
LOGS=""

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	fd_thaw back >/dev/null 2>&1 || true
	for p in $PENDING; do kill -KILL "$p" 2>/dev/null || true; done
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

echo "fault_freeze.sh: kernel $(uname -r)"
require_commands umount sync find stat diff sort md5sum head cat dd tr grep touch mv cut rm mkdir ls sleep kill

# start [flags]: a new run of dcfs.
start() {
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	fd_start "$LOG" --sync_interval_sec=3600 "$@"
}

# quiet: the kernel's caches dropped (without a sync, which would wait on the
# frozen filesystem) and the daemon past the FORGETs it sent, so that the next
# request for a name reaches the daemon and is not answered by the kernel.
quiet() {
	echo 3 >/proc/sys/vm/drop_caches
	quiesce_daemon "$DAEMON_PID"
}

# probe NAME CMD...: CMD in the background; sets PROBE to answered (it ended
# within PROBE_WAIT tenths of a second) or blocked (it did not: its pid is PROBE_PID and goes to
# PENDING), PROBE_RC to its status if it ended; prints the measurement.
probe() {
	pr_name=$1
	shift
	"$@" >/tmp/probe.out 2>&1 &
	PROBE_PID=$!
	pr_n=0
	while [ "$pr_n" -lt "$PROBE_WAIT" ]; do
		kill -0 "$PROBE_PID" 2>/dev/null || break
		sleep 0.1
		pr_n=$((pr_n + 1))
	done
	if kill -0 "$PROBE_PID" 2>/dev/null; then
		PROBE=blocked
		PROBE_RC=""
		PENDING="$PENDING $PROBE_PID"
	else
		PROBE=answered
		wait "$PROBE_PID"
		PROBE_RC=$?
	fi
	echo "freeze-table: $pr_name: $PROBE${PROBE_RC:+ (rc=$PROBE_RC)}; the daemon: $(grep '^State:' "/proc/$DAEMON_PID/status" | tr -s '\t ' ' ') $(fd_where "$DAEMON_PID")"
}

# PROBE_WAIT: tenths of a second a probe is given. A request that is going to be
# answered is answered in milliseconds, but a loaded host or a sanitizer build
# may take longer, and one that is held never returns before the thaw: so what
# is expected to be answered gets 5 s, and what is expected to be held (that
# only has to still be running when the thaw happens) 1 s.
PROBE_WAIT=50

# expect_answered NAME CMD...: a request the daemon can serve while frozen.
expect_answered() {
	ea_name=$1
	PROBE_WAIT=50
	probe "$@"
	if [ "$PROBE" = answered ]; then
		pass "$ea_name"
	else
		fail "$ea_name" "still blocked after 5 s on a frozen backing filesystem"
	fi
}

# thaw_and_wait LABEL PID...: every PID was held by the freeze, so each must
# still be running when the thaw happens (one that had already ended was never
# held: a failure of LABEL), and must end within 10 s after it. Sets TW_ALIVE
# to those that did not.
thaw_and_wait() {
	tw_label=$1
	shift
	TW_ALIVE=""
	for tw_pid in "$@"; do
		if ! kill -0 "$tw_pid" 2>/dev/null; then
			fail "$tw_label-held-at-the-thaw" "pid $tw_pid had already ended before the thaw: it was not held by the freeze"
		fi
	done
	fd_thaw back
	for tw_pid in "$@"; do
		tw_n=0
		while kill -0 "$tw_pid" 2>/dev/null && [ "$tw_n" -lt 100 ]; do
			sleep 0.1
			tw_n=$((tw_n + 1))
		done
		if kill -0 "$tw_pid" 2>/dev/null; then
			TW_ALIVE="$TW_ALIVE $tw_pid"
		else
			wait "$tw_pid" 2>/dev/null
		fi
	done
	PENDING=""
}

# same_as_backing: success if what dcfs serves is what the backing filesystem holds.
same_as_backing() {
	drop_caches
	snapshot "$SRC" atime >/tmp/backing.snap
	snapshot "$MNT" atime >/tmp/served.snap 2>&1
	command diff /tmp/served.snap /tmp/backing.snap >/tmp/snap.diff 2>&1
}
served_equals_backing() {
	if same_as_backing; then
		pass "$1"
	else
		fail "$1" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
}

# sql QUERY: the cache database's answer, rows joined by ";".
sql() { "$TESTUTIL" sql "$DB" "$1" | tr '\n' ';'; }

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
mkdir "$SRC/d1" "$SRC/d2"
echo aaaa >"$SRC/d1/a"
echo bbbbbbb >"$SRC/d1/b"
echo cccc >"$SRC/d2/c"
echo dddd >"$SRC/d2/d"
dd if=/dev/zero of="$SRC/d1/big" bs=1k count=64 2>/dev/null
sync
if start; then pass mount; else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/d1" "$MNT/d2" >/dev/null
stat "$MNT/d1/a" "$MNT/d1/b" "$MNT/d2/c" "$MNT/d1/big" >/dev/null
big_sum=$(md5sum <"$SRC/d1/big")

# The query tool this test looks into the database with must not be able to
# change it (and, being a WAL reader, must see the daemon's commits).
before_dirty=$(sql 'SELECT count(*) FROM dirty')
if "$TESTUTIL" sql "$DB" "DELETE FROM dirty" >/dev/null 2>&1; then
	fail sql-refuses-a-write "testutil sql ran a DELETE"
elif [ "$(sql 'SELECT count(*) FROM dirty')" = "$before_dirty" ]; then
	pass sql-refuses-a-write
else
	fail sql-refuses-a-write "the DELETE changed the dirty set"
fi

# --- no mutation held: what the daemon answers while frozen -----------------

quiet
fd_freeze back || fail freeze "FIFREEZE of the backing filesystem failed"
expect_answered stat-cached-name stat "$MNT/d1/a"
expect_answered readdir-filled-directory ls "$MNT/d1"
expect_answered lookup-absent-in-filled-directory stat "$MNT/d1/nope"
expect_answered read-file-through-passthrough sh -c "md5sum <$MNT/d1/big >/tmp/read.sum"
if [ "$(cat /tmp/read.sum 2>/dev/null)" = "$big_sum" ]; then
	pass read-file-content
else
	fail read-file-content "the contents read while frozen differ"
fi
expect_answered open-for-read head -c 1 "$MNT/d1/b"
expect_answered open-for-write sh -c "exec 3>>$MNT/d1/a; sleep 0.2"
# A write through passthrough goes from the client to the backing file in the
# kernel: the client blocks on the frozen filesystem, the daemon is not
# involved, and what it serves meanwhile is not held.
PROBE_WAIT=10
probe write-through-passthrough sh -c "echo x >>$MNT/d1/a"
write_pid=$PROBE_PID
if [ "$PROBE" = blocked ]; then
	pass write-through-passthrough-blocks-the-client
else
	fail write-through-passthrough-blocks-the-client "the write to a frozen filesystem returned"
fi
# Names the kernel has not looked up since quiet(), so that the daemon is asked.
expect_answered stat-while-a-write-is-held stat "$MNT/d2/c"
expect_answered readdir-while-a-write-is-held ls "$MNT/d2"
thaw_and_wait write-through-passthrough "$write_pid"
if [ -z "$TW_ALIVE" ]; then
	pass write-completes-after-thaw
else
	fail write-completes-after-thaw "still blocked 10 s after the thaw"
fi
if [ "$(cat "$SRC/d1/a")" = "aaaa
x" ]; then
	pass write-landed-on-backing
else
	fail write-landed-on-backing "d1/a: $(cat "$SRC/d1/a")"
fi
served_equals_backing no-mutation-held-served

# --- a mutation held: the daemon is held with it ------------------------------

# check_held NAME CMD STATE-QUERY WANT DIRTY-WANT: the mutation CMD blocks while
# frozen (still running at the thaw), the daemon is in its backing syscall, the
# cache database says what phase 1 left (STATE-QUERY's rows must contain WANT)
# and the dirty set holds a row, a request for a name the kernel has not cached
# waits behind it, and after the thaw both complete and the served tree is the
# backing filesystem's.
check_held() {
	ch_name=$1
	ch_cmd=$2
	ch_query=$3
	ch_want=$4
	ch_exact=${5:-}
	quiet
	fd_freeze back || fail "$ch_name-freeze" "FIFREEZE failed"
	PROBE_WAIT=10
	probe "$ch_name" sh -c "$ch_cmd"
	ch_pid=$PROBE_PID
	# Held in a syscall on the backing filesystem: a descriptor under $SRC, or
	# "/" (the backing filesystem's root, which dcfs reaches an object by).
	if [ "$PROBE" = blocked ] && fd_blocked "$DAEMON_PID" "" &&
		case "$(fd_where "$DAEMON_PID")" in
		*"fd -> $SRC"* | *"fd -> /") true ;;
		*) false ;;
		esac; then
		pass "$ch_name-blocks"
	else
		fail "$ch_name-blocks" "the mutation did not block on a frozen filesystem ($PROBE; $(fd_where "$DAEMON_PID"))"
	fi
	ch_rows=$(sql "$ch_query")
	if [ -n "$ch_exact" ]; then
		ch_ok=0
		[ "$ch_rows" = "$ch_want" ] && ch_ok=1
	else
		ch_ok=0
		case "$ch_rows" in
		*"$ch_want"*) ch_ok=1 ;;
		esac
	fi
	if [ "$ch_ok" -eq 1 ]; then
		pass "$ch_name-record-unknown-while-held"
	else
		fail "$ch_name-record-unknown-while-held" "the database says '$ch_rows', want '$ch_want'"
	fi
	ch_dirty=$(sql 'SELECT count(*) FROM dirty')
	if [ "${ch_dirty%;}" -ge 1 ]; then
		pass "$ch_name-dirty-while-held"
	else
		fail "$ch_name-dirty-while-held" "the dirty set is empty while a mutation is held"
	fi
	PROBE_WAIT=10
	probe "$ch_name-cached-read-behind-it" stat "$MNT/d2/d"
	ch_read=$PROBE
	ch_read_pid=$PROBE_PID
	if [ "$ch_read" = blocked ]; then
		pass "$ch_name-nothing-served-while-held"
	else
		fail "$ch_name-nothing-served-while-held" "a request the daemon had to serve was answered while it was held in a syscall"
	fi
	thaw_and_wait "$ch_name" "$ch_pid" "$ch_read_pid"
	if [ -z "$TW_ALIVE" ]; then
		pass "$ch_name-completes-after-thaw"
	else
		fail "$ch_name-completes-after-thaw" "still blocked 10 s after the thaw:$TW_ALIVE"
	fi
	served_equals_backing "$ch_name-served"
}

check_held create "touch $MNT/d2/new" "SELECT CAST(name AS TEXT), state FROM dentries WHERE state='unknown'" "new	unknown"
check_held mkdir "mkdir $MNT/d2/m" "SELECT CAST(name AS TEXT), state FROM dentries WHERE state='unknown'" "m	unknown"
check_held unlink "rm $MNT/d2/c" "SELECT CAST(name AS TEXT), state FROM dentries WHERE state='unknown'" "c	unknown"
check_held rename "mv $MNT/d1/b $MNT/d1/b2" "SELECT CAST(name AS TEXT), state FROM dentries WHERE state='unknown'" "b	unknown"
b_ino=$(stat -c %i "$SRC/d1/big")
check_held chmod "chmod 600 $MNT/d1/big" "SELECT attrs_valid FROM inodes WHERE backing_ino=$b_ino" "0;" exact
check_held setxattr "$TESTUTIL setxattr $MNT/d1/big user.k v" "SELECT attrs_valid FROM inodes WHERE backing_ino=$b_ino" "0;" exact
check_held truncate "$TESTUTIL truncate $MNT/d1/big 10" "SELECT attrs_valid FROM inodes WHERE backing_ino=$b_ino" "0;" exact
# After the thaw the held records are known again.
if [ -z "$(sql "SELECT 1 FROM dentries WHERE state='unknown' LIMIT 1")" ]; then
	pass no-unknown-name-after-thaw
else
	fail no-unknown-name-after-thaw "unknown dentries remain: $(sql "SELECT CAST(name AS TEXT) FROM dentries WHERE state='unknown'")"
fi

# --- a sync point during a freeze ---------------------------------------------

fd_crash
if start --sync_interval_sec=2; then pass sync-run-mount; else
	fail sync-run-mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
# A mutation (the dirty set holds a row, durably), then the sync interval
# passes with no request, then the freeze: the next request, for a name the
# kernel has not looked up (this mount is new), runs the sync point, which
# makes syncfs(2) of the frozen filesystem.
touch "$MNT/d2/s1"
sleep 3
dirty_before=$(sql 'SELECT count(*) FROM dirty')
fd_freeze back || fail sync-freeze "FIFREEZE failed"
expect_answered sync-point-request-during-freeze stat "$MNT/d2/d"
dirty_after=$(sql 'SELECT count(*) FROM dirty')
expect_answered request-after-the-sync-point stat "$MNT/d1/a"
echo "freeze-table: a sync point during a freeze: dirty rows ${dirty_before%;} before the first request, ${dirty_after%;} after"
if [ "${dirty_before%;}" -ge 1 ] && [ "${dirty_after%;}" -lt "${dirty_before%;}" ]; then
	pass sync-point-ran-while-frozen
else
	fail sync-point-ran-while-frozen "the dirty set did not shrink: ${dirty_before%;} -> ${dirty_after%;}"
fi
fd_thaw back
served_equals_backing sync-point-served

# --- SIGTERM during a freeze ---------------------------------------------------

# Without a held mutation: the shutdown (a last sync point, syncfs of the frozen
# filesystem) does not wait for the thaw.
ls "$MNT/d1" >/dev/null
touch "$MNT/d2/t1"
quiet
fd_freeze back || fail term-freeze "FIFREEZE failed"
kill -TERM "$DAEMON_PID"
term_n=0
while kill -0 "$DAEMON_PID" 2>/dev/null && [ "$term_n" -lt 50 ]; do
	sleep 0.1
	term_n=$((term_n + 1))
done
if kill -0 "$DAEMON_PID" 2>/dev/null; then
	echo "freeze-table: SIGTERM while frozen: the daemon is still alive after 5 s; $(grep '^State:' "/proc/$DAEMON_PID/status" | tr -s '\t ' ' ') $(fd_where "$DAEMON_PID")"
	fail term-frozen-exits "the daemon did not exit within 5 s of SIGTERM on a frozen backing filesystem"
else
	echo "freeze-table: SIGTERM while frozen: the daemon exited within $((term_n * 100)) ms"
	pass term-frozen-exits
fi
wait "$DAEMON_PID" 2>/dev/null
DAEMON_PID=""
MOUNTED=0
fd_thaw back
if start; then pass term-restart; else
	fail term-restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
if grep -q "did not shut down cleanly" "$LOG"; then
	fail term-frozen-clean-shutdown "the shutdown during the freeze was not recorded as clean: $(grep cleanly "$LOG")"
else
	pass term-frozen-clean-shutdown
fi
if [ -e "$MNT/d2/t1" ]; then
	pass term-frozen-mutation-kept
else
	fail term-frozen-mutation-kept "d2/t1 is gone"
fi
served_equals_backing term-frozen-served

# With a mutation held: SIGTERM waits behind it (the daemon is inside the
# syscall), and after the thaw the mutation completes and the daemon exits,
# having recorded what the backing filesystem holds.
quiet
fd_freeze back || fail term-held-freeze "FIFREEZE failed"
PROBE_WAIT=10
probe term-held-create touch "$MNT/d2/held"
held_pid=$PROBE_PID
held_log=$LOG
kill -TERM "$DAEMON_PID"
sleep 2
if kill -0 "$DAEMON_PID" 2>/dev/null; then
	pass term-waits-behind-the-held-mutation
else
	fail term-waits-behind-the-held-mutation "the daemon exited with a mutation held in its syscall"
fi
thaw_and_wait term-held-create "$held_pid"
term_n=0
while kill -0 "$DAEMON_PID" 2>/dev/null && [ "$term_n" -lt 100 ]; do
	sleep 0.1
	term_n=$((term_n + 1))
done
if kill -0 "$DAEMON_PID" 2>/dev/null; then
	fail term-held-exits-after-thaw "the daemon did not exit within 10 s of the thaw"
else
	pass term-held-exits-after-thaw
fi
wait "$DAEMON_PID" 2>/dev/null
DAEMON_PID=""
MOUNTED=0
# The shutdown is clean, or says why it is not: a writable open of the created
# file still outstanding when the loop ended (its RELEASE and the signal race,
# so neither outcome alone is pinned).
if ! grep -q "clean shutdown incomplete" "$held_log"; then
	pass term-held-shutdown-clean-or-says-why
elif grep "clean shutdown incomplete" "$held_log" | grep -q "Dirty cache entries remain"; then
	pass term-held-shutdown-clean-or-says-why
else
	fail term-held-shutdown-clean-or-says-why "an incomplete shutdown without the dirty-entries reason: $(grep 'clean shutdown incomplete' "$held_log")"
fi
if [ -e "$SRC/d2/held" ]; then
	pass term-held-mutation-reached-backing
else
	fail term-held-mutation-reached-backing "d2/held is not on the backing filesystem"
fi
if start; then pass term-held-restart; else
	fail term-held-restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ -e "$MNT/d2/held" ]; then
	pass term-held-name-served
else
	fail term-held-name-served "d2/held was created on the backing filesystem but is not served"
fi
served_equals_backing term-held-served

require_no_reclaim no-reclaim
exit "$FAILED"
