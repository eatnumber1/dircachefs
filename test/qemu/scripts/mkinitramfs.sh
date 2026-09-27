#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Assemble the dcfs QEMU guest initramfs from busybox + dcfs + fhtest +
# the guest init + any guest test scripts. Invoked as a Bazel genrule tool
# (see test/qemu/BUILD.bazel), so all paths in and out are genrule
# locations, not repo-relative paths.
#
# Usage: mkinitramfs.sh <out.cpio.gz> <busybox> <dcfs-binary> <fhtest-binary> \
#            <init> [src...]
#
# Every [src] that doesn't end in .sh is ignored (the genrule passes
# $(SRCS), which includes the four named files above again); every one
# that does end in .sh is a guest test script and is installed as
# /tests/<basename> for guest/init to run by name.
set -eu

OUT=$1
BUSYBOX=$2
DCFS=$3
FHTEST=$4
INIT=$5
shift 5

case "$OUT" in
/*) OUT_ABS="$OUT" ;;
*) OUT_ABS="$(pwd)/$OUT" ;;
esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/root"

mkdir -p "$ROOT/bin" "$ROOT/tests" "$ROOT/proc" "$ROOT/sys" "$ROOT/dev" \
	"$ROOT/tmp" "$ROOT/src" "$ROOT/cache" "$ROOT/mnt"

cp "$BUSYBOX" "$ROOT/bin/busybox"
cp "$DCFS" "$ROOT/bin/dcfs"
cp "$FHTEST" "$ROOT/bin/fhtest"
ln -sf busybox "$ROOT/bin/sh"
cp "$INIT" "$ROOT/init"

for f in "$@"; do
	case "$f" in
	*.sh) cp "$f" "$ROOT/tests/$(basename "$f")" ;;
	esac
done

chmod +x "$ROOT/init" "$ROOT/bin/"*
if [ -n "$(find "$ROOT/tests" -mindepth 1 -print -quit)" ]; then
	chmod +x "$ROOT/tests/"*
fi

(cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip -1) >"$OUT_ABS"

echo "Initramfs: $OUT_ABS"
