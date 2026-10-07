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
	cp "$report" ci-coverage/coverage.lcov
	test/qemu/scripts/check-lcov.sh ci-coverage/coverage.lcov dcfs/status.cc
	test/qemu/scripts/check-lcov.sh ci-coverage/coverage.lcov dcfs/metadata_cache.cc
else
	echo "coverage.sh: no combined report at $report" >&2
	status=1
fi
if [ "$status" != 0 ]; then
	./.github/ci/collect-logs.sh coverage || true
fi
exit "$status"
