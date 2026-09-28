#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Assemble a dcfs QEMU guest initramfs. Invoked as a Bazel genrule tool (see
# test/qemu/BUILD.bazel and test/qemu/qemu_cc_test.bzl), so all paths in and
# out are genrule locations, not repo-relative paths. Two modes:
#
#   mkinitramfs.sh <out.cpio.gz> <busybox> <dcfs-binary> <fhtest-binary> \
#       <testutil-binary> <init> [src...]
#       e2e initramfs (test/qemu/BUILD.bazel's :initramfs): busybox, dcfs,
#       fhtest, testutil, guest/init, and every guest/*.sh test script
#       (installed as /tests/<basename>); step 4.5 additionally recognizes
#       any [src] path shaped like the pjdfstest cc_binary or a file from
#       its ":tests" filegroup (see the case statement below) and installs
#       those under /pjdfstest/; every other [src] is ignored -- the
#       genrule passes $(SRCS), which includes the named files above
#       again.
#
#   mkinitramfs.sh --unit <out.cpio.gz> <busybox> <init> <test-binary> \
#       <disk0-device-or-'-'> <args> [name:path...]
#       Per-test initramfs for qemu_cc_test: busybox, guest/init, the test
#       binary at /test/run, /test/disk0 (the device name, e.g. "vdb", iff
#       a disks= entry was given), /test/args (<args> verbatim, guest/init
#       word-splits it), and each `name:path` data file copied to
#       /test/data/<name>.
#
# In both modes, any dynamically linked binary (dcfs/fhtest/testutil/the
# test binary; normal builds are fully static -- see qemu_cc_test.bzl -- but
# ASan/UBSan builds cannot be) has its ldd(1) closure and ELF interpreter
# copied into the initramfs at the same absolute paths, so the dynamic
# loader finds them with no rpath surgery. This is a no-op for a static
# binary: ldd exits nonzero and prints nothing matched by the pattern
# below.
set -eu

# Copies $1 (a binary already installed in $ROOT) plus every shared object
# ldd(1) reports for it -- including the ELF interpreter -- into $ROOT at
# their original absolute host paths.
copy_deps() {
	bin=$1
	ldd "$bin" 2>/dev/null | while read -r line; do
		path=$(echo "$line" | awk '
			$2 == "=>" && $3 ~ /^\// { print $3; next }
			$1 ~ /^\// { print $1 }
		')
		case "$path" in
		/*)
			dest="$ROOT$path"
			[ -f "$dest" ] && continue
			mkdir -p "$(dirname "$dest")"
			cp "$path" "$dest"
			;;
		esac
	done || true
}

OUT_ARG=$1
if [ "$OUT_ARG" = "--unit" ]; then
	shift
	MODE=unit
	OUT=$1
	BUSYBOX=$2
	INIT=$3
	TESTBIN=$4
	DISK0=$5
	ARGS=$6
	shift 6
else
	MODE=e2e
	OUT=$1
	BUSYBOX=$2
	DCFS=$3
	FHTEST=$4
	TESTUTIL=$5
	INIT=$6
	shift 6
fi

case "$OUT" in
/*) OUT_ABS="$OUT" ;;
*) OUT_ABS="$(pwd)/$OUT" ;;
esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/root"

if [ "$MODE" = "unit" ]; then
	mkdir -p "$ROOT/bin" "$ROOT/test/data" "$ROOT/proc" "$ROOT/sys" \
		"$ROOT/dev" "$ROOT/tmp"

	cp "$BUSYBOX" "$ROOT/bin/busybox"
	ln -sf busybox "$ROOT/bin/sh"
	cp "$INIT" "$ROOT/init"

	cp "$TESTBIN" "$ROOT/test/run"
	copy_deps "$ROOT/test/run"
	chmod +x "$ROOT/test/run"

	printf '%s' "$ARGS" >"$ROOT/test/args"

	: >"$ROOT/test/disk0"
	if [ "$DISK0" != "-" ]; then
		printf '%s' "$DISK0" >"$ROOT/test/disk0"
	fi

	for pair in "$@"; do
		name=${pair%%:*}
		src=${pair#*:}
		dest="$ROOT/test/data/$name"
		mkdir -p "$(dirname "$dest")"
		cp "$src" "$dest"
	done

	chmod +x "$ROOT/init" "$ROOT/bin/"*
else
	mkdir -p "$ROOT/bin" "$ROOT/tests" "$ROOT/proc" "$ROOT/sys" "$ROOT/dev" \
		"$ROOT/tmp" "$ROOT/src" "$ROOT/cache" "$ROOT/mnt" \
		"$ROOT/pjdfstest/tests"

	cp "$BUSYBOX" "$ROOT/bin/busybox"
	cp "$DCFS" "$ROOT/bin/dcfs"
	cp "$FHTEST" "$ROOT/bin/fhtest"
	cp "$TESTUTIL" "$ROOT/bin/testutil"
	ln -sf busybox "$ROOT/bin/sh"
	cp "$INIT" "$ROOT/init"
	copy_deps "$ROOT/bin/dcfs"
	copy_deps "$ROOT/bin/fhtest"
	copy_deps "$ROOT/bin/testutil"

	# step 4.5: pjdfstest, matched by path shape rather than a dedicated
	# argument so this stays additive -- $(SRCS) in the :initramfs genrule
	# just needs "@pjdfstest//:pjdfstest" and "@pjdfstest//:tests" added to
	# srcs (see test/qemu/BUILD.bazel) and every existing caller/argument
	# is untouched. The pjdfstest binary's source path ends in
	# ".../pjdfstest" (its own basename); every file from the ":tests"
	# filegroup has "/tests/" somewhere in its path (pjdfstest's own
	# tests/ directory) -- neither pattern collides with any other src
	# this genrule passes (busybox/dcfs/fhtest/testutil/init/guest/*.sh
	# all live under this repo's singular "test/qemu" directory).
	# */tests/* (pjdfstest's own tests/ tree, which includes misc.sh) is
	# checked before the generic *.sh guest-test-script pattern below, since
	# tests/misc.sh would otherwise match *.sh first and land in the wrong
	# place (/tests/misc.sh instead of /pjdfstest/tests/misc.sh).
	# pjdfstest.expected_failures / pjdfstest.ext4_failures (the checked-in
	# failure baselines guest/pjdfstest.sh reads) are also matched by path
	# shape here, ahead of the generic *.sh pattern for the same reason.
	for f in "$@"; do
		case "$f" in
		*/tests/*)
			rel=${f#*/tests/}
			dest="$ROOT/pjdfstest/tests/$rel"
			mkdir -p "$(dirname "$dest")"
			cp "$f" "$dest"
			;;
		*/pjdfstest) cp "$f" "$ROOT/pjdfstest/pjdfstest" ;;
		*/pjdfstest.expected_failures)
			cp "$f" "$ROOT/pjdfstest/pjdfstest.expected_failures"
			;;
		*/pjdfstest.ext4_failures)
			cp "$f" "$ROOT/pjdfstest/pjdfstest.ext4_failures"
			;;
		*.sh) cp "$f" "$ROOT/tests/$(basename "$f")" ;;
		esac
	done

	chmod +x "$ROOT/init" "$ROOT/bin/"*
	if [ -n "$(find "$ROOT/tests" -mindepth 1 -print -quit)" ]; then
		chmod +x "$ROOT/tests/"*
	fi
	[ -f "$ROOT/pjdfstest/pjdfstest" ] && chmod +x "$ROOT/pjdfstest/pjdfstest"
fi

(cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip -1) >"$OUT_ABS"

echo "Initramfs: $OUT_ABS"
