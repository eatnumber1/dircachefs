#!/bin/sh
# dcfs step 26.4b: how many FUSE requests the kernel sends dcfs for one
# user-level operation (ls -l of 100 entries, find over a tree, stat of a
# path 10 directories deep, cat of a file), each on a cold kernel cache and a
# warm dcfs cache, counted by the checking daemon's cost counter
# (request_lib.sh) and held to guest/request_budgets.txt: a count above its
# budget fails, naming both numbers; raising a budget is a deliberate edit
# whose commit says why.
#
# Run as /tests/request_counts.sh by guest/init when booted with
# dcfs_test=request_counts.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/strace_lib.sh"
. "$(dirname "$0")/request_lib.sh"

BUDGETS="$(dirname "$0")/request_budgets.txt"
DCFS=/bin/dcfs
SRC=/src
DB=/cache/requests.db
MNT=/mnt
LOG=/tmp/dcfs.log
DIR=/tmp/requests
DCFS_COUNTERS_FILE=/cache/counters
export DCFS_COUNTERS_FILE

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		tail -n 50 "$LOG" 2>/dev/null
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

require_commands find stat cat ls awk

mount /dev/vdb "$SRC"
mkdir -p /cache "$DIR" "$SRC/ls" "$SRC/t"
i=0
while [ "$i" -lt 100 ]; do
	echo x >"$SRC/ls/f$i"
	i=$((i + 1))
done
for a in 1 2 3; do
	for b in 1 2 3; do
		mkdir -p "$SRC/t/d$a/e$b"
		for c in 1 2 3 4 5; do echo x >"$SRC/t/d$a/e$b/f$c"; done
	done
done
deep="$SRC/deep"
for d in 1 2 3 4 5 6 7 8 9 10; do deep="$deep/d$d"; done
mkdir -p "$deep"
echo x >"$deep/f"
echo contents >"$SRC/cat.txt"

if start_daemon "$LOG"; then
	pass mount
	MOUNTED=1
else
	fail mount "dcfs did not mount"
	exit 1
fi
# dcfs's cache warm: every directory listed, every name known.
find "$MNT" >/dev/null
cat "$MNT/cat.txt" >/dev/null

# request_op NAME COMMAND...: COMMAND on a cold kernel cache (dcfs's stays
# warm), its requests counted into $DIR/NAME.counts and checked against the
# budgets.
request_op() {
	ro_name=$1
	shift
	drop_caches_quiesced
	cp "$DCFS_COUNTERS_FILE" "$DIR/$ro_name.before"
	"$@" >/dev/null 2>&1 || fail "$ro_name-runs" "$* failed"
	quiesce_daemon "$DAEMON_PID"
	cp "$DCFS_COUNTERS_FILE" "$DIR/$ro_name.after"
	request_delta "$DIR/$ro_name.before" "$DIR/$ro_name.after" >"$DIR/$ro_name.counts"
	echo "  requests $ro_name: $(tr '\n' ' ' <"$DIR/$ro_name.counts")"
	if out=$(strace_budget_compare "$BUDGETS" "$DIR/$ro_name.counts" "$ro_name" request_budgets.txt); then
		pass "budget-$ro_name"
	else
		fail "budget-$ro_name" "$out"
	fi
}

request_op ls-l-100 ls -l "$MNT/ls"
request_op find-tree find "$MNT/t"
request_op stat-deep-10 stat "$MNT${deep#"$SRC"}/f"
request_op cat cat "$MNT/cat.txt"

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
MOUNTED=0
exit "$FAILED"
