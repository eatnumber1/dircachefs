#!/bin/bash
# Step 26.16: puts the table of tools/ci_profile.py for the job's Bazel
# profiles (ci-profile/*.json.gz, written by test.sh, coverage.sh and
# tools/reproducible_build.sh when DCFS_CI_PROFILE is set) in the job summary
# ($GITHUB_STEP_SUMMARY; standard output when unset, as under `act` without
# it). The tool runs through Bazel's hermetic interpreter (`bazel run`), so
# this is the last Bazel command of the job: it rewrites the bazel-* symlinks
# for the default configuration, which accelerator.sh must not see.
#
#   --python3   run the tool with the host's python3 (a job that has no
#               Bazel server of its own at the repository root; the tool
#               needs only the standard library)
# Never fails the job: a missing profile or a tool error is reported on the
# summary and the exit status stays 0.
set -uo pipefail
cd "$(dirname "$0")/../.."
shopt -s nullglob
profiles=(ci-profile/*.json.gz ci-profile/*.json)
out="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
{
  echo "### Where the time went (Bazel profile)"
  echo
  if [[ ${#profiles[@]} -eq 0 ]]; then
    echo "No Bazel profile was written (the build failed before Bazel started?)."
  elif [[ ${1:-} == --python3 ]]; then
    python3 tools/ci_profile.py "${profiles[@]}" ||
      echo "tools/ci_profile.py failed (exit $?)."
  else
    bazel run //tools:ci_profile -- "${profiles[@]/#/$PWD/}" ||
      echo "bazel run //tools:ci_profile failed (exit $?)."
  fi
} >>"$out"
exit 0
