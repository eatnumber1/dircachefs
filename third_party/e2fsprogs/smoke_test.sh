#!/bin/sh
# Host-side smoke test for the pinned, minimal, static mke2fs/debugfs build
# (see BUILD.bazel, BUILD.e2fsprogs and README.md). Needs neither root nor
# kernel control: mke2fs -d builds an ext4 image directly from a plain
# userspace libext2fs operation (no mount, no loop device), the same
# property third_party/debian/scripts/mkrootfs.sh relies on.
#
# Usage: smoke_test.sh <mke2fs> <debugfs>
set -eu

MKE2FS=$1
DEBUGFS=$2

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- static linking ------------------------------------------------------
# readelf -d reads the ELF dynamic section directly; a statically linked
# binary has no .dynamic section at all, so readelf's own diagnostic ("...
# does not contain a dynamic section") is the expected, passing output --
# confirmed on this step's own manually built binaries first (see
# README.md's "Build" section for the transcript).
for bin in "$MKE2FS" "$DEBUGFS"; do
	out=$(readlink -f "$bin")
	kind=$(file -b "$out")
	case "$kind" in
	*"statically linked"*) ;;
	*) fail "$bin is not statically linked: $kind" ;;
	esac
done
echo "PASS: both binaries are statically linked"

# --- --version -------------------------------------------------------------
out=$("$MKE2FS" -V 2>&1) || fail "mke2fs -V exited nonzero: $out"
case "$out" in
*"mke2fs "*) ;;
*) fail "mke2fs -V didn't print a version string: $out" ;;
esac
echo "PASS: mke2fs -V: $(echo "$out" | head -1)"

out=$("$DEBUGFS" -V 2>&1) || fail "debugfs -V exited nonzero: $out"
case "$out" in
*"debugfs "*) ;;
*) fail "debugfs -V didn't print a version string: $out" ;;
esac
echo "PASS: debugfs -V: $(echo "$out" | head -1)"

# --- mke2fs -d <tarball> end-to-end ---------------------------------------
# The actual feature this step exists for (third_party/debian/README.md's
# "Ownership" section): build a plain tar with a root-owned, setuid entry
# (recorded in the tar header -- this test doesn't run as root and never
# needs to, since mke2fs -d never chown()s anything, it just reads the
# tar's own per-entry metadata) and confirm it survives into the image.
ROOT="$WORK/root"
mkdir -p "$ROOT/usr/bin"
echo "test" >"$ROOT/usr/bin/mount"
chmod 4755 "$ROOT/usr/bin/mount"
tar --owner=0 --group=0 --numeric-owner -cf "$WORK/test.tar" -C "$ROOT" .

truncate -s 16M "$WORK/test.img"
"$MKE2FS" -q -t ext4 -d "$WORK/test.tar" -F "$WORK/test.img" ||
	fail "mke2fs -d <tarball> failed"

stat_out=$("$DEBUGFS" -R 'stat /usr/bin/mount' "$WORK/test.img" 2>&1) ||
	fail "debugfs stat failed: $stat_out"
echo "$stat_out" | grep -q 'Mode:  04755' ||
	fail "setuid bit lost; debugfs stat said:
$stat_out"
echo "$stat_out" | grep -qE 'User:[[:space:]]*0[[:space:]]+Group:[[:space:]]*0' ||
	fail "ownership lost; debugfs stat said:
$stat_out"
echo "PASS: mke2fs -d <tarball> + debugfs stat: setuid root ownership preserved"

echo "PASS: all checks passed"
