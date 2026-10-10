#!/bin/sh
# Host-side test of run-qemu.sh's --ram-disks (step 26.17): the disk-specs name
# disks in the guest's RAM, so no image is made, no mkfs runs, no drive is
# attached and the specs go to the guest on the kernel command line; the mkfs
# archive is appended to the initramfs; and the combinations that cannot work
# (the disks must outlive the guest) are refused. A fake qemu records what it
# was given; nothing here needs root or a kernel.
#
# Usage: run_qemu_ram_disks_test.sh <run-qemu.sh>
set -eu

RUN_QEMU=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

mkdir "$WORK/bin"
cat >"$WORK/bin/qemu" <<'E'
#!/bin/sh
[ "${1:-}" = --version ] && echo "fake qemu 0"
dir=$(dirname "$0")/..
: >"$dir/qemu-args"
while [ $# -gt 0 ]; do
	echo "$1" >>"$dir/qemu-args"
	[ "$1" = -initrd ] && cat "$2" >"$dir/initrd.seen"
	shift
done
exit 0
E
for t in mke2fs mkfs-xfs mkfs-btrfs; do
	cat >"$WORK/bin/$t" <<E
#!/bin/sh
echo "$t \$*" >>"$WORK/mkfs-calls"
exit 0
E
done
chmod +x "$WORK"/bin/*
: >"$WORK/kernel"
: >"$WORK/qboot.rom"
: >"$WORK/mke2fs.conf"
printf 'BASE' >"$WORK/initrd"
printf 'FSTOOLS' >"$WORK/fstools.cpio.gz"

good="--mke2fs $WORK/bin/mke2fs --mke2fs-conf $WORK/mke2fs.conf --mkfs-xfs $WORK/bin/mkfs-xfs --mkfs-btrfs $WORK/bin/mkfs-btrfs"

# run <options...> -- <disk-spec...>: an e2e boot, RC its exit status.
run() {
	rm -rf "$WORK/t" "$WORK/out" "$WORK/mkfs-calls" "$WORK/qemu-args" "$WORK/initrd.seen"
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

# Without the option: a drive per disk (and a filler for vda), formatted.
run -- vdb:ext4:8M vdc:xfs:8M
grep -q '^-drive$' "$WORK/qemu-args" || fail "the plain boot attached no drive"
grep -q '^mkfs-xfs ' "$WORK/mkfs-calls" || fail "the plain boot did not run mkfs.xfs"
grep -q 'dcfs_ramdisks' "$WORK/qemu-args" && fail "the plain boot names RAM disks"
echo "PASS: without --ram-disks the disks are drives made on the host"

# With it: nothing on the host.
run --ram-disks "$WORK/fstools.cpio.gz" -- vdb:ext4:64M vdc:xfs:320M
[ ! -e "$WORK/mkfs-calls" ] || fail "a mkfs ran on the host: $(cat "$WORK/mkfs-calls")"
grep -q -e '-drive' -e 'virtio-blk' "$WORK/qemu-args" && fail "a drive was attached: $(cat "$WORK/qemu-args")"
grep -q '\.img' "$WORK/qemu-args" && fail "an image is named"
ls "$WORK/t"/*.img >/dev/null 2>&1 && fail "an image was made in the test's directory"
grep -q 'dcfs_ramdisks=vdb:ext4:64M,vdc:xfs:320M' "$WORK/qemu-args" ||
	fail "the kernel command line does not carry the specs: $(cat "$WORK/qemu-args")"
grep -q '^run-qemu.sh: RAM disks: vdb:ext4:64M,vdc:xfs:320M' "$WORK/out/serial.log" ||
	fail "serial.log does not record the RAM disks"
[ "$(cat "$WORK/initrd.seen")" = BASEFSTOOLS ] ||
	fail "the initramfs is not the base followed by the mkfs archive: '$(cat "$WORK/initrd.seen" 2>&1)'"
echo "PASS: --ram-disks makes no image, runs no mkfs, attaches no drive, and passes the specs"

# The modules' archive comes first, then the mkfs one.
printf 'MODULES' >"$WORK/modules.cpio.gz"
run --ram-disks "$WORK/fstools.cpio.gz" --modules "$WORK/modules.cpio.gz" -- vdb:ext4:8M
[ "$(cat "$WORK/initrd.seen")" = BASEMODULESFSTOOLS ] ||
	fail "initramfs is '$(cat "$WORK/initrd.seen" 2>&1)', want BASEMODULESFSTOOLS"
echo "PASS: the modules and the mkfs archives are both appended"

# What cannot be RAM-backed is refused, with the reason.
refused() { # <expected words> <options...> -- <disk-spec...>
	want=$1
	shift
	run "$@"
	[ "$RC" -ne 0 ] || fail "'$*' was accepted"
	grep -q "$want" "$WORK/stdout" || fail "'$*': no '$want' in: $(cat "$WORK/stdout")"
	[ ! -e "$WORK/qemu-args" ] || fail "'$*': qemu ran"
}
refused "excludes --boots and --power-cut" --ram-disks "$WORK/fstools.cpio.gz" --power-cut before -- vdb:ext4:8M
refused "excludes --boots and --power-cut" --ram-disks "$WORK/fstools.cpio.gz" --boots 2 -- vdb:ext4:8M
refused "neither --rootfs" --ram-disks "$WORK/fstools.cpio.gz" --rootfs "$WORK/kernel" -- vdb:ext4:8M
refused "neither --rootfs" --ram-disks "$WORK/fstools.cpio.gz" --systemd-image "$WORK/kernel" --qemu-img "$WORK/kernel" -- vdb:ext4:8M
refused "no mkfs options\|mkfs options are not supported" --ram-disks "$WORK/fstools.cpio.gz" -- "vdb:ext4:8M:-O casefold"
echo "PASS: --ram-disks is refused with power cuts, boots, a root image and mkfs options"
echo "PASS: all checks passed"
