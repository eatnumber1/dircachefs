#!/bin/bash
# Step 26.12 gate: two builds of the shipped outputs, in two copies of the
# repository at different paths and two Bazel output bases, with no disk cache,
# must be byte-identical. Run it with `bazel run //tools:reproducible_build`
# (CI does); it cannot be a `bazel test`, it starts Bazel itself.
#
#   bazel run //tools:reproducible_build [-- <bazel build flags>]
#
# The outputs are //dcfs:main_static and //man:dcfs.8. A difference prints the
# strings that differ (tools/repro_compare.py), which is where an embedded
# path, host name or timestamp shows up. The gate's own self-check is
# //tools:repro_compare_self_check_test.
set -euo pipefail

workspace=${BUILD_WORKSPACE_DIRECTORY:?run with: bazel run //tools:reproducible_build}
compare="${RUNFILES_DIR:-$0.runfiles}/_main/tools/repro_compare"
# REPRO_TMP=<dir>: keep the copies and outputs there and reuse the outputs of
# an earlier run (for debugging a difference without rebuilding).
if [ -n "${REPRO_TMP:-}" ]; then
	tmp=$REPRO_TMP
	mkdir -p "$tmp"
else
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/dcfs-repro.XXXXXX")
fi
cleanup() {
	for x in a b; do
		if [ -d "$tmp/ob_$x" ]; then
			(cd "$tmp/src_$x" && bazel --output_base="$tmp/ob_$x" shutdown >/dev/null 2>&1) || true
		fi
	done
	[ -n "${REPRO_TMP:-}" ] || rm -rf "$tmp"
}
trap cleanup EXIT

targets=(//dcfs:main_static //man:dcfs.8)
outputs=(dcfs/main_static man/dcfs.8)

for x in a b; do
	[ -d "$tmp/out_$x" ] && continue
	# A copy of the tracked and untracked (not ignored) files, plus the
	# per-checkout bazelrc (it has CI's cache settings): a different path
	# for each build.
	mkdir -p "$tmp/src_$x"
	(cd "$workspace" && git ls-files -co --exclude-standard -z | tar --null -T - -cf -) |
		tar -x -C "$tmp/src_$x"
	[ -f "$workspace/user.bazelrc" ] && cp "$workspace/user.bazelrc" "$tmp/src_$x/"
	(
		cd "$tmp/src_$x"
		bazel --output_base="$tmp/ob_$x" build --disk_cache= "$@" "${targets[@]}"
		mkdir -p "$tmp/out_$x"
		for o in "${outputs[@]}"; do
			mkdir -p "$tmp/out_$x/$(dirname "$o")"
			cp "bazel-bin/$o" "$tmp/out_$x/$o"
		done
	)
done

args=()
for o in "${outputs[@]}"; do
	args+=("$tmp/out_a/$o" "$tmp/out_b/$o")
done
"$compare" "${args[@]}"
echo "reproducible: ${outputs[*]} are byte-identical across two builds"
