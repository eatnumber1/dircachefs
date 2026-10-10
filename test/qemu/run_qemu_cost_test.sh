#!/bin/sh
# Host-side test of run-qemu.sh's cost verdict (step 26.17): the guest's COST
# line (guest/init's cost_report) is held to the budgets of the target under
# test, and the run fails when the daemon's CPU time is past its budget plus
# the tolerance. A fake qemu prints canned serial logs (nothing here needs root
# or a kernel); the real guest/strace_lib.sh compares.
#
# Usage: run_qemu_cost_test.sh <run-qemu.sh> <strace_lib.sh> <cpu_budgets.txt>
set -eu

RUN_QEMU=$1
BUDGET_LIB=$2
REAL_CPU_BUDGETS=$3
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

mkdir "$WORK/bin" "$WORK/out"
cat >"$WORK/bin/qemu" <<'E'
#!/bin/sh
if [ "${1:-}" = --version ]; then
	echo "fake qemu 0"
	exit 0
fi
cat "$(dirname "$0")/../canned"
exit 0
E
for t in mke2fs mkfs-xfs mkfs-btrfs; do
	printf '#!/bin/sh\nexit 0\n' >"$WORK/bin/$t"
done
chmod +x "$WORK"/bin/*
: >"$WORK/kernel"
: >"$WORK/initrd"
: >"$WORK/qboot.rom"
: >"$WORK/mke2fs.conf"

cat >"$WORK/cpu_budgets.txt" <<'EOT'
# a comment
//t:x cpu_us 400000
//t:zero cpu_us 0
EOT

# canned COST-LINE: a guest that passed, with the COST line given ("" for none).
canned() {
	{
		echo "TEST first PASS"
		[ -z "$1" ] || echo "$1"
		echo "MEM total=268435456 min_avail=200000000 peak_used=50000000 peak_shmem=1000000"
		echo "DCFS-TEST-EXIT=0"
	} >"$WORK/canned"
}

# run TARGET [ENV=VALUE... [-- run-qemu.sh flags]]: run-qemu.sh --unit on the
# canned log as the target TARGET, budgets given; sets RC.
run() {
	r_target=$1
	shift
	RC=0
	rm -f "$WORK/out/serial.log"
	env -u DCFS_NOISY -u DCFS_FORCE_TCG TEST_TARGET="$r_target" "$@" TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
		--mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
		--mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
		--budget-lib "$BUDGET_LIB" --cpu-budgets "$WORK/cpu_budgets.txt" \
		"$WORK/kernel" "$WORK/initrd" >"$WORK/stdout" 2>&1 || RC=$?
}

canned "COST cpu_us=400000"
run //t:x
[ "$RC" -eq 0 ] || fail "a run at its budget failed: $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: cost: dcfs CPU 0.400 s (budget 0.400 s, tolerance 100%" "$WORK/stdout" ||
	fail "the cost line does not say the CPU and its budget: $(cat "$WORK/stdout")"
echo "PASS: a run at its CPU budget passes, and says what it used and what it may"

canned "COST cpu_us=800000"
run //t:x
[ "$RC" -eq 0 ] || fail "a run within the tolerance failed: $(cat "$WORK/stdout")"
echo "PASS: a run at the budget plus the 100% tolerance passes"

canned "COST cpu_us=800001"
run //t:x
[ "$RC" -ne 0 ] || fail "a run past budget plus tolerance passed: $(cat "$WORK/stdout")"
grep -q "== RESULT: FAIL (over its CPU budget" "$WORK/stdout" || fail "no FAIL verdict naming the budget: $(cat "$WORK/stdout")"
grep -q "//t:x cpu_us: count rose from 400000 to 800001, past the 100% tolerance" "$WORK/stdout" ||
	fail "the failure does not name the counts: $(cat "$WORK/stdout")"
echo "PASS: a run past its budget plus the tolerance fails, naming both numbers"

# A budget of zero allows nothing: a daemon that was meant not to run.
canned "COST cpu_us=1"
run //t:zero
[ "$RC" -ne 0 ] || fail "CPU against a zero budget passed"
echo "PASS: a zero budget allows no CPU"

# A target with no budget line is not gated, and says so.
canned "COST cpu_us=99000000"
run //t:unlisted
[ "$RC" -eq 0 ] || fail "a target without a budget failed: $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: cost: dcfs CPU 99.000 s (no budget for //t:unlisted" "$WORK/stdout" ||
	fail "no note that the target has no budget: $(cat "$WORK/stdout")"
echo "PASS: a target without a budget line is reported, not gated"

# A target with a budget whose guest printed no COST line fails: the budget
# would otherwise pass without anything having been measured.
canned ""
run //t:x
[ "$RC" -ne 0 ] || fail "a run with a budget and no COST line passed"
grep -q "== RESULT: FAIL (no COST line" "$WORK/stdout" || fail "no FAIL verdict for the missing COST line: $(cat "$WORK/stdout")"
canned ""
run //t:unlisted
[ "$RC" -eq 0 ] || fail "a run with no budget and no COST line failed: $(cat "$WORK/stdout")"
echo "PASS: a missing COST line fails a budgeted target only"

# A COST line that lacks the budgeted count fails too.
canned "COST io_ops=5"
run //t:x
[ "$RC" -ne 0 ] || fail "a COST line without cpu_us passed a CPU budget"
grep -q "//t:x cpu_us: no such count" "$WORK/stdout" || fail "no message for the missing count: $(cat "$WORK/stdout")"
echo "PASS: a COST line without the budgeted count fails"

# The budgets are for the plain build under KVM on the quiet kernel; the
# others report what they used and are not held to them.
canned "COST cpu_us=9999999"
run //t:x DCFS_NOISY=1
[ "$RC" -eq 0 ] || fail "a noisy run was held to the budget: $(cat "$WORK/stdout")"
grep -q "not held to the budget: noisy kernel)" "$WORK/stdout" || fail "no reason for the noisy run: $(cat "$WORK/stdout")"
run //t:x DCFS_FORCE_TCG=1
[ "$RC" -eq 0 ] || fail "a TCG run was held to the budget: $(cat "$WORK/stdout")"
grep -q "not held to the budget: TCG)" "$WORK/stdout" || fail "no reason for the TCG run: $(cat "$WORK/stdout")"
# run_flagged WORD: the run with WORD on the kernel command line.
run_flagged() {
	RC=0
	env -u DCFS_NOISY TEST_TARGET=//t:x TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
		--mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
		--mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
		--budget-lib "$BUDGET_LIB" --cpu-budgets "$WORK/cpu_budgets.txt" \
		--cmdline "$1" "$WORK/kernel" "$WORK/initrd" >"$WORK/stdout" 2>&1 || RC=$?
}
for flagged in "dcfs_sanitizer=asan:asan build" "dcfs_sanitizer=ubsan:ubsan build" "dcfs_cov=/dev/vdz:coverage build"; do
	run_flagged "${flagged%%:*}"
	[ "$RC" -eq 0 ] || fail "a run with ${flagged%%:*} was held to the budget: $(cat "$WORK/stdout")"
	grep -q "not held to the budget: ${flagged#*:})" "$WORK/stdout" ||
		fail "no reason for ${flagged%%:*}: $(cat "$WORK/stdout")"
done
echo "PASS: noisy, TCG and sanitizer runs report their CPU and are not held to the budget"

# The checked-in budgets: every line is OP KIND MAX with a number, the kind a
# COST field, and no target twice (a second line would be ignored by the awk
# that reads the first match last).
awk '
	/^#/ || /^$/ { next }
	NF != 3 || $3 !~ /^[0-9]+$/ { print FILENAME ":" FNR ": not OP KIND MAX: " $0; bad = 1 }
	($1 SUBSEP $2) in seen { print FILENAME ":" FNR ": " $1 " " $2 " twice"; bad = 1 }
	{ seen[$1 SUBSEP $2] = 1 }
	END { exit bad }' "$REAL_CPU_BUDGETS" || fail "cpu_budgets.txt is malformed"
echo "PASS: the checked-in CPU budgets are well formed"
echo "PASS: all checks passed"
