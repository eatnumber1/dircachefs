#!/bin/bash
# Step 26.14e: one `bazel test` of the noisy job, reported as findings.
#
#   noisy.sh NAME [--targets=LABEL,LABEL...] [test.sh or bazel arguments...]
#
# Runs the suite (through test.sh, so its --shard and --sizes work) or the
# named targets, with the noisy knob (--test_env=DCFS_NOISY=1: run-qemu.sh
# leaves the quiet-kernel sysctls at the kernel's defaults and gives every
# guest two vCPUs; README.md, "A noisy run") and without the tests that are
# quiet by definition (tag quiet-only). At most two guests run at a time (two
# vCPUs each on a four-core runner). Bazel's build event file is summarized by
# tools/noisy_report.py into the job summary, and the logs of every test that
# failed or was flaky are copied to noisy-report/logs/NAME for upload.
#
# A test that failed (Bazel's exit status 3) is a finding, not a failed job:
# the exit status is 0 then. Anything else that went wrong (a build failure,
# a bad command line, an interrupted run) is the job's own and is returned.
set -uo pipefail
cd "$(dirname "$0")/../.."

name=$1
shift
targets=""
args=()
for arg in "$@"; do
	case "$arg" in
	--targets=*) targets="${arg#--targets=}" ;;
	*) args+=("$arg") ;;
	esac
done

mkdir -p noisy-report
bep="$PWD/noisy-report/$name.bep.json"
common=(--test_env=DCFS_NOISY=1 --test_tag_filters=-quiet-only
	--local_test_jobs=2 "--build_event_json_file=$bep")
# One profile per invocation (test.sh writes DCFS_CI_PROFILE; this job calls it
# more than once).
if [ -n "${DCFS_CI_PROFILE:-}" ]; then
	DCFS_CI_PROFILE="${DCFS_CI_PROFILE%.json.gz}-$name.json.gz"
	export DCFS_CI_PROFILE
fi

status=0
if [ -n "$targets" ]; then
	profile_args=()
	if [ -n "${DCFS_CI_PROFILE:-}" ]; then
		mkdir -p "$(dirname "$DCFS_CI_PROFILE")"
		profile_args=("--profile=$DCFS_CI_PROFILE")
	fi
	# shellcheck disable=SC2086 # the labels are words
	bazel test --keep_going "${profile_args[@]}" "${common[@]}" "${args[@]}" -- ${targets//,/ } || status=$?
else
	./.github/ci/test.sh "${common[@]}" "${args[@]}" || status=$?
fi

# Nothing was tested (a shard with nothing in it): no event file, no report.
if [ -f "$bep" ]; then
	python3 tools/noisy_report.py --name "$name" --bep "$bep" \
		--testlogs "$(readlink -f bazel-testlogs)" \
		--logs-out "$PWD/noisy-report/logs/$name" >>"${GITHUB_STEP_SUMMARY:-/dev/stdout}" ||
		status=$?
fi

case "$status" in
0 | 3) exit 0 ;;
*) exit "$status" ;;
esac
