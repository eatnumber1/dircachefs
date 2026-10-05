#!/bin/sh
# Builds a statically linked busybox from the fetched @busybox source
# tree and the checked-in Kconfig fragment (busybox.config.fragment).
# Invoked as a Bazel genrule tool (third_party/busybox/BUILD.bazel); all
# paths in and out are genrule locations.
#
# Usage: build_busybox.sh <busybox-source-Makefile> <fragment> <out-binary>
#
# busybox's own build is a Kconfig/make tree, not configure+make or
# meson, so this is a plain genrule + checked-in config fragment rather
# than a rules_foreign_cc rule (see README.md's "Build system" section).
# The fragment is applied the way busybox's own
# scripts/kconfig/Makefile recommends (its header comment: "make
# allnoconfig; sed -i ... .config; make") rather than via
# KCONFIG_ALLCONFIG, which does not work for this purpose in this
# busybox version -- see busybox.config.fragment's own comment for why.
set -eu

MAKEFILE=$1
FRAGMENT=$2
OUT=$3

case "$OUT" in
/*) OUT_ABS=$OUT ;;
*) OUT_ABS="$(pwd)/$OUT" ;;
esac
case "$FRAGMENT" in
/*) FRAGMENT_ABS=$FRAGMENT ;;
*) FRAGMENT_ABS="$(pwd)/$FRAGMENT" ;;
esac

SRC=$(cd "$(dirname "$MAKEFILE")" && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

make() {
	# Bazel's genrule sandbox PATH already has /usr/bin:/bin (same as
	# every other genrule in this repo that shells out to host tools,
	# e.g. test/qemu/scripts/mkinitramfs.sh's use of cpio/gzip); no
	# toolchain plumbing beyond that is needed here, consistent with
	# the rest of the tree pre-Phase-7 (the pinned-clang phase).
	command make "$@"
}

make -C "$SRC" O="$BUILD" allnoconfig >/dev/null

while IFS= read -r line; do
	case "$line" in
	\#* | "") continue ;;
	esac
	sym=${line%%=*}
	sed -i "s/^# ${sym} is not set\$/${line}/" "$BUILD/.config"
done <"$FRAGMENT_ABS"

make -C "$SRC" O="$BUILD" silentoldconfig >/dev/null

make -C "$SRC" O="$BUILD" -j"$(nproc)" busybox

cp "$BUILD/busybox" "$OUT_ABS"
