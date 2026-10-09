#!/bin/sh
# Host-side self-check of guest/kernel_mode_lib.sh (step 26.14e): the check that
# says which kernel mode a guest is in must accept a guest in that mode and
# reject one in the other, in both directions, so that quiet_kernel_test (passes
# quiet, fails noisy) and noisy_kernel_test (the reverse) cannot pass for the
# wrong reason. The canned trees are what guest/init and run-qemu.sh leave in a
# guest; the real lib is sourced.
#
# Usage: kernel_mode_test.sh <guest/kernel_mode_lib.sh>
set -eu

LIB=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# shellcheck disable=SC1090 # the path is an argument
. "$LIB"

# tree DIR WRITEBACK EXPIRE LAPTOP PRESSURE CPUS CMDLINE
tree() {
	mkdir -p "$1/proc/sys/vm"
	echo "$2" >"$1/proc/sys/vm/dirty_writeback_centisecs"
	echo "$3" >"$1/proc/sys/vm/dirty_expire_centisecs"
	echo "$4" >"$1/proc/sys/vm/laptop_mode"
	echo "$5" >"$1/proc/sys/vm/vfs_cache_pressure"
	: >"$1/proc/cpuinfo"
	i=0
	while [ "$i" -lt "$6" ]; do
		printf 'processor\t: %d\nmodel name\t: fake\n\n' "$i" >>"$1/proc/cpuinfo"
		i=$((i + 1))
	done
	echo "$7" >"$1/proc/cmdline"
}

tree "$WORK/quiet" 0 8640000 0 100 1 "console=ttyS0 dcfs_accel=kvm dcfs_test=quiet_kernel.sh"
tree "$WORK/noisy" 500 3000 0 100 2 "console=ttyS0 dcfs_accel=kvm dcfs_test=noisy_kernel.sh dcfs_noisy=1"

# expect_ok MODE TREE / expect_bad MODE TREE WORDS
expect_ok() {
	got=$(kernel_mode_problems "$1" "$WORK/$2")
	[ -z "$got" ] || fail "a $2 guest is not accepted as $1: $got"
}
expect_bad() {
	got=$(kernel_mode_problems "$1" "$WORK/$2")
	[ -n "$got" ] || fail "a $2 guest was accepted as $1"
	case "$got" in
	*"$3"*) ;;
	*) fail "a $2 guest rejected as $1 without saying '$3': $got" ;;
	esac
}

expect_ok quiet quiet
expect_ok noisy noisy
echo "PASS: each mode accepts a guest in it"
expect_bad quiet noisy "dirty_writeback_centisecs"
expect_bad quiet noisy "vCPUs"
expect_bad quiet noisy "dcfs_noisy=1"
expect_bad noisy quiet "dirty_writeback_centisecs"
expect_bad noisy quiet "vCPUs"
expect_bad noisy quiet "lacks dcfs_noisy=1"
echo "PASS: each mode rejects a guest in the other (sysctls, vCPUs and command line each say so)"

# One departure at a time from each mode is caught: a mode half in effect is
# not the mode.
tree "$WORK/q1" 500 8640000 0 100 1 "x"
expect_bad quiet q1 "dirty_writeback_centisecs"
tree "$WORK/q2" 0 8640000 0 100 2 "x"
expect_bad quiet q2 "vCPUs"
tree "$WORK/q3" 0 8640000 0 100 1 "x dcfs_noisy=1"
expect_bad quiet q3 "dcfs_noisy=1"
tree "$WORK/n1" 0 3000 0 100 2 "x dcfs_noisy=1"
expect_bad noisy n1 "dirty_writeback_centisecs"
tree "$WORK/n2" 500 3000 0 100 1 "x dcfs_noisy=1"
expect_bad noisy n2 "vCPUs"
tree "$WORK/n3" 500 3000 0 100 2 "x"
expect_bad noisy n3 "lacks dcfs_noisy=1"
tree "$WORK/n4" 500 3000 0 100 2 "x dcfs_noisy=10"
expect_bad noisy n4 "lacks dcfs_noisy=1"
echo "PASS: a mode half in effect is rejected"

[ -n "$(kernel_mode_problems sideways "$WORK/quiet")" ] || fail "an unknown mode was accepted"
echo "PASS: all checks passed"
