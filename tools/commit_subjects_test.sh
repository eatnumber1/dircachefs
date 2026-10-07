#!/bin/bash
# Self-check of .github/ci/commit_subjects.sh (plan steps 26.1 and 26.13):
# canned subject lists, one accepted and one rejected per rule.
set -euo pipefail

script=$1

accepts() {
  if ! printf '%s\n' "$1" | "${script}" --stdin 2>/dev/null; then
    echo "FAIL: rejected a good subject: $1" >&2
    exit 1
  fi
}

rejects() {
  local out
  if out=$(printf '%s\n' "$1" | "${script}" --stdin 2>&1); then
    echo "FAIL: accepted a bad subject: $1" >&2
    exit 1
  fi
  if [[ "${out}" != *"$1"* ]]; then
    echo "FAIL: the failure does not name the subject: ${out}" >&2
    exit 1
  fi
}

accepts "26.13: tools: repository-shape tests"
accepts "5.3c: sbom: shipped entries"
accepts "24.1-24.3 (review N8, N9): stale claims fixed"
accepts "plan: 26.9 done"
accepts "style: C15 helpers"
accepts "warnings: -Weverything"
accepts "notes: measurements"
accepts "audits: Phase 6"
accepts "R2: M3 follow-up"
accepts "R12.4: a review fix"
accepts "3: phase three"

rejects "Fix the thing"
rejects "26.13 no colon"
rejects "wip: stuff"
rejects "26.8:"
rejects "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
# An empty range (no commits) passes.
printf '' | "${script}" --stdin

# A list with one bad subject among good ones fails and names only it.
if out=$(printf '%s\n' "26.8: fine" "oops" "plan: fine" |
  "${script}" --stdin 2>&1); then
  echo "FAIL: a mixed list passed" >&2
  exit 1
fi
[[ "${out}" == *"-> oops"* && "${out}" != *"-> 26.8"* ]] || {
  echo "FAIL: wrong subject named: ${out}" >&2
  exit 1
}
echo "PASS"
