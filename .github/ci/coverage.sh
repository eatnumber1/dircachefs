#!/bin/bash
# `bazel coverage` for CI (step 7.2): measures our code (.bazelrc, `coverage`
# section) over the tests the arguments select (--config=presubmit, ...), then
# checks that the combined lcov report really has coverage in it (a harness
# that silently stopped shipping profiles must not publish an empty report)
# and copies it to ci-coverage/, which the workflow uploads. No threshold yet
# (Phase 8).
set -euo pipefail
targets=(//...)
if [ "${DCFS_CI_KVM:-0}" != 1 ]; then
	targets+=(-//test/qemu:pjdfstest_test_xfs -//test/qemu:pjdfstest_test_btrfs)
fi
status=0
bazel coverage --keep_going "$@" -- "${targets[@]}" || status=$?
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
else
	echo "coverage.sh: no combined report at $report" >&2
	status=1
fi
if [ "$status" != 0 ]; then
	./.github/ci/collect-logs.sh coverage || true
fi
exit "$status"
