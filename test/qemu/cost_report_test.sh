#!/bin/sh
# Host-side self-check of the guest's cost accounting (step 26.17): the real
# guest/cost_lib.sh functions that read what a test cost (the CPU of the
# processes that ran dcfs's code, from the cgroup they were put in; the I/O of
# the disks) and the program that puts the processes there (/tests/cost_exec.sh,
# which guest/init writes), over canned cgroup and sysfs files. A reader that
# read nothing would print 0 for every test and pass every budget.
#
# Usage: cost_report_test.sh <guest/init> <guest/cost_lib.sh>
set -eu

INIT=$1
LIB=$2
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# The here-document of init that starts at "<<'MARK'" up to MARK.
heredoc_of() {
	sed -n "/<<'$1'\$/,/^$1\$/p" "$INIT" | sed '1d;$d'
}

mkdir "$WORK/cg"
COST_CGROUP=$WORK/cg
export COST_CGROUP

# shellcheck disable=SC1090 # the path is an argument
. "$LIB"

printf 'usage_usec 1234567\nuser_usec 1000000\nsystem_usec 234567\n' >"$WORK/cg/cpu.stat"
[ "$(cost_cpu_us)" = 1234567 ] || fail "usage_usec 1234567 read as '$(cost_cpu_us)'"
printf 'user_usec 5\nusage_usec 7000000\n' >"$WORK/cg/cpu.stat"
[ "$(cost_cpu_us)" = 7000000 ] || fail "usage_usec not on the first line read as '$(cost_cpu_us)'"
echo "PASS: the CPU of the cgroup is its usage_usec"

# A cgroup that is not there stops the reader: a missing file read as 0 would
# pass every budget.
rm "$WORK/cg/cpu.stat"
if got=$(cost_cpu_us 2>/dev/null); then
	fail "a missing cpu.stat read as '$got' instead of failing"
fi
echo "PASS: a missing cpu.stat is an error, not 0"

# cost_exec.sh: its own pid goes to cgroup.procs, then it becomes the command
# (the pid is the command's, so the caller's $! is the daemon's).
heredoc_of EOF_COST_EXEC >"$WORK/cost_exec.sh"
[ -s "$WORK/cost_exec.sh" ] || fail "no cost_exec.sh here-document in $INIT"
: >"$WORK/cg/cgroup.procs"
got=$(sh "$WORK/cost_exec.sh" sh -c 'echo "ran $$ with $1" "$2"' sh "an arg" "two words")
procs=$(cat "$WORK/cg/cgroup.procs")
[ "$got" = "ran $procs with an arg two words" ] ||
	fail "cost_exec.sh: output '$got', cgroup.procs '$procs' (want the command's own pid and its arguments intact)"
echo "PASS: cost_exec.sh joins the cgroup and becomes the command"

rm -rf "$WORK/cg"
if sh "$WORK/cost_exec.sh" echo ran >"$WORK/out" 2>&1; then
	fail "cost_exec.sh ran its command with no cgroup to join: $(cat "$WORK/out")"
fi
if grep -qx ran "$WORK/out"; then
	fail "cost_exec.sh ran its command although it could not join the cgroup"
fi
echo "PASS: cost_exec.sh does not run its command if the cgroup cannot be joined"
# --- the I/O of the disks (the counters of /sys/block/<disk>/stat, summed) ---


# Fields 1, 3, 5, 7 and 16 of a stat line are the reads and writes completed,
# the sectors read and written and the flush requests completed (Documentation/
# admin-guide/iostats.rst); the others (merges, times, in flight, discards) are
# not counted, and a kernel that has no flush field (before 5.5) is not ours.
# Two disks: vdb with 92 reads, 764 sectors read, 225 writes, 133080 sectors
# written and 136 flushes; vdc the rest.
got=$(cost_sum_disk_stats <<'EOT'
      92        2      764       21      225       91   133080      437        0      328      702        0        0        0        0      136      242
      40        0      308        6        6        0       16        3        0        3       13        0        0        0        0        6        3
EOT
)
[ "$got" = "132 231 1072 133096 142" ] || fail "cost_sum_disk_stats: '$got', want '132 231 1072 133096 142' (reads writes sectors-read sectors-written flushes)"
got=$(cost_sum_disk_stats </dev/null)
[ "$got" = "0 0 0 0 0" ] || fail "cost_sum_disk_stats of no disk: '$got', want zeros"
echo "PASS: the disks' counters are summed field by field"

# The test's I/O is the difference of two readings, one line of name=value.
got=$(cost_disk_delta "10 20 300 4000 5" "13 25 380 4600 9")
[ "$got" = "reads=3 writes=5 sectors_read=80 sectors_written=600 flushes=4" ] || fail "cost_disk_delta: '$got'"
echo "PASS: the I/O of a test is the difference of the counters before and after"

# What the daemon's own tasks caused, from the cgroup's io.stat, for the disks
# of the test only (a device-mapper device stacked on a disk is counted again
# at its own level: the bio is accounted at each queue it passes).
cat >"$WORK/io.stat" <<'EOT'
7:0 rbytes=8192 wbytes=0 rios=2 wios=90 dbytes=0 dios=0
7:1 rbytes=57253888 wbytes=81215488 rios=1555 wios=3353 dbytes=0 dios=0
252:3 rbytes=999999 wbytes=999999 rios=999 wios=999 dbytes=0 dios=0
EOT
mkdir -p "$WORK/cg"
cp "$WORK/io.stat" "$WORK/cg/io.stat"
got=$(cost_cgroup_io "7:0 7:1")
[ "$got" = "dcfs_reads=1557 dcfs_writes=3443 dcfs_read_bytes=57262080 dcfs_write_bytes=81215488" ] || fail "cost_cgroup_io: '$got'"
got=$(cost_cgroup_io "7:0")
[ "$got" = "dcfs_reads=2 dcfs_writes=90 dcfs_read_bytes=8192 dcfs_write_bytes=0" ] || fail "cost_cgroup_io, one disk: '$got'"
: >"$WORK/cg/io.stat"
got=$(cost_cgroup_io "7:0")
[ "$got" = "dcfs_reads=0 dcfs_writes=0 dcfs_read_bytes=0 dcfs_write_bytes=0" ] || fail "cost_cgroup_io, no I/O: '$got'"
echo "PASS: the daemon's I/O counts the test's disks only"
echo "PASS: all checks passed"
