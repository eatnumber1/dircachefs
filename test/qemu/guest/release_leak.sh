#!/bin/sh
# dcfs step 4.14 regression test for 2a4f672: Release() must never leak the
# backing file (fd + passthrough registration) when the attribute refresh
# it does on the last writable close fails.
#
# Before 2a4f672, Release() propagated a failed backing::RefreshAttrsFromFd
# via ABSL_RETURN_IF_ERROR *before* decrementing BackingFile::refs -- so on
# a refresh failure, refs never reached 0, and the fd/passthrough
# registration for that inode were never torn down: a real, permanent
# resource leak inside the daemon (see dir_cache_fs.cc's Release()).
#
# RefreshAttrsFromFd's only two failure modes are a bad fd (unreachable
# here: it is dcfs's own still-open backing fd) and a failed cache-database
# write, so this test forces the latter: a plain shell/busybox cannot see
# dcfs's cache database, let alone lock it, so //tools:testutil grows a
# "sqlite-lock" subcommand that opens dcfs's own cache_db file directly via
# libsqlite3 and holds the single WAL writer lock (BEGIN IMMEDIATE) for
# while the daemon closes the file -- forcing dcfs's own attribute-refresh
# write transaction to fail with a genuine SQLITE_BUSY. dcfs sets no busy
# timeout (step 15.6b: a lock that is not free is an immediate error, sqlite.cc),
# so each write transaction that finds the lock held fails at once.
#
# The observable proof of no leak: the number of open fds in the daemon's
# own /proc/<pid>/fd returns to its pre-open baseline after the forced
# failure. Before the fix this stays elevated forever (once leaked, that
# BackingFile entry -- and its fd -- outlives the process).
#
# Run as /tests/release_leak.sh by guest/init when booted with
# dcfs_test=release_leak.sh; prints one "TEST ... PASS/FAIL" line per check
# and exits nonzero if any check failed. init turns that into the final
# ALL-TESTS-PASSED / TEST-FAILED verdict.
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
HOLD_PID=""
LOCK_PID=""

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	[ -n "$HOLD_PID" ] && kill -KILL "$HOLD_PID" 2>/dev/null || true
	[ -n "$HOLD_PID" ] && wait "$HOLD_PID" 2>/dev/null || true
	[ -n "$LOCK_PID" ] && kill -KILL "$LOCK_PID" 2>/dev/null || true
	[ -n "$LOCK_PID" ] && wait "$LOCK_PID" 2>/dev/null || true
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

echo "release_leak.sh: kernel $(uname -r)"

mount /dev/vdb /src
sync

mkdir -p /cache /mnt
# sync_interval_sec is pushed way out so DirCacheFS::MaybeSyncBacking's own
# periodic sync point (called at the start of every request, once 5s have
# passed since the last one) never fires during the sqlite lock window
# below and competes for it -- this test wants only Flush's and Release's
# own attribute-refresh writes contending for that lock, so the timing math
# for how long to hold it stays simple.
if start_daemon "$LOG" --sync_interval_sec=3600; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

before_open=$(daemon_fd_count)

# Open leak-test for writing and hold it open (no close, so no FLUSH/RELEASE
# yet): this allocates the BackingFile (fd + passthrough registration) this
# test is about, via a real writable Open().
"$TESTUTIL" writehold "$MNT/leak-test" create 16 >/tmp/writehold.out 2>&1 &
HOLD_PID=$!
"$TESTUTIL" waitline /tmp/writehold.out READY "$HOLD_PID" || true
if ! grep -q READY /tmp/writehold.out 2>/dev/null; then
	fail writehold-ready "writehold did not get ready: $(cat /tmp/writehold.out)"
	exit "$FAILED"
fi
pass writehold-ready

after_open=$(daemon_fd_count)
if [ "$after_open" -gt "$before_open" ]; then
	pass open-allocates-fd
else
	fail open-allocates-fd "fd count $before_open -> $after_open (expected an increase)"
fi

# Take dcfs's own cache database's single WAL writer lock from outside the
# daemon, across the close of the file: dcfs's own write transactions fail at
# once while it is held (no busy timeout), and RecordWrittenAttrs makes up to
# two of them back to back on a failure (the attribute refresh, then -- since
# that failed -- MarkAttrsUnknown). Flush's own RecordWrittenAttrs runs first,
# then Release's -- for Release's refresh, the one 2a4f672 fixed, to be the one
# that actually fails (not just Flush's), the lock must be held across both,
# which it is from READY until sqlite-lock exits. The lock is let go as soon as
# the Release's failure is in the daemon's log (below), the event the hold is
# for; the 30 s only bounds a lock the test never lets go of.
"$TESTUTIL" sqlite-lock "$DB" 30 >/tmp/sqlite-lock.out 2>&1 &
LOCK_PID=$!
"$TESTUTIL" waitline /tmp/sqlite-lock.out READY "$LOCK_PID" || true
if ! grep -q READY /tmp/sqlite-lock.out 2>/dev/null; then
	fail sqlite-lock-ready "sqlite-lock did not get ready: $(cat /tmp/sqlite-lock.out)"
	exit "$FAILED"
fi
pass sqlite-lock-ready

# Close the held-open file (kernel sends FLUSH then RELEASE for the last
# writable open) while the cache database is locked out from under dcfs.
kill -KILL "$HOLD_PID" 2>/dev/null || true
wait "$HOLD_PID" 2>/dev/null || true
HOLD_PID=""

# Wait for the Release's refresh to have failed under the lock (the warning
# checked below; up to the 30s the lock is held for), then let go of the
# lock (killed: the transaction it holds is rolled back, as its COMMIT of
# nothing would have left it) and give the daemon a moment to finish
# processing FLUSH/RELEASE.
"$TESTUTIL" waitline "$LOG" "Release: could not refresh the attributes" "$DAEMON_PID" || true
kill "$LOCK_PID" 2>/dev/null || true
wait "$LOCK_PID" 2>/dev/null || true
LOCK_PID=""
sleep 2

if grep -q "Release: could not refresh the attributes" "$LOG"; then
	pass release-refresh-failed-as-forced
else
	fail release-refresh-failed-as-forced \
		"no 'Release: could not refresh the attributes' warning in $LOG -- the fault injection did not land"
fi

# Since step 23.6 dcfs keeps one O_PATH descriptor on a written file from
# its last close until the kernel forgets it (design.md, "mmap after
# close"); make the kernel forget it (twice: the dentry LRU's second
# chance), so the count compares the leaked backing file alone.
echo 2 >/proc/sys/vm/drop_caches
echo 2 >/proc/sys/vm/drop_caches
quiesce_daemon "$DAEMON_PID"
after_release=$(daemon_fd_count)
if [ "$after_release" -eq "$before_open" ]; then
	pass no-fd-leak
else
	fail no-fd-leak "fd count before-open=$before_open after-release=$after_release (leaked $((after_release - before_open)) fd(s))"
fi

# The daemon must still be alive and functional: the attributes are left
# unknown by the forced failure, not the process.
if kill -0 "$DAEMON_PID" 2>/dev/null; then
	pass daemon-still-alive
else
	fail daemon-still-alive "daemon exited after the forced refresh failure"
fi

size_mnt=$(stat -c %s "$MNT/leak-test" 2>/dev/null)
if [ "$size_mnt" = 16 ]; then
	pass attrs-repopulate-after-forced-failure
else
	fail attrs-repopulate-after-forced-failure "stat size=$size_mnt, want 16"
fi

require_no_reclaim no-reclaim
exit "$FAILED"
