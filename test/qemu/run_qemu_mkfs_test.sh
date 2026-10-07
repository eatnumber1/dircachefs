#!/bin/sh
# Host-side test of run-qemu.sh's mkfs wiring (R3, L5): the scratch disks are
# formatted by the pinned, Bazel-built mkfs tools passed with --mke2fs /
# --mke2fs-conf / --mkfs-xfs / --mkfs-btrfs, never by a host mkfs.* found on
# PATH; the paths used are logged to serial.log; a path under /usr, /bin,
# /sbin is refused. A fake qemu stands in for the emulator (nothing here
# needs root or a kernel), and fake mkfs tools record how they were run.
#
# Usage: run_qemu_mkfs_test.sh <run-qemu.sh>
set -eu

RUN_QEMU=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

mkdir "$WORK/bin" "$WORK/host"
cat >"$WORK/bin/qemu" <<'E'
#!/bin/sh
[ "${1:-}" = --version ] && echo "fake qemu 0"
# Keep what the guest would be given as its initramfs.
while [ $# -gt 0 ]; do
	[ "$1" = -initrd ] && cat "$2" >"$(dirname "$0")/../initrd.seen"
	shift
done
exit 0
E
for t in mke2fs mkfs-xfs mkfs-btrfs; do
	cat >"$WORK/bin/$t" <<E
#!/bin/sh
echo "$t MKE2FS_CONFIG=\${MKE2FS_CONFIG:-} \$*" >>"$WORK/mkfs-calls"
exit 0
E
done
chmod +x "$WORK"/bin/*
# A decoy host mkfs.ext4 first on PATH: it must never run.
cat >"$WORK/host/mkfs.ext4" <<E
#!/bin/sh
echo host-mkfs-ran >>"$WORK/host-ran"
E
chmod +x "$WORK/host/mkfs.ext4"
: >"$WORK/kernel"
: >"$WORK/initrd"
: >"$WORK/qboot.rom"
: >"$WORK/mke2fs.conf"

run() { # <extra args...> -- disks; sets RC
	mkdir -p "$WORK/t" "$WORK/out"
	rm -f "$WORK/out/serial.log"
	RC=0
	PATH="$WORK/host:$PATH" TEST_TMPDIR="$WORK/t" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" "$@" \
		"$WORK/kernel" "$WORK/initrd" vdb:ext4:8M vdc:xfs:8M vdd:btrfs:8M \
		>"$WORK/stdout" 2>&1 || RC=$?
}

good="--mke2fs $WORK/bin/mke2fs --mke2fs-conf $WORK/mke2fs.conf --mkfs-xfs $WORK/bin/mkfs-xfs --mkfs-btrfs $WORK/bin/mkfs-btrfs"

# shellcheck disable=SC2086
run $good
LOG="$WORK/out/serial.log"
[ -f "$LOG" ] || fail "no serial.log: $(cat "$WORK/stdout")"
[ ! -e "$WORK/host-ran" ] || fail "a host mkfs.ext4 from PATH ran"
grep -q "^mke2fs MKE2FS_CONFIG=$WORK/mke2fs.conf .*-t ext4" "$WORK/mkfs-calls" ||
	fail "mke2fs not run as the pinned binary with the checked-in config: $(cat "$WORK/mkfs-calls")"
grep -q "^mkfs-xfs " "$WORK/mkfs-calls" || fail "pinned mkfs.xfs not run"
grep -q "^mkfs-btrfs " "$WORK/mkfs-calls" || fail "pinned mkfs.btrfs not run"
for want in "mke2fs: $WORK/bin/mke2fs" "mke2fs.conf: $WORK/mke2fs.conf" \
	"mkfs.xfs: $WORK/bin/mkfs-xfs" "mkfs.btrfs: $WORK/bin/mkfs-btrfs"; do
	grep -q "^run-qemu.sh: $want" "$LOG" || fail "serial.log does not record '$want'"
done
echo "PASS: scratch disks are made by the explicitly passed tools, which are logged"

# The kernel-module archive (step 24.2) is appended to the initramfs the
# guest is given: the kernel unpacks concatenated archives into one.
printf 'MODULES' >"$WORK/modules.cpio.gz"
printf 'BASE' >"$WORK/initrd"
rm -f "$WORK/initrd.seen"
# shellcheck disable=SC2086
run $good --modules "$WORK/modules.cpio.gz"
[ "$(cat "$WORK/initrd.seen")" = BASEMODULES ] ||
	fail "the guest's initramfs is not the base followed by the modules: '$(cat "$WORK/initrd.seen" 2>&1)'"
rm -f "$WORK/initrd.seen"
# shellcheck disable=SC2086
run $good
[ "$(cat "$WORK/initrd.seen")" = BASE ] ||
	fail "without --modules the initramfs is not the base alone"
echo "PASS: --modules appends the module archive to the initramfs"

# A host path is refused, before any image is made.
rm -f "$WORK/mkfs-calls"
run --mke2fs /usr/sbin/mke2fs --mke2fs-conf "$WORK/mke2fs.conf" \
	--mkfs-xfs "$WORK/bin/mkfs-xfs" --mkfs-btrfs "$WORK/bin/mkfs-btrfs"
[ "$RC" -ne 0 ] || fail "a /usr/sbin/mke2fs was accepted"
grep -q "looks like a host path" "$WORK/stdout" || fail "no host-path error: $(cat "$WORK/stdout")"
[ ! -e "$WORK/mkfs-calls" ] || fail "a mkfs ran despite the host-path error"
echo "PASS: a host mkfs path is refused"

# Omitting a tool is an error: no silent fallback to PATH.
run --qemu "$WORK/bin/qemu"
[ "$RC" -ne 0 ] || fail "run-qemu.sh ran without the mkfs tools"
grep -q "are required" "$WORK/stdout" || fail "no 'required' error: $(cat "$WORK/stdout")"
echo "PASS: the mkfs tools are required"
echo "PASS: all checks passed"
