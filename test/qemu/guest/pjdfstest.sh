#!/bin/sh
# dcfs step 4.5 acceptance test: POSIX filesystem-call conformance, measured
# with pjdfstest (github.com/pjd/pjdfstest -- ~8000 checks of chmod/chown/
# link/mkdir/open/rename/unlink/... semantics and errnos, used by ZFS,
# gVisor and FUSE filesystems). See //third_party/pjdfstest:BUILD.pjdfstest
# for how the binary is built (no autoconf; hand-written config.h for
# Linux/glibc) and docs/conformance.md for the full writeup.
#
# The suite (baked into the initramfs at /pjdfstest/, see
# scripts/mkinitramfs.sh) is run twice against two directories on the SAME
# backing ext4 filesystem (/dev/vdb):
#
#   - /mnt/dcfs-work, through the dcfs mount (the thing under test).
#   - /src/ext4-work, directly on the backing ext4 filesystem, bypassing
#     dcfs entirely.
#
# A failure that reproduces on both is ext4/Linux semantics (or a pjdfstest
# quirk on a filesystem/OS combination it doesn't specifically know about --
# tests/conf hard-codes fs="EXT4" via 0001-linux-portability.patch, so both
# runs make identical tests/misc.sh supported()/todo() decisions and a
# side-by-side diff isolates real dcfs behavior differences, not detection
# noise); it is not dcfs's fault and is filtered out automatically by
# diffing the two failure sets. What's left -- failing against dcfs but not
# against raw ext4 -- is the dcfs-specific set, checked against the
# checked-in baseline (pjdfstest.expected_failures, embedded below): any
# dcfs-specific failure NOT already in that baseline is a regression and
# fails this test; any baseline entry that no longer fails is printed as an
# "info:" line so the baseline can be tightened.
#
# Every check is identified as "<path under /pjdfstest/tests, relative>:
# <pjdfstest TAP test number within that file>", e.g. "chmod/00.t:42".
# Numbering restarts at 1 in every .t file (misc.sh's $ntest is reset by
# each file's own fresh shell), so the pair (relative path, number) is only
# meaningful together, never the number alone.
#
# Run as /tests/pjdfstest.sh by guest/init when booted with
# dcfs_test=pjdfstest.sh.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/bin/dcfs
PJD_ROOT=/pjdfstest
TESTS_DIR="$PJD_ROOT/tests"

is_mounted() { grep -q " $1 " /proc/mounts; }
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log

DAEMON_PID=""
MOUNTED=0

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		cat "$LOG" 2>/dev/null
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

echo "pjdfstest.sh: kernel $(uname -r)"

start_daemon() {
	"$DCFS" --source="$SRC" --cache_db="$DB" --allow_other "$MNT" >"$LOG" 2>&1 &
	DAEMON_PID=$!
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

# run_suite ROOT OUTFILE: runs every tests/**/*.t under $TESTS_DIR with cwd
# set to ROOT (a fresh, empty directory on the filesystem under test --
# same layout pjdfstest's own `prove -rv tests` invocation uses: it never
# isolates individual .t files into their own scratch directories, relying
# instead on namegen()'s random names to avoid collisions between files
# run back to back), and appends one "<relative-.t-path>:<n>:ok" or
# "...:notok" line per TAP result line to OUTFILE. Non-TAP lines (each
# file's own "1..N" plan line, or misc.sh's `set -x`-free diagnostics) are
# not matched by the awk patterns below and are dropped.
run_suite() {
	root="$1"
	outfile="$2"
	mkdir -p "$root"
	: >"$outfile"
	(cd "$TESTS_DIR" && find . -name '*.t' | sed 's#^\./##') | sort |
		while IFS= read -r t; do
			(cd "$root" && sh "$TESTS_DIR/$t") 2>>/tmp/pjd-stderr.log |
				awk -v rel="$t" '
					/^ok [0-9]+/     { print rel ":" $2 ":ok" }
					/^not ok [0-9]+/ { print rel ":" $3 ":notok" }
				'
		done >"$outfile"
}

mkdir -p /cache /mnt
if start_daemon; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

mkdir -p "$MNT/dcfs-work" /src/ext4-work

echo "pjdfstest.sh: running suite against dcfs ($MNT/dcfs-work)..."
t0=$(date +%s)
run_suite "$MNT/dcfs-work" /tmp/dcfs_results.txt
t1=$(date +%s)
echo "pjdfstest.sh: dcfs suite took $((t1 - t0))s"

echo "pjdfstest.sh: running suite directly against ext4 ($SRC/ext4-work, no dcfs)..."
t0=$(date +%s)
run_suite /src/ext4-work /tmp/ext4_results.txt
t1=$(date +%s)
echo "pjdfstest.sh: ext4 suite took $((t1 - t0))s"

total_dcfs=$(wc -l </tmp/dcfs_results.txt)
total_ext4=$(wc -l </tmp/ext4_results.txt)

awk -F: '$3 == "notok" { print $1 ":" $2 }' /tmp/dcfs_results.txt | sort -u >/tmp/fail_dcfs.txt
awk -F: '$3 == "notok" { print $1 ":" $2 }' /tmp/ext4_results.txt | sort -u >/tmp/fail_ext4.txt

# set_diff A B OUT: lines present in B but not in A (set difference),
# written to OUT.
#
# NOT implemented as the obvious single `awk 'NR==FNR{a[$0]=1;next}
# !($0 in a)' A B`: that classic idiom silently produces NO output at all
# when A is completely empty (0 lines) -- NR and FNR then stay in lockstep
# for the entirety of B too, so every line of B is wrongly treated as
# still "belonging to A" and skipped -- which is exactly the case an empty
# pjdfstest.expected_failures baseline hits on a clean run. `grep
# -vFxf A B` has the identical problem in busybox grep (1.36.1): an empty
# -f pattern file is treated as one empty-string pattern that matches
# every line, so -v excludes everything instead of nothing. Both were
# caught by testing with a genuinely empty A, not just by checking that
# the command didn't crash.
set_diff() {
	if [ -s "$1" ]; then
		grep -vFxf "$1" "$2" >"$3" || true
	else
		cp "$2" "$3"
	fi
}

# dcfs-specific = fails against dcfs, but the identical check does NOT fail
# directly against ext4 -- i.e. dcfs, not upstream ext4/Linux/pjdfstest
# quirks, is responsible.
set_diff /tmp/fail_ext4.txt /tmp/fail_dcfs.txt /tmp/dcfs_specific.txt

grep -v '^#' "$PJD_ROOT/pjdfstest.expected_failures" 2>/dev/null |
	grep -v '^$' | sort -u >/tmp/expected_clean.txt

set_diff /tmp/expected_clean.txt /tmp/dcfs_specific.txt /tmp/new_regressions.txt
set_diff /tmp/dcfs_specific.txt /tmp/expected_clean.txt /tmp/now_passing.txt

dcfs_failed=$(wc -l </tmp/fail_dcfs.txt)
ext4_failed=$(wc -l </tmp/fail_ext4.txt)
dcfs_specific=$(wc -l </tmp/dcfs_specific.txt)
baseline_n=$(wc -l </tmp/expected_clean.txt)

echo "pjdfstest.sh: SUMMARY dcfs_checks=$total_dcfs dcfs_failed=$dcfs_failed" \
	"ext4_checks=$total_ext4 ext4_failed=$ext4_failed" \
	"dcfs_specific=$dcfs_specific baseline=$baseline_n"

if [ -s /tmp/now_passing.txt ]; then
	while IFS= read -r line; do
		echo "info: $line no longer fails against dcfs; consider removing it from pjdfstest.expected_failures"
	done </tmp/now_passing.txt
fi

if [ -s /tmp/new_regressions.txt ]; then
	echo "pjdfstest.sh: NEW dcfs-specific failure(s) not in the baseline:"
	cat /tmp/new_regressions.txt
	fail pjdfstest-no-regressions "$(wc -l </tmp/new_regressions.txt) new failure(s), see above"
else
	pass pjdfstest-no-regressions
fi

# dcfs (see backing.cc's ReconcileAttrs/VerifyBackingIdentity, step 4.6) logs
# a WARNING for any change on the backing filesystem it didn't make itself
# (an "out-of-band change"). pjdfstest's dcfs suite only ever goes through
# $MNT/dcfs-work -- it never touches /src directly, and /src/ext4-work (the
# ext4-direct suite's own work directory) is a name dcfs has no cached row
# for at all -- so dcfs should see zero such warnings for the entire
# duration of this test. Any that show up are a real dcfs bug (a false
# positive in its own out-of-band detection, most likely), not something
# pjdfstest itself provoked on purpose.
oob_count=$(grep -c "out-of-band" "$LOG" 2>/dev/null || true)
oob_count=${oob_count:-0}
echo "pjdfstest.sh: dcfs stderr out-of-band warning count: $oob_count"
if [ "$oob_count" -gt 0 ]; then
	echo "pjdfstest.sh: unexpected out-of-band warning(s) in dcfs stderr (a dcfs bug -- pjdfstest never touches the backing filesystem outside the mount):"
	grep "out-of-band" "$LOG"
	fail pjdfstest-no-out-of-band "$oob_count warning(s), see dcfs stderr above"
else
	pass pjdfstest-no-out-of-band
fi

exit "$FAILED"
