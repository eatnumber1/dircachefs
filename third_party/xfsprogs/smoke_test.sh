#!/bin/sh
# Host-side smoke test for the pinned, static mkfs.xfs and xfs_io (see
# BUILD.bazel, BUILD.xfsprogs and README.md). Needs neither root nor kernel
# control: mkfs.xfs on a regular file is a plain userspace write.
#
# Usage: smoke_test.sh <mkfs.xfs> <xfs_io> <version>
set -eu

MKFS=$1
XFS_IO=$2
VERSION=$3

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- static linking, and no sanitizer runtime ---------------------------------
# --config=asan/ubsan's global --copt/--linkopt flags leak into
# rules_foreign_cc builds (see README.md); a binary that picked them up is
# either dynamically linked against libasan/libubsan or carries the runtime
# statically. Check both.
for bin in "$MKFS" "$XFS_IO"; do
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
out=$("$MKFS" -V 2>&1) || fail "mkfs.xfs -V exited nonzero: $out"
[ "$out" = "mkfs.xfs version $VERSION" ] ||
	fail "mkfs.xfs -V printed '$out', want 'mkfs.xfs version $VERSION'"
echo "PASS: $out"

out=$("$XFS_IO" -V 2>&1) || fail "xfs_io -V exited nonzero: $out"
[ "$out" = "xfs_io version $VERSION" ] ||
	fail "xfs_io -V printed '$out', want 'xfs_io version $VERSION'"
echo "PASS: $out"

# --- mkfs.xfs on a file image ------------------------------------------------
truncate -s 512M "$WORK/test.img"
"$MKFS" -q -f "$WORK/test.img" || fail "mkfs.xfs failed"
magic=$(dd if="$WORK/test.img" bs=4 count=1 2>/dev/null)
[ "$magic" = "XFSB" ] || fail "no XFS superblock magic in the image (got '$magic')"
echo "PASS: mkfs.xfs wrote an XFS superblock"

# xfs_io can open a regular file and report its stat without a mounted XFS.
echo hello >"$WORK/f"
out=$("$XFS_IO" -c 'stat' "$WORK/f" 2>&1) || fail "xfs_io stat failed: $out"
case "$out" in
*"stat.size = 6"*) ;;
*) fail "xfs_io stat did not report the size: $out" ;;
esac
echo "PASS: xfs_io stat"

echo "PASS: all checks passed"
