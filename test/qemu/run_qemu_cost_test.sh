#!/bin/sh
# SPDX-License-Identifier: MIT
# Host-side test of run-qemu.sh's cost verdict (step 26.17): the guest's COST
# line (guest/init's cost_report) is held to the budgets of the target under
# test, and the run fails when the daemon's CPU time is past its budget plus
# the tolerance. A fake qemu prints canned serial logs (nothing here needs root
# or a kernel); the real guest/strace_lib.sh compares.
#
# Usage: run_qemu_cost_test.sh <run-qemu.sh> <strace_lib.sh> <cpu_budgets.txt> <io_budgets.txt>
set -eu

RUN_QEMU=$1
BUDGET_LIB=$2
REAL_CPU_BUDGETS=$3
REAL_IO_BUDGETS=$4
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir "$WORK/bin" "$WORK/out"
# The host's own CPU pressure must not decide these verdicts (a run that the
# load flag marks is not held to its CPU budget): every run reads this file,
# whose counter does not move, unless a test points DCFS_PRESSURE_FILE elsewhere.
printf 'some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' >"$WORK/quiet-psi"
cat >"$WORK/bin/qemu" <<'E'
#!/bin/sh
if [ "${1:-}" = --version ]; then
	echo "fake qemu 0"
	exit 0
fi
# A run on a loaded host: the host's pressure counter moves while the guest runs.
[ -f "$(dirname "$0")/../psi.after" ] && cp "$(dirname "$0")/../psi.after" "$(dirname "$0")/../psi"
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
//t:both cpu_us 400000
EOT

cat >"$WORK/io_budgets.txt" <<'EOT'
# a comment
//t:io reads 50
//t:io writes 7
//t:io sectors_read 400
//t:io sectors_written 900
//t:io flushes 30   # observed 27..28
//t:io dcfs_write_bytes 0
//t:io dcfs_reads 752
//t:io dcfs_writes 10
//t:both reads 50
//t:y reads 5
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

# The budget files run() holds a target to: these tests' own, set below, or the
# checked-in ones for the one section that holds the real lines.
CPU_FILE="$WORK/cpu_budgets.txt"
IO_FILE="$WORK/io_budgets.txt"

# run TARGET [ENV=VALUE... [-- run-qemu.sh flags]]: run-qemu.sh --unit on the
# canned log as the target TARGET, budgets given; sets RC.
run() {
  r_target=$1
  shift
  RC=0
  rm -f "$WORK/out/serial.log"
  env -u DCFS_NOISY -u DCFS_FORCE_TCG -u DCFS_PIN_NON_SMT DCFS_PRESSURE_FILE="$WORK/quiet-psi" TEST_TARGET="$r_target" "$@" TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
    sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
    --mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
    --mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
    --budget-lib "$BUDGET_LIB" --cpu-budgets "$CPU_FILE" --io-budgets "$IO_FILE" \
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
grep -q "== RESULT: FAIL (over its cost budget" "$WORK/stdout" || fail "no FAIL verdict naming the budget: $(cat "$WORK/stdout")"
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
grep -q "run-qemu.sh: cost: dcfs CPU 99.000 s (no line for //t:unlisted in cpu_budgets.txt)" "$WORK/stdout" ||
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
grep -q "not held to the budgets: noisy kernel)" "$WORK/stdout" || fail "no reason for the noisy run: $(cat "$WORK/stdout")"
run //t:x DCFS_FORCE_TCG=1
[ "$RC" -eq 0 ] || fail "a TCG run was held to the budget: $(cat "$WORK/stdout")"
grep -q "not held to the budgets: TCG)" "$WORK/stdout" || fail "no reason for the TCG run: $(cat "$WORK/stdout")"
# run_flagged WORD: the run with WORD on the kernel command line.
run_flagged() {
  RC=0
  env -u DCFS_NOISY DCFS_PRESSURE_FILE="$WORK/quiet-psi" TEST_TARGET=//t:x TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
    sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
    --mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
    --mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
    --budget-lib "$BUDGET_LIB" --cpu-budgets "$WORK/cpu_budgets.txt" --io-budgets "$WORK/io_budgets.txt" \
    --cmdline "$1" "$WORK/kernel" "$WORK/initrd" >"$WORK/stdout" 2>&1 || RC=$?
}
for flagged in "dcfs_sanitizer=asan:asan build" "dcfs_sanitizer=ubsan:ubsan build" "dcfs_cov=/dev/vdz:coverage build"; do
  run_flagged "${flagged%%:*}"
  [ "$RC" -eq 0 ] || fail "a run with ${flagged%%:*} was held to the budget: $(cat "$WORK/stdout")"
  grep -q "not held to the budgets: ${flagged#*:})" "$WORK/stdout" ||
    fail "no reason for ${flagged%%:*}: $(cat "$WORK/stdout")"
done
echo "PASS: noisy, TCG and sanitizer runs report their CPU and are not held to the budget"

# The console ends its lines with a carriage return: the last count of the COST
# line has one in the serial log, and must not be printed or compared with it.
{
  echo "TEST first PASS"
  printf 'COST cpu_us=400000 %s\r\n' "reads=50 writes=7 sectors_read=400 sectors_written=900 flushes=30 dcfs_reads=752 dcfs_writes=10 dcfs_read_bytes=4096 dcfs_write_bytes=0"
  echo "MEM total=268435456 min_avail=200000000 peak_used=50000000 peak_shmem=1000000"
  echo "DCFS-TEST-EXIT=0"
} >"$WORK/canned"
run //t:both
[ "$RC" -eq 0 ] || fail "a COST line that ends with a carriage return failed: $(cat "$WORK/stdout")"
if grep '^run-qemu.sh: cost:' "$WORK/stdout" | grep -q "$(printf '\r')"; then fail "the carriage return reached the cost line: $(grep '^run-qemu.sh: cost:' "$WORK/stdout" | cat -A)"; fi
grep -q "(0 bytes) (within the budgets in io_budgets.txt)" "$WORK/stdout" || fail "the last count of the line is not read: $(cat "$WORK/stdout")"
echo "PASS: the carriage return the console adds to the COST line is dropped"

# --- I/O (guest/io_budgets.txt: counts) -------------------------

IO_OK="reads=50 writes=7 sectors_read=400 sectors_written=900 flushes=30 dcfs_reads=752 dcfs_writes=10 dcfs_read_bytes=4096 dcfs_write_bytes=0"
canned "COST cpu_us=400000 $IO_OK"
run //t:io
[ "$RC" -eq 0 ] || fail "a run at its I/O budgets failed: $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: cost: disk I/O 50 reads, 7 writes, 400 sectors read, 900 sectors written, 30 flushes; dcfs caused 752 reads (4096 bytes), 10 writes (0 bytes) (within the budgets in io_budgets.txt)" "$WORK/stdout" ||
  fail "the cost line does not say the I/O and the budget: $(cat "$WORK/stdout")"
echo "PASS: a run at its I/O budgets passes, and says what it did"

# One over, in each exact kind that has a budget (flushes and the bytes): fails,
# naming both numbers.
for over in flushes:31 dcfs_write_bytes:1; do
  canned "COST cpu_us=400000 $(echo "$IO_OK" | sed "s/${over%%:*}=[0-9]*/${over%%:*}=${over#*:}/")"
  run //t:io
  [ "$RC" -ne 0 ] || fail "${over%%:*} one past its budget passed: $(cat "$WORK/stdout")"
  grep -q "== RESULT: FAIL (over its cost budget" "$WORK/stdout" || fail "no FAIL verdict for ${over%%:*}: $(cat "$WORK/stdout")"
  grep -q "^//t:io ${over%%:*}: count rose from [0-9]* to ${over#*:}; raising a budget is a deliberate edit of io_budgets.txt whose commit says why" "$WORK/stdout" ||
    fail "the failure for ${over%%:*} does not name the counts: $(cat "$WORK/stdout")"
done
echo "PASS: an I/O count one past its budget fails, naming both numbers"

# Interim headroom (26.17c): the disk-request counts move with the host's load
# (writeback and readahead timing), so reads, writes, sectors_read,
# sectors_written, dcfs_reads and dcfs_writes get +2% rounded up (at least +1);
# every other count stays exact. Allowed: reads 50 -> 51, writes 7 -> 8,
# sectors_read 400 -> 408, sectors_written 900 -> 918, dcfs_reads 752 -> 768,
# dcfs_writes 10 -> 11.
IO_HEAD=$(echo "$IO_OK" | sed 's/^reads=50/reads=51/; s/ writes=7/ writes=8/; s/sectors_read=400/sectors_read=408/; s/sectors_written=900/sectors_written=918/; s/dcfs_reads=752/dcfs_reads=768/; s/dcfs_writes=10 /dcfs_writes=11 /')
canned "COST cpu_us=400000 $IO_HEAD"
run //t:io
[ "$RC" -eq 0 ] || fail "counts at their headroom failed: $(cat "$WORK/stdout")"
for over in reads:52 writes:9 sectors_read:409 sectors_written:919 dcfs_reads:769 dcfs_writes:12; do
  canned "COST cpu_us=400000 $(echo "$IO_HEAD" | sed "s/\(^\| \)${over%%:*}=[0-9]*/\1${over%%:*}=${over#*:}/")"
  run //t:io
  [ "$RC" -ne 0 ] || fail "${over%%:*} past its headroom passed: $(cat "$WORK/stdout")"
  grep -q "^//t:io ${over%%:*}: count rose from [0-9]* to ${over#*:}, past the 2% headroom (at least 1); raising a budget" "$WORK/stdout" ||
    fail "the failure for ${over%%:*} does not name the counts and the headroom: $(cat "$WORK/stdout")"
done
canned "COST cpu_us=400000 $(echo "$IO_HEAD" | sed 's/flushes=30/flushes=31/')"
run //t:io
[ "$RC" -ne 0 ] || fail "a deterministic count one over passed beside the headroom: $(cat "$WORK/stdout")"
echo "PASS: the disk-request counts get 2% headroom (at least 1); flushes and the byte counts stay exact"

# A count with no line in the file is not held (dcfs_read_bytes has none here).
canned "COST cpu_us=400000 $(echo "$IO_OK" | sed 's/dcfs_read_bytes=4096/dcfs_read_bytes=40960000/')"
run //t:io
[ "$RC" -eq 0 ] || fail "an I/O count without a budget line failed: $(cat "$WORK/stdout")"
echo "PASS: only the kinds that have a line are held"

# The CPU and the I/O are held separately, and both are reported.
canned "COST cpu_us=800001 $(echo "$IO_OK" | sed 's/reads=50/reads=52/')"
run //t:both
[ "$RC" -ne 0 ] || fail "CPU and I/O both over passed"
grep -q "^//t:both cpu_us: count rose" "$WORK/stdout" && grep -q "^//t:both reads: count rose" "$WORK/stdout" ||
  fail "both failures are not reported: $(cat "$WORK/stdout")"
echo "PASS: CPU and I/O over their budgets are both reported"

# A COST line without the I/O counts fails a target with an I/O budget.
canned "COST cpu_us=400000"
run //t:io
[ "$RC" -ne 0 ] || fail "a COST line without I/O counts passed an I/O budget"
grep -q "//t:io reads: no such count" "$WORK/stdout" || fail "no message for the missing I/O counts: $(cat "$WORK/stdout")"
echo "PASS: a COST line without the budgeted I/O counts fails"

# A target with a CPU budget and none for I/O (and the reverse) is held by the
# one it has; one with neither is reported.
canned "COST cpu_us=400000 reads=999999"
run //t:zero
[ "$RC" -ne 0 ] || fail "//t:zero (a CPU budget of 0, none for I/O) passed 400000 CPU"
canned "COST cpu_us=99999999 reads=5"
run //t:y
[ "$RC" -eq 0 ] || fail "a target with only an I/O budget was held to CPU: $(cat "$WORK/stdout")"
grep -q "no line for //t:y in cpu_budgets.txt" "$WORK/stdout" || fail "no note that //t:y has no CPU budget: $(cat "$WORK/stdout")"
echo "PASS: a target is held by the budget files that have a line for it"

# The same runs that are not held to the CPU budgets are not held to the I/O
# ones (noisy writeback changes the I/Os, TCG and the sanitizers their timing).
canned "COST cpu_us=1 reads=99999"
run //t:io DCFS_NOISY=1
[ "$RC" -eq 0 ] || fail "a noisy run was held to the I/O budget: $(cat "$WORK/stdout")"
grep -q "disk I/O .*not held to the budgets: noisy kernel" "$WORK/stdout" || fail "no reason for the noisy run's I/O: $(cat "$WORK/stdout")"
echo "PASS: runs that are not held to the CPU budgets are not held to the I/O budgets"

# A target whose budget says it reads, and whose guest counted no read, did not
# count (mounting a disk reads it); one with no such budget may have no read (a
# test that declares a disk and never touches it).
canned "COST cpu_us=1 reads=0 writes=0 sectors_read=0 sectors_written=0 flushes=0"
run //t:io
[ "$RC" -ne 0 ] || fail "a COST line of zeros for a target that reads passed"
grep -q "== RESULT: FAIL (no I/O counted" "$WORK/stdout" || fail "no FAIL verdict for the uncounted I/O: $(cat "$WORK/stdout")"
run //t:unlisted
[ "$RC" -eq 0 ] || fail "a COST line of zeros for a target that has no I/O budget failed: $(cat "$WORK/stdout")"
echo "PASS: a target with a budget for reads whose guest counted none fails"

# --- the load-starvation flag (the host's CPU pressure over the run's wall time) ---
#
# A fake date makes the run last 10 s (its first call is the start, its second
# the end), and the host's pressure file (DCFS_PRESSURE_FILE: /proc/pressure/cpu,
# or a container's cgroup's cpu.pressure) says the host had a task waiting for a
# CPU for the given microseconds while it ran (the fake qemu moves the counter).
mkdir "$WORK/clock"
cat >"$WORK/clock/date" <<'E_DATE'
#!/bin/sh
d=$(dirname "$0")
n=$(cat "$d/n" 2>/dev/null || echo 0)
echo $((n + 1)) >"$d/n"
if [ "$n" = 0 ]; then echo 1000.000000000; else echo "$((1000 + $(cat "$d/wall"))).000000000"; fi
E_DATE
chmod +x "$WORK/clock/date"
pressure_file() { # TOTAL_US: a /proc/pressure/cpu with that total
  printf 'some avg10=0.00 avg60=0.00 avg300=0.00 total=%s\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' "$1"
}
# run_loaded BEFORE_US AFTER_US [TARGET]: the run lasted 10 s and the counter
# moved so; TARGET (default //t:unlisted) is the target under test.
run_loaded() {
  rm -f "$WORK/clock/n"
  echo 10 >"$WORK/clock/wall"
  pressure_file "$1" >"$WORK/psi"
  pressure_file "$2" >"$WORK/psi.after"
  run "${3:-//t:unlisted}" DCFS_PRESSURE_FILE="$WORK/psi" PATH="$WORK/clock:$PATH"
  rm -f "$WORK/psi.after"
}

canned "COST cpu_us=1"
run_loaded 5000000 8000000
[ "$RC" -eq 0 ] || fail "a starved run failed (the flag is a diagnostic): $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: time: guest wall 10.0 s, .*host CPU pressure 3.0 s (30% of the wall)" "$WORK/stdout" ||
  fail "the time line does not say the pressure: $(cat "$WORK/stdout")"
grep -q "^run-qemu.sh: load-starved: " "$WORK/stdout" || fail "no load-starved line at 30%: $(cat "$WORK/stdout")"
grep -q "^== RESULT: PASS .*\[load-starved\] ==" "$WORK/stdout" || fail "the verdict does not say load-starved: $(cat "$WORK/stdout")"
echo "PASS: a run that lost 30% of its wall time to a loaded host says so in the time line and the verdict"

# The threshold is 20%: 19% is not starved, 20% is.
run_loaded 0 1900000
grep -q "(19% of the wall)" "$WORK/stdout" || fail "19% not reported: $(cat "$WORK/stdout")"
if grep -q "load-starved" "$WORK/stdout"; then fail "19% was flagged: $(cat "$WORK/stdout")"; fi
run_loaded 0 2000000
grep -q "^== RESULT: PASS .*\[load-starved\] ==" "$WORK/stdout" || fail "20% was not flagged: $(cat "$WORK/stdout")"
echo "PASS: the flag starts at 20% of the wall time"

# russ, 2026-10-17: "in general for all our tests I'm interested in them being
# load-agnostic": CPU time stretches with the host's load, so a run the flag
# marks is not held to its CPU budget; the number is printed with the reason,
# as for a sanitizer or TCG run. The I/O counts do not move with load and stay
# held, and a run that is not starved is held as before.
canned "COST cpu_us=800001"
run_loaded 0 3000000 //t:x
[ "$RC" -eq 0 ] || fail "a load-starved run was held to its CPU budget: $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: cost: dcfs CPU 0.800 s (not held to the budgets: host load-starved)" "$WORK/stdout" ||
  fail "the cost line does not say the CPU budget is not held for the load: $(cat "$WORK/stdout")"
run_loaded 0 1900000 //t:x
[ "$RC" -ne 0 ] || fail "a run below the starvation threshold was not held to its CPU budget"
canned "COST cpu_us=400000 $(echo "$IO_OK" | sed 's/reads=50/reads=52/')"
run_loaded 0 3000000 //t:io
[ "$RC" -ne 0 ] || fail "a load-starved run was not held to its I/O budget: $(cat "$WORK/stdout")"
grep -q "^//t:io reads: count rose from 50 to 52" "$WORK/stdout" || fail "the I/O failure is not reported: $(cat "$WORK/stdout")"
echo "PASS: a load-starved run is not held to its CPU budget, and is held to its I/O budgets"

canned "COST cpu_us=1"
# A failing run is tagged too, and so is one that has no MEM line (a guest that
# was killed by the time limit says nothing about itself).
printf 'TEST first FAIL (x)\nDCFS-TEST-EXIT=1\n' >"$WORK/canned"
run_loaded 0 9000000
[ "$RC" -ne 0 ] || fail "a failing guest passed"
grep -q "no MEM line" "$WORK/stdout" || fail "the guest without a MEM line was not reported: $(cat "$WORK/stdout")"
grep -q "^== RESULT: FAIL (see .*) \[load-starved\] ==" "$WORK/stdout" || fail "a starved failing run is not tagged: $(cat "$WORK/stdout")"
echo "PASS: a failing run, and one with no MEM line, carries the flag too"

# A host without pressure information has no flag and says so.
canned "COST cpu_us=1"
rm -f "$WORK/clock/n"
echo 10 >"$WORK/clock/wall"
run //t:unlisted DCFS_PRESSURE_FILE="$WORK/no-such-file" PATH="$WORK/clock:$PATH"
[ "$RC" -eq 0 ] || fail "a host without pressure information failed the run: $(cat "$WORK/stdout")"
grep -q "host CPU pressure not available" "$WORK/stdout" || fail "no note that the pressure is not available: $(cat "$WORK/stdout")"
if grep -q "load-starved" "$WORK/stdout"; then fail "a run was flagged without pressure information"; fi
echo "PASS: without pressure information there is no flag, and the time line says so"

# --- the hang guard (the guest's wall allowance against the budgets) -----------
#
# //t:both has a CPU budget of 0.4 s (0.8 s with the tolerance) and 50 reads: 3 x
# (0.8 s + 50 x 2 ms) = 2.7 s, rounded up to 3 s.
canned "COST cpu_us=400000 $IO_OK"
run //t:both TIMEOUT=3
[ "$RC" -eq 0 ] || fail "a wall allowance of 3 s was refused for a budget that needs 3: $(cat "$WORK/stdout")"
run //t:both TIMEOUT=2
[ "$RC" -ne 0 ] || fail "a wall allowance of 2 s was accepted for a budget that needs 3"
grep -q "== RESULT: FAIL (timeout shorter than the budgets need" "$WORK/stdout" || fail "no FAIL verdict for the short timeout: $(cat "$WORK/stdout")"
grep -q "wall allowance, 2 s, is shorter than the 3 s that //t:both's budgets need" "$WORK/stdout" ||
  fail "the refusal does not name the allowance and what is needed: $(cat "$WORK/stdout")"
if grep -q "guest memory" "$WORK/stdout"; then fail "the guest was booted although its timeout was refused"; fi
echo "PASS: a wall allowance shorter than 3 x (CPU budget + tolerance + I/Os x 2 ms + flushes x 20 ms) is refused before the guest boots"

# The flush term (30 flushes x 20 ms), and a target without budgets.
run //t:io TIMEOUT=2
[ "$RC" -ne 0 ] || fail "a wall allowance of 2 s was accepted for 30 flushes and 50 reads (3 x 0.7 s = 2.1 s, 3 s)"
run //t:io TIMEOUT=3
[ "$RC" -eq 0 ] || fail "a wall allowance of 3 s was refused for 30 flushes and 50 reads: $(cat "$WORK/stdout")"
run //t:unlisted TIMEOUT=1
[ "$RC" -eq 0 ] || fail "a target without budgets was refused a short timeout: $(cat "$WORK/stdout")"
echo "PASS: flushes count 20 ms each, and a target without budgets has no floor"

# --- CPU affinity (non-SMT pinning) -------------------------------------------
#
# Off by default: QEMU runs unpinned unless DCFS_PIN_NON_SMT=1 is set.
canned "COST cpu_us=400000"
run //t:x
[ "$RC" -eq 0 ] || fail "a default unpinned run failed: $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: CPU affinity: unpinned" "$WORK/out/serial.log" ||
  fail "default run not logged as unpinned: $(cat "$WORK/out/serial.log")"

# DCFS_PIN_NON_SMT=0 is unpinned.
run //t:x DCFS_PIN_NON_SMT=0
[ "$RC" -eq 0 ] || fail "an unpinned run with DCFS_PIN_NON_SMT=0 failed: $(cat "$WORK/stdout")"
grep -q "run-qemu.sh: CPU affinity: unpinned" "$WORK/out/serial.log" ||
  fail "affinity not logged as unpinned with DCFS_PIN_NON_SMT=0: $(cat "$WORK/out/serial.log")"
# DCFS_PIN_NON_SMT=1 pins QEMU to one logical CPU per physical core.
run //t:x DCFS_PIN_NON_SMT=1
[ "$RC" -eq 0 ] || fail "a pinned run failed: $(cat "$WORK/stdout")"
if command -v taskset >/dev/null 2>&1 && [ -d /sys/devices/system/cpu ]; then
  grep -q "run-qemu.sh: CPU affinity: pinned to non-SMT cores" "$WORK/out/serial.log" ||
    fail "affinity not logged as pinned with DCFS_PIN_NON_SMT=1: $(cat "$WORK/out/serial.log")"
fi

# Synthetic sysfs parsing with DCFS_PIN_NON_SMT=1: thread siblings 0-1 and 2-3 yield 0,2.
mkdir -p "$WORK/fake_cpu/cpu0/topology" "$WORK/fake_cpu/cpu1/topology" \
  "$WORK/fake_cpu/cpu2/topology" "$WORK/fake_cpu/cpu3/topology"
printf "0-1\n" >"$WORK/fake_cpu/cpu0/topology/thread_siblings_list"
printf "0-1\n" >"$WORK/fake_cpu/cpu1/topology/thread_siblings_list"
printf "2-3\n" >"$WORK/fake_cpu/cpu2/topology/thread_siblings_list"
printf "2-3\n" >"$WORK/fake_cpu/cpu3/topology/thread_siblings_list"
run //t:x DCFS_PIN_NON_SMT=1 DCFS_CPU_SYSFS="$WORK/fake_cpu"
[ "$RC" -eq 0 ] || fail "a synthetic sysfs run failed: $(cat "$WORK/stdout")"
if command -v taskset >/dev/null 2>&1; then
  grep -q "run-qemu.sh: CPU affinity: pinned to non-SMT cores (0,2)" "$WORK/out/serial.log" ||
    fail "synthetic affinity not logged as pinned to 0,2: $(cat "$WORK/out/serial.log")"
fi
echo "PASS: non-SMT pinning is off by default, enabled by DCFS_PIN_NON_SMT=1"

# --- a shard whose work includes a duration, against the checked-in budgets ---
#
# xfstests_1_test_ext4 runs generic/364, which loops direct writes and fsyncs
# for 10 s (`timeout 10s`): the requests it issues are a rate, not work. The
# shard issues thousands more writes on a quiet host than under host pressure,
# so the write-side lines are absent and both quiet and loaded COST lines are
# held to the remaining budgets. (The 3540 s is what the target's `eternal`
# timeout leaves the guest.)
XFSTESTS_QUIET="COST cpu_us=8269532 reads=2153 writes=57097"
XFSTESTS_QUIET="$XFSTESTS_QUIET sectors_read=963214 sectors_written=4546248"
XFSTESTS_QUIET="$XFSTESTS_QUIET flushes=15620 dcfs_reads=18 dcfs_writes=14896"
XFSTESTS_QUIET="$XFSTESTS_QUIET dcfs_read_bytes=73728"
XFSTESTS_QUIET="$XFSTESTS_QUIET dcfs_write_bytes=24764416"
XFSTESTS_LOADED="COST cpu_us=19163765 reads=2121 writes=13965"
XFSTESTS_LOADED="$XFSTESTS_LOADED sectors_read=952942 sectors_written=4483072"
XFSTESTS_LOADED="$XFSTESTS_LOADED flushes=1878 dcfs_reads=14 dcfs_writes=2435"
XFSTESTS_LOADED="$XFSTESTS_LOADED dcfs_read_bytes=57344"
XFSTESTS_LOADED="$XFSTESTS_LOADED dcfs_write_bytes=19111936"
CPU_FILE=$REAL_CPU_BUDGETS
IO_FILE=$REAL_IO_BUDGETS
awk '
	/^#/ || /^$/ { next }
	$1 == "//test/qemu:xfstests_1_test_ext4" &&
	$2 ~ /^(writes|sectors_written|flushes|dcfs_writes|dcfs_write_bytes)$/ {
		print FILENAME ":" FNR ": " $1 " has write-side budget line: " $0
		bad = 1
	}
	END { exit bad }' "$REAL_IO_BUDGETS" ||
  fail "xfstests_1_test_ext4 must not have write-side lines in io_budgets.txt"
echo "PASS: xfstests_1_test_ext4 has no write-side lines in io_budgets.txt"
for line in "$XFSTESTS_QUIET" "$XFSTESTS_LOADED"; do
  canned "$line"
  run //test/qemu:xfstests_1_test_ext4 TIMEOUT=3540
  if [ "$RC" -ne 0 ]; then
    fail "xfstests_1_test_ext4 over budgets: $(cat "$WORK/stdout")"
  fi
done
echo "PASS: xfstests_1_test_ext4 is within budgets on quiet and loaded hosts"

# The checked-in budgets: every line is OP KIND MAX with a number, the kind a
# COST field, and no target twice (a second line would be ignored by the awk
# that reads the first match last).
awk '
	/^#/ || /^$/ { next }
	NF != 3 || $3 !~ /^[0-9]+$/ { print FILENAME ":" FNR ": not OP KIND MAX: " $0; bad = 1 }
	$2 != "cpu_us" { print FILENAME ":" FNR ": " $2 " is not cpu_us"; bad = 1 }
	($1 SUBSEP $2) in seen { print FILENAME ":" FNR ": " $1 " " $2 " twice"; bad = 1 }
	{ seen[$1 SUBSEP $2] = 1 }
	END { exit bad }' "$REAL_CPU_BUDGETS" || fail "cpu_budgets.txt is malformed"
echo "PASS: the checked-in CPU budgets are well formed"
awk '
	/^#/ || /^$/ { next }
	NF < 3 || $3 !~ /^[0-9]+$/ { print FILENAME ":" FNR ": not OP KIND MAX: " $0; bad = 1 }
	$2 !~ /^(reads|writes|sectors_read|sectors_written|flushes|dcfs_reads|dcfs_writes|dcfs_read_bytes|dcfs_write_bytes)$/ { print FILENAME ":" FNR ": " $2 " is not a COST field"; bad = 1 }
	($1 SUBSEP $2) in seen { print FILENAME ":" FNR ": " $1 " " $2 " twice"; bad = 1 }
	{ seen[$1 SUBSEP $2] = 1 }
	END { exit bad }' "$REAL_IO_BUDGETS" || fail "io_budgets.txt is malformed"
echo "PASS: the checked-in I/O budgets are well formed"
echo "PASS: all checks passed"
