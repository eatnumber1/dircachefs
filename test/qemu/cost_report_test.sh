#!/bin/sh
# Host-side self-check of the guest's cost accounting (step 26.17): the real
# functions of guest/init that read what a test cost (the CPU of the processes
# that ran dcfs's code, from the cgroup they were put in) and the program that
# puts them there (/tests/cost_exec.sh, which init writes), over canned cgroup
# files. A reader that read nothing would print 0 for every test and pass
# every budget.
#
# Usage: cost_report_test.sh <guest/init> <guest/lib.sh>
set -eu

INIT=$1
LIB=$2
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# The text of init between "name() {" and its closing brace, and the here-
# document that starts at "<<'MARK'" up to MARK.
function_of() {
	sed -n "/^$2() {\$/,/^}\$/p" "$1"
}
heredoc_of() {
	sed -n "/<<'$1'\$/,/^$1\$/p" "$INIT" | sed '1d;$d'
}

mkdir "$WORK/cg"
COST_CGROUP=$WORK/cg
export COST_CGROUP

CPU=$(function_of "$INIT" cost_cpu_us)
[ -n "$CPU" ] || fail "no cost_cpu_us in $INIT"
eval "$CPU"

printf 'usage_usec 1234567\nuser_usec 1000000\nsystem_usec 234567\n' >"$WORK/cg/cpu.stat"
[ "$(cost_cpu_us)" = 1234567 ] || fail "usage_usec 1234567 read as '$(cost_cpu_us)'"
printf 'user_usec 5\nusage_usec 7000000\n' >"$WORK/cg/cpu.stat"
[ "$(cost_cpu_us)" = 7000000 ] || fail "usage_usec not on the first line read as '$(cost_cpu_us)'"
echo "PASS: the CPU of the cgroup is its usage_usec"

# Scripts call lib.sh's reader; init cannot source lib.sh, so it has its own:
# the same function.
[ "$(function_of "$LIB" cost_cpu_us)" = "$CPU" ] || fail "lib.sh's cost_cpu_us is not init's"
echo "PASS: lib.sh's cost_cpu_us is init's"

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
echo "PASS: all checks passed"
