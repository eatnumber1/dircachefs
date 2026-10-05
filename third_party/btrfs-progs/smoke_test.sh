#!/bin/sh
# Host-side smoke test for the pinned, static mkfs.btrfs and btrfs (see
# BUILD.bazel, BUILD.btrfs_progs and README.md). Needs neither root nor
# kernel control: mkfs.btrfs on a regular file is a plain userspace write.
#
# Usage: smoke_test.sh <mkfs.btrfs> <btrfs> <version>
set -eu

MKFS=$1
BTRFS=$2
VERSION=$3

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- static linking, and no sanitizer runtime ---------------------------------
for bin in "$MKFS" "$BTRFS"; do
	kind=$(file -b "$(readlink -f "$bin")")
	case "$kind" in
	*"statically linked"*) ;;
	*) fail "$bin is not statically linked: $kind" ;;
	esac
	if grep -aq -e AddressSanitizer -e UndefinedBehaviorSanitizer -e __asan_init -e __ubsan_handle "$bin"; then
		fail "$bin contains a sanitizer runtime"
	fi
done
echo "PASS: both binaries are static and sanitizer-free"

# --- --version -------------------------------------------------------------
# (both print a second line of compiled-in features, "-LZO -ZSTD ...")
out=$("$MKFS" --version 2>&1 | head -1) || fail "mkfs.btrfs --version exited nonzero: $out"
[ "$out" = "mkfs.btrfs, part of btrfs-progs v$VERSION" ] ||
	fail "mkfs.btrfs --version printed '$out', want 'mkfs.btrfs, part of btrfs-progs v$VERSION'"
echo "PASS: $out"

out=$("$BTRFS" --version 2>&1 | head -1) || fail "btrfs --version exited nonzero: $out"
[ "$out" = "btrfs-progs v$VERSION" ] ||
	fail "btrfs --version printed '$out', want 'btrfs-progs v$VERSION'"
echo "PASS: $out"

# --- mkfs.btrfs on a file image ----------------------------------------------
truncate -s 256M "$WORK/test.img"
"$MKFS" -q -f "$WORK/test.img" || fail "mkfs.btrfs failed"
magic=$(dd if="$WORK/test.img" bs=1 skip=$((65536 + 64)) count=8 2>/dev/null)
[ "$magic" = "_BHRfS_M" ] || fail "no btrfs superblock magic in the image (got '$magic')"
echo "PASS: mkfs.btrfs wrote a btrfs superblock"

out=$("$BTRFS" inspect-internal dump-super "$WORK/test.img" 2>&1) ||
	fail "btrfs inspect-internal dump-super failed: $out"
case "$out" in
*"magic"*"_BHRfS_M"*) ;;
*) fail "dump-super did not show the magic: $out" ;;
esac
echo "PASS: btrfs inspect-internal dump-super"

echo "PASS: all checks passed"
