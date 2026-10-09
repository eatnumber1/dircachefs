#!/bin/bash
# `bazel coverage` for CI (step 7.2): measures our code (.bazelrc, `coverage`
# section) over the tests the arguments select (--config=presubmit, ...), then
# checks that the combined lcov report really has coverage in it (a harness
# that silently stopped shipping profiles must not publish an empty report)
# and copies it to ci-coverage/, which the workflow uploads. Then the gate
# (step 8.1, tools/coverage_gate.sh): dcfs/*.cc lines and branches must match
# dcfs/coverage_baseline.txt (not below it, and not 0.1 or more above it).
set -euo pipefail
targets=(//...)
if [ "${DCFS_CI_KVM:-0}" != 1 ]; then
	targets+=(-//test/qemu:pjdfstest_test_xfs -//test/qemu:pjdfstest_test_btrfs)
fi
# Step 26.16: the JSON trace profile, as in test.sh.
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
bazel coverage --keep_going "${profile_args[@]}" "$@" -- "${targets[@]}" || status=$?
report="$(bazel info output_path)/_coverage/_coverage_report.dat"
mkdir -p ci-coverage
if [ -s "$report" ]; then
	# Our code only (dcfs/, bench/, tools/), and nothing that measured no
	# line (test files and scripts Bazel lists with LF:0).
	awk -v RS='end_of_record\n' -v ORS='' '
		/(^|\n)SF:(dcfs|bench|tools)\// && !/\nLF:0\n/ { print $0 "end_of_record\n" }
	' "$report" >ci-coverage/coverage.lcov
	test/qemu/scripts/check-lcov.sh ci-coverage/coverage.lcov dcfs/status.cc
	test/qemu/scripts/check-lcov.sh ci-coverage/coverage.lcov dcfs/metadata_cache.cc
	# main.cc runs only in the end-to-end tests: hits here prove the whole
	# guest -> coverage disk -> lcov path, not only the unit tests'.
	test/qemu/scripts/check-lcov.sh ci-coverage/coverage.lcov dcfs/main.cc
	# The gate (step 8.1): dcfs/*.cc lines and branches against the committed
	# baseline; it also fails when coverage rose by 0.1 or more, so that the
	# baseline is raised in the commit that raised the coverage. A failure
	# here fails the job, but the report above is already published.
	tools/coverage_gate.sh ci-coverage/coverage.lcov dcfs/coverage_baseline.txt || status=1
else
	echo "coverage.sh: no combined report at $report" >&2
	status=1
fi
if [ "$status" != 0 ]; then
	./.github/ci/collect-logs.sh coverage || true
fi
exit "$status"
