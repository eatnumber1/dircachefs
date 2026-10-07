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
echo "PASS: all checks passed"
