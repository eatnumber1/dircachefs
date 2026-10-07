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
set -euo pipefail
targets=(//...)
if [ "${DCFS_CI_KVM:-0}" != 1 ]; then
	targets+=(-//test/qemu:pjdfstest_test_xfs -//test/qemu:pjdfstest_test_btrfs)
fi
status=0
bazel test --keep_going "$@" -- "${targets[@]}" || status=$?
if [ "$status" != 0 ]; then
	# Keep the logs of this invocation (the next one may use another
	# configuration, and bazel-testlogs only follows the latest).
	./.github/ci/collect-logs.sh "$(echo "${*:-all}" | tr -c 'A-Za-z0-9\n' _)" || true
fi
exit "$status"
