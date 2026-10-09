#!/bin/bash
# Prepares the `osv` job (plan step 5.3): checks osv-scanner.toml's ignores
# (every one needs a reason and an expiry date that has not passed), checks
# the pinned upstream tags against their commits (network), and writes to osv/:
#   shipped.cdx.json   the SBOM of what the dcfs binaries link (gates)
#   shipped-debs.cdx.json  its Debian packages (glibc), scanned by package (gates)
#   testonly.cdx.json  the SBOM of everything else (informational)
#   shipped-git/       one detached git root per shipped component, the form
#                      in which osv-scanner takes commits (tools/sbom/README.md)
#   seeded-git/        the same for the known-vulnerable self-check fixture
# Uses the runner's python3 (3.11 or newer: tomllib); //tools/sbom:sbom_test
# runs the same code under Bazel's hermetic interpreter. The Alpine packages
# (third_party/alpine) are known only after Bazel has fetched them, so this
# fetches those repositories and reads their resolved.json.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tools/sbom/sbom.py check-ignores --config osv-scanner.toml
python3 tools/sbom/sbom.py verify-commits
rm -rf osv
mkdir -p osv
alpine=()
output_base=$(bazel info output_base)
# One fetch of every Alpine repository (step 26.16: a profile of it when
# DCFS_CI_PROFILE is set, as in test.sh; the job summary shows the table).
fetch=()
if [[ -n ${DCFS_CI_PROFILE:-} ]]; then
  mkdir -p "$(dirname "$DCFS_CI_PROFILE")"
  fetch=("--profile=$DCFS_CI_PROFILE" --generate_json_trace_profile
    --experimental_profile_include_target_label)
fi
repos=$(python3 tools/sbom/sbom.py alpine-repos)
bazel fetch "${fetch[@]}" $(printf -- '--repo=@%s ' $repos)
for repo in $repos; do
  # cquery prints the file's path relative to the output base, whatever the
  # repository's canonical name is.
  file=$(bazel cquery --output=files "@${repo}//:resolved.json" 2>/dev/null)
  alpine+=(--alpine "${repo}=${output_base}/${file}")
done
python3 tools/sbom/sbom.py generate --out-dir osv "${alpine[@]}"
python3 tools/sbom/sbom.py git-roots --sbom osv/shipped.cdx.json --out-dir osv/shipped-git
python3 tools/sbom/sbom.py git-roots --sbom tools/sbom/testdata/seeded_vulnerable.cdx.json \
  --out-dir osv/seeded-git
