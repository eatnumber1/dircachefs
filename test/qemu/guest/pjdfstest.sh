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
# backing filesystem (/dev/vdb -- step 5.2: ext4, xfs or btrfs, picked by
# qemu_test_matrix; this script detects which via guest/lib.sh's
# backing_fstype and uses it only to name directories/files and pick the
# right baseline below, never to change what gets run):
#
#   - /mnt/dcfs-work, through the dcfs mount (the thing under test).
#   - /src/<fstype>-work, directly on the backing filesystem, bypassing
#     dcfs entirely.
#
# A failure that reproduces on both is backing-filesystem/Linux semantics
# (or a pjdfstest quirk on a filesystem/OS combination it doesn't
# specifically know about -- tests/conf hard-codes fs="EXT4" via
# 0001-linux-portability.patch regardless of the actual backing filesystem,
# so both runs make identical tests/misc.sh supported()/todo() decisions
# either way and a side-by-side diff isolates real dcfs behavior
# differences, not detection noise); it is not dcfs's fault and is filtered
# out automatically by diffing the two failure sets. What's left -- failing
# against dcfs but not against the raw backing filesystem -- is the
# dcfs-specific set, checked against the checked-in per-filesystem baseline
# (pjdfstest.<fstype>.expected_failures): any dcfs-specific failure NOT
# already in that baseline is a regression and fails this test; any
# baseline entry that no longer fails is printed as an "info:" line so the
# baseline can be tightened.
#
# Every check is identified as "<path under /pjdfstest/tests, relative>:
# <pjdfstest TAP test number within that file>", e.g. "chmod/00.t:42".
# Numbering restarts at 1 in every .t file (misc.sh's $ntest is reset by
# each file's own fresh shell), so the pair (relative path, number) is only
# meaningful together, never the number alone.
#
# Shards (step 6.2): the whole suite takes about 14 minutes in one guest on
# a loaded host (both runs, ~7 minutes each; rename and chown are ~120 s each
# of ~390 s per run, everything else is 1-40 s per directory), so test/qemu
# runs it as three guests, one per shard, each a one-line wrapper script that
# sets PJD_SHARD and sources this file (pjdfstest_rename.sh,
# pjdfstest_chown.sh, pjdfstest_rest.sh; same pattern as idle_short.sh):
#
#   rename: rename/ chmod/
#   chown:  chown/
#   rest:   every other directory (open, unlink, link, truncate, ...)
#
# Assignment is by test directory, fixed in shard_of() below, so it never
# depends on the order or number of files; a directory that is not named
# there lands in "rest", so no check can fall out of every shard. (Step 6.5:
# chmod/ is with rename/, not chown/: on the CI runner chown alone took 185-211 s
# and rename 60 s, and chmod/ is about a fifth of chown's shard.) With
# PJD_SHARD unset the script refuses to run. Each
# shard runs the same two-run comparison on its own directories and uses the
# part of the expected_failures baseline that names them.
#
# Run as /tests/pjdfstest.sh by guest/init when booted with
# dcfs_test=pjdfstest.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/pjdfstest_lib.sh"

DCFS=/bin/dcfs
PJD_ROOT=/pjdfstest
TESTS_DIR="$PJD_ROOT/tests"

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

# --- shards ---------------------------------------------------------------


# shard_of DIR: the shard that runs tests/DIR/.
shard_of() {
	case "$1" in
	rename | chmod) echo rename ;;
	chown) echo chown ;;
	*) echo rest ;;
	esac
}

# in_shard PATH: true if the .t file or "<dir>/<file>.t:<n>" baseline entry
# PATH belongs to this guest's shard.
in_shard() {
	[ "$(shard_of "${1%%/*}")" = "$PJD_SHARD" ]
}

# filter_shard FILE: FILE's lines that in_shard() accepts, on stdout.
filter_shard() {
	while IFS= read -r fs_line; do
		if in_shard "$fs_line"; then echo "$fs_line"; fi
	done <"$1"
}

case "${PJD_SHARD:-}" in
rename | chown | rest) ;;
*)
	fail pjdfstest-shard "PJD_SHARD '${PJD_SHARD:-}' is not rename, chown or rest (run a guest/pjdfstest_<shard>.sh wrapper)"
	exit "$FAILED"
	;;
esac
echo "pjdfstest.sh: shard: $PJD_SHARD"

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
	: >"$outfile.times"
	(cd "$TESTS_DIR" && find . -name '*.t' | sed 's#^\./##') | sort |
		while IFS= read -r t; do
			in_shard "$t" || continue
			t_start=$(cut -d' ' -f1 /proc/uptime)
			(cd "$root" && sh "$TESTS_DIR/$t") 2>>/tmp/pjd-stderr.log |
				tap_results "$t"
			echo "$t $t_start $(cut -d' ' -f1 /proc/uptime)" >>"$outfile.times"
		done >"$outfile"
	# Seconds per test directory, for balancing the shards.
	awk '{ split($1, p, "/"); d[p[1]] += $3 - $2 }
		END { for (k in d) printf "%s=%.0f ", k, d[k]; print "" }' "$outfile.times" |
		sed 's/^/pjdfstest.sh: seconds per directory: /'
}

# The backing filesystem under test: vdb, mounted with no -t so the kernel
# autodetects it -- step 5.2: ext4, xfs or btrfs, per qemu_test_matrix.
# (Before step 4.7 nothing mounted it, so both runs went to the initramfs's
# own tmpfs root -- see docs/conformance.md.)
mount /dev/vdb "$SRC"
FSTYPE=$(backing_fstype "$SRC")
echo "pjdfstest.sh: $SRC is $(stat -f -c %T "$SRC") (detected: $FSTYPE)"

# Several .t files look users up by name (`id -u nobody`, `id -g root`,
# e.g. utimensat/06.t and 07.t); the busybox initramfs has no passwd or
# group database, so those lookups come back empty and the checks that use
# them run with a missing argument and fail on both runs for nothing.
mkdir -p /etc
[ -s /etc/passwd ] || printf '%s\n' 'root:x:0:0:root:/:/bin/sh' \
	'nobody:x:65534:65534:nobody:/:/bin/false' >/etc/passwd
[ -s /etc/group ] || printf '%s\n' 'root:x:0:' 'nobody:x:65534:' >/etc/group

mkdir -p /cache /mnt
if start_daemon "$LOG" --allow_other; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

BACKING_WORK="/src/$FSTYPE-work"
mkdir -p "$MNT/dcfs-work" "$BACKING_WORK"

echo "pjdfstest.sh: running suite against dcfs ($MNT/dcfs-work)..."
t0=$(date +%s)
run_suite "$MNT/dcfs-work" /tmp/dcfs_results.txt
t1=$(date +%s)
echo "pjdfstest.sh: dcfs suite took $((t1 - t0))s"

echo "pjdfstest.sh: running suite directly against $FSTYPE ($BACKING_WORK, no dcfs)..."
t0=$(date +%s)
run_suite "$BACKING_WORK" /tmp/backing_results.txt
t1=$(date +%s)
echo "pjdfstest.sh: $FSTYPE suite took $((t1 - t0))s"

total_dcfs=$(wc -l </tmp/dcfs_results.txt)
total_backing=$(wc -l </tmp/backing_results.txt)

awk -F: '$3 == "notok" { print $1 ":" $2 }' /tmp/dcfs_results.txt | sort -u >/tmp/fail_dcfs.txt
awk -F: '$3 == "notok" { print $1 ":" $2 }' /tmp/backing_results.txt | sort -u >/tmp/fail_backing.txt

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
# directly against the backing filesystem -- i.e. dcfs, not upstream
# backing-filesystem/Linux/pjdfstest quirks, is responsible.
set_diff /tmp/fail_backing.txt /tmp/fail_dcfs.txt /tmp/dcfs_specific.txt

EXPECTED_FAILURES="$PJD_ROOT/pjdfstest.$FSTYPE.expected_failures"
grep -v '^#' "$EXPECTED_FAILURES" 2>/dev/null |
	grep -v '^$' | sort -u >/tmp/expected_all.txt
filter_shard /tmp/expected_all.txt >/tmp/expected_clean.txt

set_diff /tmp/expected_clean.txt /tmp/dcfs_specific.txt /tmp/new_regressions.txt
set_diff /tmp/dcfs_specific.txt /tmp/expected_clean.txt /tmp/now_passing.txt

dcfs_failed=$(wc -l </tmp/fail_dcfs.txt)
backing_failed=$(wc -l </tmp/fail_backing.txt)
dcfs_specific=$(wc -l </tmp/dcfs_specific.txt)
baseline_n=$(wc -l </tmp/expected_clean.txt)

echo "pjdfstest.sh: SUMMARY fstype=$FSTYPE dcfs_checks=$total_dcfs dcfs_failed=$dcfs_failed" \
	"${FSTYPE}_checks=$total_backing ${FSTYPE}_failed=$backing_failed" \
	"dcfs_specific=$dcfs_specific baseline=$baseline_n ($EXPECTED_FAILURES)"

# The reverse direction, for docs/conformance.md: checks that fail directly
# on the backing filesystem but pass through dcfs. Not a failure either
# way (dcfs's kernel side -- the FUSE VFS path with default_permissions --
# may apply a generic rule the backing filesystem's own path does not),
# but printed so the difference stays visible and explained.
set_diff /tmp/fail_dcfs.txt /tmp/fail_backing.txt /tmp/backing_only.txt
echo "pjdfstest.sh: backing_only=$(wc -l </tmp/backing_only.txt)" \
	"(fail directly on $SRC, pass through dcfs)"
while IFS= read -r line; do
	echo "note: $line fails directly on $SRC but passes through dcfs"
done </tmp/backing_only.txt
while IFS= read -r line; do
	echo "dcfs-fail: $line"
done </tmp/fail_dcfs.txt

# See pjdfstest_lib.sh for why a run that fails almost everything on the raw
# backing filesystem must not pass.
if ! pjdfstest_suite_sane "$FSTYPE" /tmp/dcfs_results.txt /tmp/backing_results.txt; then
	head -20 /tmp/pjd-stderr.log
fi

if [ -s /tmp/now_passing.txt ]; then
	while IFS= read -r line; do
		echo "info: $line no longer fails against dcfs; consider removing it from $EXPECTED_FAILURES"
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
# $MNT/dcfs-work -- it never touches /src directly, and $BACKING_WORK (the
# backing-direct suite's own work directory) is a name dcfs has no cached
# row for at all -- so dcfs should see zero such warnings for the entire
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
