#!/bin/sh
# Host-side test of run-qemu.sh's verdict (step 23.7): a guest whose kernel
# logged an oops, a BUG, a WARNING or a panic fails the run, whatever its own
# checks said, and the FAIL names the first such line. A fake qemu prints a
# canned serial log (nothing here needs root or a kernel).
#
# Usage: run_qemu_verdict_test.sh <run-qemu.sh>
set -eu

RUN_QEMU=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

mkdir "$WORK/bin" "$WORK/out"
cat >"$WORK/bin/qemu" <<'E'
#!/bin/sh
[ "${1:-}" = --version ] && echo "fake qemu 0"
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

# Run run-qemu.sh --unit against the canned log in $WORK/canned; sets RC.
run() {
	rm -f "$WORK/out/serial.log"
	RC=0
	TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
		--mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
		--mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
		"$WORK/kernel" "$WORK/initrd" >"$WORK/stdout" 2>&1 || RC=$?
}

# guest/init copies only the kernel log's real failures to the serial log as
# KERNEL-OOPS: lines (which fail the run whatever they say). Hardware-
# vulnerability advisories printed at boot contain "WARNING:" (AMD runners
# print them; an Intel box does not) and are not failures. The pattern is
# taken from guest/init itself.
INIT=$2
PATTERN=$(sed -n "s/.*dmesg 2>\/dev\/null | grep -E '\(.*Call Trace:.*\)' |\$/\1/p" "$INIT")
[ -n "$PATTERN" ] || fail "no kernel-failure pattern found in $INIT"
dmesg_oops() { printf '%s\n' "$1" | grep -E "$PATTERN" || true; }
while IFS= read -r line; do
	[ -z "$(dmesg_oops "$line")" ] || fail "guest/init copies an advisory as a failure: $line"
done <<'EOF3'
Speculative Return Stack Overflow: WARNING: See https://kernel.org/doc/html/latest/admin-guide/hw-vuln/srso.html for mitigation options.
Spectre V2 : WARNING: Unprivileged eBPF is enabled with eIBRS on, data leaks possible via Spectre v2 BHB attacks!
RETBleed: WARNING: Spectre v2 mitigation leaves CPU vulnerable to RETBleed attacks, data leaks possible!
MDS: WARNING: Microcode update is needed
EOF3
while IFS= read -r line; do
	[ -n "$(dmesg_oops "$line")" ] || fail "guest/init does not copy a real failure: $line"
done <<'EOF4'
[    3.2] WARNING: CPU: 0 PID: 123 at fs/fuse/dir.c:99 fuse_lookup+0x10/0x20
WARNING: at fs/fuse/dir.c:99 fuse_lookup+0x10/0x20
[    3.1] ------------[ cut here ]------------
BUG: kernel NULL pointer dereference, address: 0000000000000000
Oops: 0000 [#1] SMP NOPTI
kernel BUG at fs/ext4/inode.c:1234!
Call Trace:
Kernel panic - not syncing: VFS
general protection fault, probably for non-canonical address 0xdead: 0000 [#1] SMP
EOF4
echo "PASS: guest/init's dmesg filter skips hw-vuln advisories and keeps real failures"

# A guest that passed: guest/init's MEM line and DCFS-TEST-EXIT=0, around
# whatever else is in $WORK/extra.
canned() {
	{
		echo "TEST first PASS"
		cat "$WORK/extra"
		echo "MEM total=268435456 min_avail=200000000 peak_used=50000000 peak_shmem=1000000"
		echo "DCFS-TEST-EXIT=0"
	} >"$WORK/canned"
}

: >"$WORK/extra"
canned
run
[ "$RC" -eq 0 ] || fail "a clean run failed: $(cat "$WORK/stdout")"
grep -q "== RESULT: PASS" "$WORK/stdout" || fail "a clean run did not pass"
echo "PASS: a clean run passes"

# Step 23.7: a guest whose kernel reclaimed memory (the MEM line's
# reclaim_scans, guest/init) gets a WARNING: tests that depend on cached
# inodes or pages may be invalid. A guest that did not (a zero, or an older
# line without the field) does not.
: >"$WORK/extra"
for scans in 0 ""; do
	{
		echo "TEST first PASS"
		echo "MEM total=268435456 min_avail=200000000 peak_used=50000000 peak_shmem=1000000${scans:+ reclaim_scans=$scans slabs_scanned=0}"
		echo "DCFS-TEST-EXIT=0"
	} >"$WORK/canned"
	run
	[ "$RC" -eq 0 ] || fail "a run without reclaim failed: $(cat "$WORK/stdout")"
	if grep -q "the guest reclaimed memory" "$WORK/stdout"; then
		fail "a warning for a run without reclaim (reclaim_scans='$scans')"
	fi
done
echo "PASS: no reclaim, no warning"
{
	echo "TEST first PASS"
	echo "MEM total=268435456 min_avail=200000000 peak_used=50000000 peak_shmem=1000000 reclaim_scans=1234 slabs_scanned=99"
	echo "DCFS-TEST-EXIT=0"
} >"$WORK/canned"
run
[ "$RC" -eq 0 ] || fail "reclaim failed the run (it only warns): $(cat "$WORK/stdout")"
grep -q "WARNING: the guest reclaimed memory (1234 pages scanned)" "$WORK/stdout" ||
	fail "no reclaim warning: $(cat "$WORK/stdout")"
echo "PASS: reclaim_scans > 0 produces the WARNING"

# The guest's QEMU timeout (e2e mode) follows Bazel's TEST_TIMEOUT less 60 s
# for teardown and log collection (half of it when that is less than 60 s); 1800 s when it is unset (a manual run); an
# explicit TIMEOUT wins over both. The fake qemu ignores it: the line
# run-qemu.sh prints says what it would have used.
guest_timeout() {
	: >"$WORK/extra"
	canned_e2e ALL-TESTS-PASSED
	FLAG=""
	rm -f "$WORK/out/serial.log"
	RC=0
	env -u TEST_TIMEOUT -u TIMEOUT "$@" TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
		--mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
		--mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
		"$WORK/kernel" "$WORK/initrd" casefold_tune_oops.sh >"$WORK/stdout" 2>&1 || RC=$?
	sed -n 's/^run-qemu.sh: guest timeout \([0-9]*\) s.*/\1/p' "$WORK/stdout"
}

# Each line is what the kernel prints for one kind of failure (with the
# timestamp the console adds when it adds one, and without, as guest/init's
# dump of the kernel log does).
while IFS= read -r line; do
	printf '%s\n' "$line" "BUG: a later line" >"$WORK/extra"
	canned
	run
	[ "$RC" -ne 0 ] || fail "'$line' did not fail the run"
	grep -q "== RESULT: FAIL" "$WORK/stdout" || fail "no FAIL verdict for '$line'"
	# The message quotes the first such line, not the later one.
	[ "$(sed -n '/the first such line/{n;p;}' "$WORK/stdout")" = "$line" ] ||
		fail "the message does not quote '$line' as the first line: $(cat "$WORK/stdout")"
	echo "PASS: a kernel log line fails the run: $line"
done <<'EOF2'
BUG: kernel NULL pointer dereference, address: 0000000000000000
[   12.345678] BUG: unable to handle page fault for address: ffffffffdeadbeef
Oops: 0000 [#1] SMP NOPTI
[    3.1] kernel BUG at fs/ext4/inode.c:1234!
[    3.2] WARNING: CPU: 1 PID: 77 at fs/fuse/dir.c:99 fuse_lookup+0x10/0x20
[    3.3] Call Trace:
[    3.4] Kernel panic - not syncing: VFS: Unable to mount root fs
KERNEL-OOPS: [    1.4] WARNING: possible circular locking dependency detected
KERNEL-OOPS: [    1.5] general protection fault, probably for non-canonical address 0xdead: 0000 [#1] SMP
EOF2

# The same words in the test's own output are not kernel failures.
printf '%s\n' "TEST bug-reports PASS" "checking that a warning is logged" \
	"WARNING: All log messages before absl::InitializeLog() is called are written to STDERR" >"$WORK/extra"
canned
run
[ "$RC" -eq 0 ] || fail "ordinary words failed the run: $(cat "$WORK/stdout")"
echo "PASS: words that are not kernel messages do not fail the run"
# Step 26.2: a DCFS-INVARIANT-VIOLATION line (the testonly checking build of
# dcfs found an invariant violated, and aborted) fails the run, quoted;
# the same words elsewhere on a line do not.
line="DCFS-INVARIANT-VIOLATION tri-state: inode 5: attributes recorded as current with nlink 0 (in request GETATTR nodeid 1)"
printf '%s\n' "$line" "DCFS-INVARIANT-VIOLATION dirty-set: a later one" >"$WORK/extra"
canned
run
[ "$RC" -ne 0 ] || fail "an invariant violation did not fail the run"
grep -q "== RESULT: FAIL (dcfs invariant violated" "$WORK/stdout" ||
	fail "no FAIL verdict for an invariant violation: $(cat "$WORK/stdout")"
[ "$(sed -n '/the first such line/{n;p;}' "$WORK/stdout")" = "$line" ] ||
	fail "the message does not quote the first violation: $(cat "$WORK/stdout")"
echo "PASS: an invariant violation fails the run"
printf '%s\n' "TEST no-violation PASS: no DCFS-INVARIANT-VIOLATION line" \
	"F1007 invariant_checker.cc:225] invariant violated: tri-state: ..." >"$WORK/extra"
canned
run
[ "$RC" -eq 0 ] || fail "the words mid-line failed the run: $(cat "$WORK/stdout")"
echo "PASS: the words mid-line do not fail the run"

# --expect-kernel-failure <script> (step 23.7, casefold_tune_oops_test): the
# one deliberate exception. An oops is tolerated only if the guest script
# reported it itself as "would FAIL (kernel: ..." on a DISABLED_ check, and
# the run's verdict then follows the other checks. Nothing else is weakened.
oops="KERNEL-OOPS: [    1.5] BUG: kernel NULL pointer dereference, address: 0000000000000018
KERNEL-OOPS: [    1.6] Oops: Oops: 0000 [#1] SMP PTI
KERNEL-OOPS: [    1.7] Call Trace:"
reported="TEST DISABLED_casefold-tune-online-oops DISABLED (kernel bug)
  would FAIL (kernel: BUG: kernel NULL pointer dereference, address: 0000000000000018)"

# Run run-qemu.sh in e2e mode on the canned log; $FLAG is extra flags.
# shellcheck disable=SC2086 # FLAG is a list of words
run_e2e() {
	rm -f "$WORK/out/serial.log"
	RC=0
	TEST_TMPDIR="$WORK" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" $FLAG --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" \
		--mke2fs "$WORK/bin/mke2fs" --mke2fs-conf "$WORK/mke2fs.conf" \
		--mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs" \
		"$WORK/kernel" "$WORK/initrd" casefold_tune_oops.sh >"$WORK/stdout" 2>&1 || RC=$?
}
# The guest's lines: $1 is the test's verdict line.
canned_e2e() {
	{
		cat "$WORK/extra"
		echo "MEM total=268435456 min_avail=200000000 peak_used=50000000 peak_shmem=1000000"
		echo "$1"
	} >"$WORK/canned"
}
FLAG="--expect-kernel-failure casefold_tune_oops.sh"

printf '%s\n' "$reported" "$oops" >"$WORK/extra"
canned_e2e ALL-TESTS-PASSED
run_e2e
[ "$RC" -eq 0 ] || fail "a reported oops failed the run under the flag: $(cat "$WORK/stdout")"
grep -q "== RESULT: PASS" "$WORK/stdout" || fail "no PASS for a reported oops under the flag"
grep -q "would FAIL (kernel: BUG: kernel NULL" "$WORK/out/serial.log" ||
	fail "the would-FAIL line is not in the serial log"
echo "PASS: --expect-kernel-failure: an oops the guest reported leaves the verdict to the other checks"

canned_e2e TEST-FAILED
run_e2e
[ "$RC" -ne 0 ] || fail "a failing check passed under the flag"
echo "PASS: --expect-kernel-failure: a failing check still fails the run"

printf '%s\n' "$oops" >"$WORK/extra"
canned_e2e ALL-TESTS-PASSED
run_e2e
[ "$RC" -ne 0 ] || fail "an oops the guest did not report passed under the flag"
grep -q "== RESULT: FAIL" "$WORK/stdout" || fail "no FAIL verdict for an unreported oops"
echo "PASS: --expect-kernel-failure: an oops the guest did not report fails the run"

printf '%s\n' "$reported" "$oops" "[    1.8] WARNING: CPU: 1 PID: 7 at fs/x.c:1 f+0x1/0x2" >"$WORK/extra"
canned_e2e ALL-TESTS-PASSED
run_e2e
[ "$RC" -ne 0 ] || fail "a WARNING passed under the flag"
echo "PASS: --expect-kernel-failure: a warning or panic still fails the run"

printf '%s\n' "$reported" >"$WORK/extra"
canned_e2e ALL-TESTS-PASSED
run_e2e
[ "$RC" -eq 0 ] || fail "a run without an oops failed under the flag: $(cat "$WORK/stdout")"
echo "PASS: --expect-kernel-failure: no oops (a fixed kernel) passes"

printf '%s\n' "$reported" "$oops" >"$WORK/extra"
canned_e2e ALL-TESTS-PASSED
FLAG=""
run_e2e
[ "$RC" -ne 0 ] || fail "an oops passed without the flag"
echo "PASS: without the flag the same log fails"

FLAG="--expect-kernel-failure other.sh"
run_e2e
[ "$RC" -ne 0 ] || fail "the flag naming another script was accepted"
grep -q "must name this test's guest script" "$WORK/stdout" || fail "no refusal message: $(cat "$WORK/stdout")"
echo "PASS: --expect-kernel-failure must name the test's own guest script"

FLAG="--unit --expect-kernel-failure casefold_tune_oops.sh"
run_e2e
[ "$RC" -ne 0 ] || fail "the flag was accepted in --unit mode"
echo "PASS: --expect-kernel-failure is refused for unit tests"
[ "$(guest_timeout TEST_TIMEOUT=900)" = 840 ] || fail "TEST_TIMEOUT=900 did not give 840 s: $(cat "$WORK/stdout")"
[ "$(guest_timeout TEST_TIMEOUT=3600)" = 3540 ] || fail "TEST_TIMEOUT=3600 did not give 3540 s"
[ "$(guest_timeout X=1)" = 1800 ] || fail "no TEST_TIMEOUT did not give 1800 s: $(cat "$WORK/stdout")"
[ "$(guest_timeout TEST_TIMEOUT=900 TIMEOUT=77)" = 77 ] || fail "TIMEOUT=77 did not win"
[ "$(guest_timeout TEST_TIMEOUT=60)" = 30 ] || fail "a TEST_TIMEOUT of 60 s did not give half of it"
echo "PASS: the guest timeout follows TEST_TIMEOUT less 60 s, else 1800, and TIMEOUT wins"
echo "PASS: all checks passed"
