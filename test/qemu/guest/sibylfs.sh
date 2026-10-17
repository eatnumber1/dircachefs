#!/bin/sh
# SPDX-License-Identifier: MIT
# dcfs step 12.18b: SibylFS (third_party/sibylfs) as a differential second
# oracle. SibylFS is a 2015 executable specification of POSIX file systems:
# the guest runs scripts of its pinned test suite on a file system with
# fs_test_posix, which prints the trace of the system calls and what they
# returned, and checks the trace with fs_test_check (guest/sibylfs_lib.sh).
#
# Every selected script is run twice, once in a new directory of the dcfs mount
# (the thing under test) and once in a new directory of the backing file system
# directly, and both traces are checked. Where the model rejects the backing
# file system's trace as well, the 2015 model disagrees with today's kernel
# (the suite's own "wontfix" cases among them) and dcfs is not blamed. A script
# whose trace the model accepts on the backing file system and rejects through
# dcfs is a difference between dcfs and the file system under it, and fails
# the test, unless guest/sibylfs.<fstype>.expected_divergences lists it (with
# the reason, in a comment above it).
#
# The scripts are chosen by dcfs_sibylfs=<spec> on the kernel command line, a
# comma-separated list of "adhoc" (the 51 hand-written scripts that fs_test_posix
# can run) and "K/N" (shard K of N of the 21,018 generated ones;
# guest/sibylfs_lib.sh sibylfs_scripts).
# The scripts and the checker are the initramfs's /sibylfs (mkinitramfs.sh).
#
# Run as /tests/sibylfs.sh by guest/init when booted with dcfs_test=sibylfs.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/sibylfs_lib.sh"
require_commands awk find grep id mkdir rm sed sort tar uname wc

DCFS_WORK=/mnt/dcfs-work
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log
OUT=/tmp/sibylfs-out
SIBYLFS_TMP=/tmp/sibylfs-tmp
SIBYLFS_LOG=$OUT/posix.log

DAEMON_PID=""
MOUNTED=0

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

echo "sibylfs.sh: kernel $(uname -r)"

SPEC=$(sed -n 's/.*\bdcfs_sibylfs=\([^ ]*\).*/\1/p' /proc/cmdline)
if [ -z "$SPEC" ]; then
	fail sibylfs-spec "no dcfs_sibylfs=<spec> on the kernel command line (see the header of sibylfs.sh)"
	exit "$FAILED"
fi
echo "sibylfs.sh: scripts: $SPEC"

sibylfs_install_account_commands

mkdir -p /cache "$MNT" "$SRC" "$OUT" "$SIBYLFS_TMP" /tmp/sibylfs-suite
mount /dev/vdb "$SRC"
FSTYPE=$(backing_fstype "$SRC")
echo "sibylfs.sh: $SRC is $FSTYPE"

if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

tar -xzf /sibylfs/test-suite.tar.gz -C /tmp/sibylfs-suite
sibylfs_scripts /tmp/sibylfs-suite "$SPEC" >"$OUT/scripts.txt"
total=$(wc -l <"$OUT/scripts.txt")
echo "sibylfs.sh: $total scripts"

BACKING_WORK="$SRC/$FSTYPE-work"
: >"$OUT/reasons.txt"
mkdir -p "$DCFS_WORK" "$BACKING_WORK"

# run_one N SCRIPT: runs SCRIPT through dcfs and on the backing file system and
# prints "<script> <dcfs verdict> <backing verdict>". What a script where the
# two differ left, the traces and the checker's words, stays in $OUT for the
# report below; a script both reject adds the model's first complaint on the
# backing file system to $OUT/reasons.txt; the rest is removed, with the
# directories the runs made.
run_one() {
	ro_n=$1
	ro_rel=${2#/tmp/sibylfs-suite/*/test-suite/}
	sibylfs_run "$2" "$DCFS_WORK/d$ro_n" "$OUT/d$ro_n.trace"
	sibylfs_forget_accounts
	sibylfs_check "$OUT/d$ro_n.trace" "$OUT/d$ro_n.check"
	ro_d=$?
	sibylfs_run "$2" "$BACKING_WORK/b$ro_n" "$OUT/b$ro_n.trace"
	sibylfs_forget_accounts
	sibylfs_check "$OUT/b$ro_n.trace" "$OUT/b$ro_n.check"
	ro_b=$?
	rm -rf "$DCFS_WORK/d$ro_n" "$BACKING_WORK/b$ro_n"
	if [ "$ro_d" -ne 0 ] && [ "$ro_b" -ne 0 ]; then
		grep -m1 -A1 'unexpected results' "$OUT/b$ro_n.check" | tr '\n' ' ' >>"$OUT/reasons.txt"
		echo >>"$OUT/reasons.txt"
	fi
	if [ "$ro_d" -eq "$ro_b" ] || [ "$ro_b" -ne 0 ]; then
		rm -f "$OUT/d$ro_n.trace" "$OUT/d$ro_n.check" "$OUT/b$ro_n.trace" "$OUT/b$ro_n.check"
	fi
	echo "$ro_rel $ro_d $ro_b"
}

up() { cut -d' ' -f1 /proc/uptime; }
t0=$(up)
n=0
while IFS= read -r script; do
	run_one "$n" "$script"
	n=$((n + 1))
done <"$OUT/scripts.txt" >"$OUT/verdicts.txt"
t1=$(up)
echo "sibylfs.sh: $n scripts took $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.0f", b - a }') s"

sibylfs_regressions "$OUT/verdicts.txt" | sort >"$OUT/regressions.txt"
echo "sibylfs.sh: SUMMARY fstype=$FSTYPE scripts=$total" \
	"dcfs_accepted=$(awk '$2 == 0' "$OUT/verdicts.txt" | wc -l)" \
	"backing_accepted=$(awk '$3 == 0' "$OUT/verdicts.txt" | wc -l)" \
	"both_rejected=$(awk '$2 != 0 && $3 != 0' "$OUT/verdicts.txt" | wc -l)" \
	"accepted_only_through_dcfs=$(awk '$2 == 0 && $3 != 0' "$OUT/verdicts.txt" | wc -l)" \
	"rejected_only_through_dcfs=$(wc -l <"$OUT/regressions.txt")"

# What the model objects to on the backing file system, the 2015 model against
# today's kernel (and the suite's own known failures): the first complaint of
# each script both reject, counted. Its numbers and names are what differ
# between scripts, so a line is reduced to its words.
sed '/^ *$/d; s/[0-9][0-9]*/N/g' "$OUT/reasons.txt" | sort | uniq -c | sort -rn |
	head -n 12 | sed 's/^/sibylfs.sh: rejected on both: /'

if ! sibylfs_suite_sane "$FSTYPE" "$OUT/verdicts.txt"; then
	head -n 20 "$SIBYLFS_LOG"
fi

EXPECTED="/sibylfs/sibylfs.$FSTYPE.expected_divergences"
grep -v '^#' "$EXPECTED" 2>/dev/null | grep -v '^$' | sort -u >"$OUT/expected.txt"
sibylfs_not_in "$OUT/expected.txt" "$OUT/regressions.txt" >"$OUT/new_regressions.txt"
# An entry for a script of another shard says nothing about this run.
awk '{ print $1 }' "$OUT/verdicts.txt" | sort >"$OUT/ran.txt"
sibylfs_in "$OUT/ran.txt" "$OUT/expected.txt" >"$OUT/expected_ran.txt"
sibylfs_not_in "$OUT/regressions.txt" "$OUT/expected_ran.txt" >"$OUT/now_accepted.txt"
while IFS= read -r rel; do
	echo "info: $rel is no longer rejected through dcfs; consider removing it from $EXPECTED"
done <"$OUT/now_accepted.txt"

if [ -s "$OUT/new_regressions.txt" ]; then
	echo "sibylfs.sh: rejected through dcfs, accepted on $FSTYPE, and not in $EXPECTED:"
	while IFS= read -r rel; do
		echo "dcfs-rejected: $rel"
	done <"$OUT/new_regressions.txt"
	fail sibylfs-no-regressions "$(wc -l <"$OUT/new_regressions.txt") script(s), see above"
else
	pass sibylfs-no-regressions
fi

exit "$FAILED"
