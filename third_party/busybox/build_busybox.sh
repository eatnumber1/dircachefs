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

# No build timestamp in .config or the version banner (reproducible builds,
# review L10): confdata.c leaves AUTOCONF_TIMESTAMP empty.
KCONFIG_NOTIMESTAMP=1
export KCONFIG_NOTIMESTAMP

make -C "$SRC" O="$BUILD" allnoconfig >/dev/null

while IFS= read -r line; do
	case "$line" in
	\#* | "") continue ;;
	esac
	sym=${line%%=*}
	sed -i "s/^# ${sym} is not set\$/${line}/" "$BUILD/.config"
done <"$FRAGMENT_ABS"

make -C "$SRC" O="$BUILD" silentoldconfig >/dev/null

# The sed above only rewrites "# CONFIG_X is not set" lines: a symbol that
# was misspelled, renamed or removed by a busybox version bump is silently
# ignored, and silentoldconfig silently resets a symbol whose dependencies
# are off. Fail the build unless every fragment line is in the final
# .config exactly as written (the same check as the kernel's,
# third_party/linux/build_kernel.sh; review L2).
echo "Checking every fragment symbol survived silentoldconfig..."
missing=0
while IFS= read -r line; do
	case "$line" in
	\#* | "") continue ;;
	esac
	sym=${line%%=*}
	if ! grep -qxF "$line" "$BUILD/.config"; then
		echo "MISSING: fragment wants '$line', final .config has: $(grep "^${sym}=" "$BUILD/.config" || echo "(unset)")" >&2
		missing=$((missing + 1))
	fi
done <"$FRAGMENT_ABS"
if [ "$missing" -ne 0 ]; then
	echo "FAIL: $missing fragment symbol(s) did not survive silentoldconfig" \
		"(misspelled/renamed/removed, or a dependency is off). Fix busybox.config.fragment." >&2
	exit 1
fi
echo "OK: every fragment symbol is set as requested."

make -C "$SRC" O="$BUILD" -j"$(nproc)" busybox

cp "$BUILD/busybox" "$OUT_ABS"
