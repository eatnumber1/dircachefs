#!/bin/sh
# Step 3.1a: build the pinned, stock upstream dcfs QEMU test kernel as an
# ordinary Bazel action (see BUILD.bazel's :bzImage genrule). Unlike
# test/qemu/scripts/build-kernel.sh (the out-of-tree, not-Bazel-tracked
# build of the *patched* kernel that stays the default for now -- see
# test/qemu/README.md), this script:
#
#   - never writes outside its own scratch/output directories (no changes
#     to $SRC_ROOT, the read-only @linux_source checkout Bazel handed us);
#   - makes no network access;
#   - starts from `make tinyconfig` (everything off) instead of
#     x86_64_defconfig + kvm_guest.config, plus the checked-in fragment
#     (kernel.config) instead of a long scripts/config -e/-d list, and
#     fails the build if olddefconfig drops anything the fragment asked
#     for (a symbol that silently stopped existing, or whose dependencies
#     the fragment forgot);
#   - uses the host's flex and bison (conf's lexer.l/parser.y need them; the
#     BCR builds of both were tried and reverted, see README.md's "Hermetic
#     build tools" and "Remaining host tools") and GNU bc built by Bazel
#     from source (not in the BCR -- see third_party/bc/);
#   - still uses the host C compiler/binutils (gcc, ld, as, ar, ...) and
#     host libelf/zlib headers: making those hermetic is left to Phase 7
#     (the pinned LLVM toolchain) and a later pass -- see README.md's
#     "Remaining host tools".
set -eu

SRC_ROOT=$1
FRAGMENT=$2
BC=$3
OUT_BZIMAGE=$4
OUT_LOG=$5
JOBS=${6:-4}

# Run the actual build in a child shell (re-executing this script), with
# its output going to a log. This must NOT be written as
# `{ ...; } >"$LOG" 2>&1 || { ...; }`: `set -e` is ignored inside any
# command list followed by `||` (verified with a dash/bash repro, review
# L1), which made every failure inside it silent and let the fragment
# check "pass" after a failing olddefconfig. The child runs under a plain
# `sh -eu`, so any failing command stops it, and the parent prints the log
# to stderr on failure.
if [ -z "${DCFS_KERNEL_BUILD_INNER:-}" ]; then
	BUILD=$(mktemp -d)
	# The out-of-tree build directory holds about 1 GB of objects: never
	# leave it behind, whether the build succeeds or fails.
	trap 'rm -rf "$BUILD"' EXIT
	status=0
	DCFS_KERNEL_BUILD_INNER=1 DCFS_KERNEL_BUILD_DIR=$BUILD \
		sh -eu "$0" "$@" >"$BUILD/build.log" 2>&1 || status=$?
	if [ "$status" -ne 0 ]; then
		cat "$BUILD/build.log" >&2
		echo "build_kernel.sh: FAILED (exit $status); log above" >&2
		exit "$status"
	fi
	cp "$BUILD/arch/x86/boot/bzImage" "$OUT_BZIMAGE"
	cp "$BUILD/build.log" "$OUT_LOG"
	# The whole log is the :kernel_build target's kernel-build.log output;
	# only its closing summary goes to Bazel's output.
	tail -n 4 "$BUILD/build.log"
	exit 0
fi

BUILD=$DCFS_KERNEL_BUILD_DIR
TOOLBIN=$(mktemp -d)
trap 'rm -rf "$TOOLBIN"' EXIT

# Reproducible builds (review L10): no build time, user or host in the
# kernel's version banner.
KBUILD_BUILD_TIMESTAMP='Thu Jan  1 00:00:00 UTC 1970'
KBUILD_BUILD_USER=dcfs
KBUILD_BUILD_HOST=dcfs
KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP KBUILD_BUILD_USER KBUILD_BUILD_HOST KBUILD_BUILD_VERSION

# Hermetic bc ahead of whatever the host has on PATH (see the header
# comment above and README.md's "Hermetic build tools" / "Remaining host
# tools": flex and bison are NOT made hermetic here -- see that section for
# why the BCR builds of both were tried and reverted).
ln -s "$(readlink -f "$BC")" "$TOOLBIN/bc"
PATH="$TOOLBIN:$PATH"
export PATH

{
	echo "dcfs stock test kernel build"
	echo "source: $SRC_ROOT"
	echo "fragment: $FRAGMENT"
	echo "flex: $(flex --version) (host)"
	echo "bison: $(bison --version | head -1) (host)"
	echo "bc: $("$TOOLBIN/bc" --version | head -1) (Bazel-built, see third_party/bc/)"
	echo "jobs: $JOBS"
	echo

	# make O=$BUILD builds out-of-tree, so $SRC_ROOT itself is never
	# written to -- it stays exactly the read-only input Bazel gave us.
	make -C "$SRC_ROOT" O="$BUILD" tinyconfig

	# Merge the fragment on top of tinyconfig's baseline (-m: merge only,
	# don't invoke make yet -- we run olddefconfig ourselves next so we
	# can diff its output against what the fragment asked for).
	"$SRC_ROOT/scripts/kconfig/merge_config.sh" \
		-O "$BUILD" -m "$BUILD/.config" "$FRAGMENT"
	make -C "$SRC_ROOT" O="$BUILD" olddefconfig

	echo
	echo "Checking every fragment symbol survived olddefconfig..."
	missing=0
	while IFS= read -r line; do
		case "$line" in
		''|'#'*) continue ;;
		esac
		name=${line%%=*}
		if ! grep -qxF "$line" "$BUILD/.config"; then
			echo "MISSING: $FRAGMENT wants '$line'," \
				"final .config has: $(grep "^$name=" "$BUILD/.config" || echo "(unset)")"
			missing=$((missing + 1))
		fi
	done <"$FRAGMENT"
	if [ "$missing" -ne 0 ]; then
		echo "FAIL: $missing fragment symbol(s) did not survive olddefconfig" \
			"(dropped by a Kconfig dependency, or the symbol no longer exists" \
			"in this kernel version). Fix third_party/linux/kernel.config."
		exit 1
	fi
	echo "OK: every fragment symbol is set exactly as requested."
	echo

	before_yes=$(grep -c '=y' "$BUILD/.config" || true)
	nice -n 19 make -C "$SRC_ROOT" O="$BUILD" -j"$JOBS" bzImage
	after_yes=$(grep -c '=y' "$BUILD/.config" || true)
	size=$(stat -c '%s' "$BUILD/arch/x86/boot/bzImage")
	echo "enabled config symbols: $after_yes (before olddefconfig's fill-in: $before_yes)"
	echo "bzImage size: $size bytes"
} 
