#!/bin/bash
# `bazel test` for CI: the arguments are Bazel's (--config=fast, ...), every
# target is tested (--keep_going: a failure must not hide the others) and
# with the restored disk cache, an unchanged test is a cached pass (Bazel
# invalidates on any input change, so this is sound); every push still
# builds and tests what changed; a runner that lost KVM still runs the
# changed tests (under TCG, slower).
#
# Without KVM (DCFS_CI_KVM=0, set by prepare.sh) pjdfstest runs on ext4
# only: under TCG it takes about 3500 s per filesystem (Phase 5.1), so all
# three would not fit a job, and xfs and btrfs are still covered by the
# other tests' xfs and btrfs variants.
#
# Sharding (step 7.4b), for the suites too long for one runner:
#   --shard=I/N      test shard I of N (0 <= I < N) of the suite instead of
#                    all of it: the test targets of `bazel cquery` (so the
#                    ones incompatible with the configuration are not
#                    counted), dealt out by .github/ci/shard.sh, and passed
#                    to `bazel test` as explicit targets (so the result
#                    cache applies per shard);
#   --list           print the targets this invocation would test, one per
#                    line, and exit (no `bazel test`);
#   --sizes=a,b      only tests of these sizes (default: all); the `full`
#                    job's shards take large and enormous, small and medium
#                    having run in `presubmit`.
#   --tag=NAME       only the tests tagged NAME (default: every test except the
#                    ones tagged `xfstests`, which have a job of their own,
#                    step 17.1: eighteen shards of 11 to 21 minutes would
#                    double the full, asan and ubsan jobs); --tag also takes
#                    --shard and --sizes;
#   --print-queries  print the cquery expression of each size (no Bazel run),
#                    for //tools:test_sh_test.
# These are consumed here; every other argument goes to Bazel.
#
# Profile (step 26.16): with DCFS_CI_PROFILE set (the workflow does, one file
# per job and shard), `bazel test` writes its JSON trace profile there
# (gzipped by its .gz suffix) and the compact execution log beside it;
# tools/ci_profile.py sums the profile into the job summary and the workflow
# uploads both.
set -euo pipefail

shard=""
tag=""
print_queries=0
list=0
sizes="small,medium,large,enormous"
bazel_args=()
config_args=()
for arg in "$@"; do
	case "$arg" in
	--shard=*) shard="${arg#--shard=}" ;;
	--sizes=*) sizes="${arg#--sizes=}" ;;
	--list) list=1 ;;
	--tag=*) tag="${arg#--tag=}" ;;
	--print-queries) print_queries=1 ;;
	*)
		bazel_args+=("$arg")
		# cquery takes the build options (--config=asan, ...) but not
		# the test options (--test_timeout, ...).
		case "$arg" in --config=* | --define=*) config_args+=("$arg") ;; esac
		;;
	esac
done

# The xfstests shards need KVM: under TCG one takes hours (the generic tests
# are file system work for the guest's own CPU) and a backing file system's six
# would not fit a job. Say so in the log, and test nothing.
case "$tag" in
xfstests*)
	if [ "$list" != 1 ] && [ "$print_queries" != 1 ] && [ "${DCFS_CI_KVM:-0}" != 1 ]; then
		echo "test.sh: no KVM on this runner: the xfstests shards (--tag=$tag) are not run" >&2
		exit 0
	fi
	;;
esac

excluded=()
if [ "${DCFS_CI_KVM:-0}" != 1 ]; then
	excluded+=(//test/qemu:pjdfstest_test_xfs //test/qemu:pjdfstest_test_btrfs)
fi

# The cquery expression of the tests of size $1 this invocation may run. Tests
# tagged `manual` run only when asked for by name (step 11.2b:
# stress_random_test_*): `bazel test //...` skips them, and so must this. With
# --tag only the tests with that tag; without, none tagged `xfstests` (they
# are the `xfstests` job's, step 17.1).
query() {
	if [ -n "$tag" ]; then
		echo "attr(size, '^$1\$', attr(tags, '$tag', tests(//...))) except attr(tags, 'manual', tests(//...))"
	else
		echo "attr(size, '^$1\$', tests(//...)) except attr(tags, 'manual|xfstests', tests(//...))"
	fi
}

if [ "$print_queries" = 1 ]; then
	for size in ${sizes//,/ }; do query "$size"; done
	exit 0
fi

# The `SIZE LABEL` lines of the tests this shard may run, before sharding.
suite_tests() {
	local size
	for size in ${sizes//,/ }; do
		bazel cquery "${config_args[@]}" "$(query "$size")" \
			--output=starlark \
			--starlark:expr="'' if 'IncompatiblePlatformProvider' in str(providers(target)) else str(target.label)" |
			awk -v size="$size" 'NF { sub(/^@@/, "", $1); print size, $1 }'
	done
}

# Drops the lines whose label is one of the arguments.
without() {
	awk -v list="$*" 'BEGIN { n = split(list, a, " "); for (i = 1; i <= n; i++) drop[a[i]] = 1 } !($2 in drop)'
}

targets=(//...)
if [ -n "$tag" ] && [ -z "$shard" ]; then
	shard=0/1 # a tag selects test targets, which only the shard path lists
fi
if [ -n "$shard" ]; then
	mapfile -t targets < <(suite_tests | without "${excluded[@]}" | ./.github/ci/shard.sh "${shard%/*}" "${shard#*/}")
	echo "test.sh: shard $shard of sizes $sizes: ${#targets[@]} test targets" >&2
	if [ "${#targets[@]}" = 0 ]; then
		echo "test.sh: nothing to test in this shard" >&2
		exit 0
	fi
else
	for label in "${excluded[@]}"; do targets+=("-$label"); done
fi
if [ "$list" = 1 ]; then
	printf '%s\n' "${targets[@]}"
	exit 0
fi
profile_args=()
if [ -n "${DCFS_CI_PROFILE:-}" ]; then
	mkdir -p "$(dirname "$DCFS_CI_PROFILE")"
	# The label and primary output let tools/ci_profile.py tell a third-party
	# action from ours. The compact execution log (zstd; 0.9 MB for a fast-tier
	# run) shows what each spawn ran and whether it hit the cache.
	profile_args=("--profile=$DCFS_CI_PROFILE" --generate_json_trace_profile
		--experimental_profile_include_target_label
		--experimental_profile_include_primary_output
		"--execution_log_compact_file=${DCFS_CI_PROFILE%.json.gz}.execlog.zst")
fi
status=0
bazel test --keep_going "${profile_args[@]}" "${bazel_args[@]}" -- "${targets[@]}" || status=$?
if [ "$status" != 0 ]; then
	# Keep the logs of this invocation (the next one may use another
	# configuration, and bazel-testlogs only follows the latest).
	./.github/ci/collect-logs.sh "$(echo "${*:-all}" | tr -c 'A-Za-z0-9\n' _)" || true
fi
exit "$status"
