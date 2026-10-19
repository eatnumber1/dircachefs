#!/bin/sh
# SPDX-License-Identifier: MIT
# Host-side test of run-qemu.sh's mkfs wiring (R3, L5): the scratch disks are
# formatted by the pinned, Bazel-built mkfs tools passed with --mke2fs /
# --mke2fs-conf / --mkfs-xfs / --mkfs-btrfs, never by a host mkfs.* found on
# PATH; the paths used are logged to serial.log; a path under /usr, /bin,
# /sbin is refused. A fake qemu stands in for the emulator (nothing here
# needs root or a kernel), and fake mkfs tools record how they were run. The
# same fake qemu records the disk layout it was given (SCSI disks, step 5.18).
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
# Keep what the guest would be given as its initramfs, and every argument.
: >"$(dirname "$0")/../qemu-args"
while [ $# -gt 0 ]; do
	echo "$1" >>"$(dirname "$0")/../qemu-args"
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

run() { # <extra args...>; sets RC; $VDB_SPEC overrides the vdb disk-spec
	mkdir -p "$WORK/t" "$WORK/out"
	rm -f "$WORK/out/serial.log"
	RC=0
	PATH="$WORK/host:$PATH" TEST_TMPDIR="$WORK/t" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --unit --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" "$@" \
		"$WORK/kernel" "$WORK/initrd" "${VDB_SPEC:-vdb:ext4:8M}" vdc:xfs:8M vdd:btrfs:8M \
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

# Step 23.7: an ext4 disk-spec may carry mke2fs options as a fourth field
# (a casefold-capable filesystem, made at mkfs time: the kernel cannot load
# the encoding when the feature is switched on under a mounted filesystem).
rm -f "$WORK/mkfs-calls"
# shellcheck disable=SC2086
VDB_SPEC="vdb:ext4:8M:-O casefold -E encoding=utf8" run $good
grep -q "^mke2fs MKE2FS_CONFIG=$WORK/mke2fs.conf .*-t ext4 -O casefold -E encoding=utf8 " "$WORK/mkfs-calls" ||
	fail "the fourth field is not passed to mke2fs: $(cat "$WORK/mkfs-calls")"
echo "PASS: an ext4 disk-spec's fourth field is passed to mke2fs"
VDB_SPEC="vdb:xfs:8M:-O casefold" run $good
[ "$RC" -ne 0 ] || fail "mkfs options for xfs were accepted"
grep -q "only supported for ext4" "$WORK/stdout" || fail "no ext4-only error: $(cat "$WORK/stdout")"
echo "PASS: mkfs options are refused for other filesystems"

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

# Step 5.18: a disk-spec named sd<letter> is a disk behind the one virtio-scsi
# controller (one virtio-mmio device however many disks: the microvm's
# fifth device would share the RTC's interrupt), a vd<letter> spec stays a
# virtio-blk disk, and the disks after the specs (the Debian root, the systemd
# image, the coverage disk) are numbered by the virtio-blk specs alone.
# layout <options...> -- <disk-spec...>: an e2e boot, RC its exit status (a
# fake qemu boots no guest, so a run that got as far as QEMU still fails).
layout() {
	rm -rf "$WORK/t" "$WORK/out" "$WORK/mkfs-calls" "$WORK/qemu-args"
	mkdir -p "$WORK/t" "$WORK/out"
	opts=""
	while [ "$1" != -- ]; do
		opts="$opts $1"
		shift
	done
	shift
	RC=0
	# shellcheck disable=SC2086 # opts is a list of words
	TEST_TMPDIR="$WORK/t" TEST_UNDECLARED_OUTPUTS_DIR="$WORK/out" \
		sh "$RUN_QEMU" --qemu "$WORK/bin/qemu" --qboot "$WORK/qboot.rom" $good $opts \
		"$WORK/kernel" "$WORK/initrd" test.sh "$@" >"$WORK/stdout" 2>&1 || RC=$?
}

# count <fixed string>: how many lines of the QEMU command line are it.
count() { grep -c -x -F -e "$1" "$WORK/qemu-args" || true; }

# Three SCSI disks: one controller, a LUN each in the letters' order, each
# formatted as its spec says, and no virtio-blk device at all.
layout -- sda:ext4:8M sdb:xfs:16M sdc:btrfs:32M
[ -e "$WORK/qemu-args" ] ||
	fail "three SCSI disks: qemu did not run: $(cat "$WORK/stdout")"
[ "$(count virtio-scsi-device,id=scsi0)" -eq 1 ] ||
	fail "want one virtio-scsi-device: $(cat "$WORK/qemu-args")"
[ "$(count scsi-hd,drive=sda,bus=scsi0.0,scsi-id=0,lun=0)" -eq 1 ] ||
	fail "sda is not LUN 0: $(cat "$WORK/qemu-args")"
[ "$(count scsi-hd,drive=sdb,bus=scsi0.0,scsi-id=0,lun=1)" -eq 1 ] ||
	fail "sdb is not LUN 1: $(cat "$WORK/qemu-args")"
[ "$(count scsi-hd,drive=sdc,bus=scsi0.0,scsi-id=0,lun=2)" -eq 1 ] ||
	fail "sdc is not LUN 2: $(cat "$WORK/qemu-args")"
grep -q 'virtio-blk-device' "$WORK/qemu-args" &&
	fail "a SCSI-only run attached a virtio-blk device: $(cat "$WORK/qemu-args")"
[ "$(stat -c %s "$WORK/t/sda.img")" -eq 8388608 ] || fail "sda.img is not 8M"
[ "$(stat -c %s "$WORK/t/sdb.img")" -eq 16777216 ] || fail "sdb.img is not 16M"
[ "$(stat -c %s "$WORK/t/sdc.img")" -eq 33554432 ] || fail "sdc.img is not 32M"
# Each image got its own filesystem: the fake tools log the image they were
# given last.
grep -q "^mke2fs .* $WORK/t/sda.img\$" "$WORK/mkfs-calls" ||
	fail "sda.img was not formatted ext4: $(cat "$WORK/mkfs-calls")"
grep -q "^mkfs-xfs .* $WORK/t/sdb.img\$" "$WORK/mkfs-calls" ||
	fail "sdb.img was not formatted xfs: $(cat "$WORK/mkfs-calls")"
grep -q "^mkfs-btrfs .* $WORK/t/sdc.img\$" "$WORK/mkfs-calls" ||
	fail "sdc.img was not formatted btrfs: $(cat "$WORK/mkfs-calls")"
echo "PASS: SCSI disks share one virtio-scsi controller, a LUN each, formatted"

# The letter is the LUN, gaps included: the guest finds a disk by its LUN.
layout -- sdb:ext4:8M sdd:ext4:8M
[ "$(count scsi-hd,drive=sdb,bus=scsi0.0,scsi-id=0,lun=1)" -eq 1 ] &&
	[ "$(count scsi-hd,drive=sdd,bus=scsi0.0,scsi-id=0,lun=3)" -eq 1 ] ||
	fail "the LUNs are not the letters: $(cat "$WORK/qemu-args")"
echo "PASS: a SCSI disk's LUN is its letter"

# --keep-disks (a second boot): the images of the first are used as they are,
# SCSI ones included -- not made again, not formatted again.
rm -rf "$WORK/keep"
layout --keep-disks "$WORK/keep" -- sda:ext4:8M sdb:xfs:16M
printf 'FIRSTBOOT' | dd of="$WORK/keep/sdb.img" conv=notrunc 2>/dev/null
layout --keep-disks "$WORK/keep" -- sda:ext4:8M sdb:xfs:16M
[ -e "$WORK/qemu-args" ] ||
	fail "kept SCSI disks: qemu did not run: $(cat "$WORK/stdout")"
[ ! -e "$WORK/mkfs-calls" ] ||
	fail "a kept SCSI image was formatted again: $(cat "$WORK/mkfs-calls")"
grep -q "^run-qemu.sh: keeping $WORK/keep/sdb.img\$" "$WORK/stdout" ||
	fail "no 'keeping' line for sdb.img: $(cat "$WORK/stdout")"
[ "$(head -c 9 "$WORK/keep/sdb.img")" = FIRSTBOOT ] ||
	fail "a kept SCSI image was rewritten"
[ "$(count scsi-hd,drive=sdb,bus=scsi0.0,scsi-id=0,lun=1)" -eq 1 ] ||
	fail "the kept sdb is not LUN 1: $(cat "$WORK/qemu-args")"
echo "PASS: --keep-disks keeps the SCSI images of an earlier boot"

# With a systemd image and a coverage disk: the image is the first virtio-blk
# disk (vda) and the coverage disk the next, as nothing else is virtio-blk.
: >"$WORK/image"
: >"$WORK/qemu-img"
printf '#!/bin/sh\nfor a; do last=$a; done\n: >"$last"\n' >"$WORK/bin/qemu-img"
chmod +x "$WORK/bin/qemu-img"
mkdir "$WORK/cov"
COVERAGE_DIR="$WORK/cov"
export COVERAGE_DIR
layout --systemd-image "$WORK/image" --qemu-img "$WORK/bin/qemu-img" \
	--cov-script x --cov-llvm-profdata x --cov-llvm-cov x --cov-object x -- \
	sda:ext4:8M sdb:xfs:16M
unset COVERAGE_DIR
[ -e "$WORK/qemu-args" ] ||
	fail "systemd image with SCSI disks: qemu did not run: $(cat "$WORK/stdout")"
grep -q 'dcfs_systemd=/dev/vda1' "$WORK/qemu-args" ||
	fail "the systemd image is not vda: $(cat "$WORK/qemu-args")"
grep -q 'dcfs_cov=/dev/vdb' "$WORK/qemu-args" ||
	fail "the coverage disk is not vdb: $(cat "$WORK/qemu-args")"
[ "$(count virtio-scsi-device,id=scsi0)" -eq 1 ] || fail "want one controller"
echo "PASS: the systemd image and the coverage disk after SCSI disks" \
	"are vda and vdb"

# SCSI and virtio-blk disks together: a vdb with a vda filler beside the
# controller.
layout -- vdb:ext4:8M sda:ext4:8M
[ -e "$WORK/qemu-args" ] ||
	fail "mixed disks: qemu did not run: $(cat "$WORK/stdout")"
[ "$(count scsi-hd,drive=sda,bus=scsi0.0,scsi-id=0,lun=0)" -eq 1 ] ||
	fail "mixed: sda is not LUN 0"
[ "$(grep -c 'virtio-blk-device' "$WORK/qemu-args")" -eq 2 ] ||
	fail "mixed: want a filler and vdb on virtio-blk: $(cat "$WORK/qemu-args")"
echo "PASS: SCSI and virtio-blk disks can be given together"

# No SCSI disk, no controller: the other tests' device count is unchanged.
layout -- vdb:ext4:8M
grep -q scsi "$WORK/qemu-args" &&
	fail "a run with no SCSI disk has a SCSI device"
echo "PASS: a run with no SCSI disk has no controller"

refused() { # <expected words> <options...> -- <disk-spec...>
	want=$1
	shift
	layout "$@"
	[ "$RC" -ne 0 ] || fail "'$*' was accepted"
	grep -q "$want" "$WORK/stdout" ||
		fail "'$*': no '$want' in: $(cat "$WORK/stdout")"
	[ ! -e "$WORK/qemu-args" ] || fail "'$*': qemu ran"
}
refused "not a disk name" -- hda:ext4:8M
printf 'FSTOOLS' >"$WORK/fstools.cpio.gz"
refused "SCSI disks cannot be RAM disks" \
	--ram-disks "$WORK/fstools.cpio.gz" -- sda:ext4:8M
echo "PASS: unknown names and RAM-backed SCSI disks are refused"

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
