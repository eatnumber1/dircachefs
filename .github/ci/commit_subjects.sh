#!/bin/bash
# Plan step 26.13: every commit subject on a CI push range starts with the
# plan step it belongs to (`N.M:` or `N:`, optionally with a letter suffix, a
# `-N.M` / `/N.M` second step and a parenthesized note) or one of the
# prefixes `plan:`, `style:`, `process:`, `warnings:`, `notes:`, `audits:`, `agents:`,
# `R<n>:` and `R<n>.<m>:` (docs/style.md, "Docs and commits"). Co-Authored-By
# lines are trailers, not subjects.
#
# Usage: commit_subjects.sh RANGE   (e.g. "$BEFORE..$GITHUB_SHA"; an empty
#                                    or all-zero base checks the tip only)
#        commit_subjects.sh --stdin (one subject per line; the self-check
#                                    //tools:commit_subjects_test uses it)
#        commit_subjects.sh --stdin-log (one `HASH<TAB>SUBJECT` per line, as
#                                    `git log --format='%h%x09%s'` prints)
# Merge commits are skipped. Exit status 1 names every offending commit by
# hash and subject.
set -euo pipefail

readonly STEP='[0-9]+(\.[0-9]+[a-z]?)?([-/][0-9]+(\.[0-9]+[a-z]?)?)*( \([^)]*\))?'
readonly PREFIX='(plan|style|process|warnings|notes|audits|agents|R[0-9]+(\.[0-9]+)?)'
readonly PATTERN="^(${STEP}|${PREFIX}): ."

# One `HASH<TAB>SUBJECT` per line (HASH is `-` where the input has none).
subjects() {
  local range=$1
  case "${range}" in
    --stdin) sed 's/^/-\t/' ;;
    --stdin-log) cat ;;
    ..* | 0000000000000000000000000000000000000000..*)
      git log --no-merges --format='%h%x09%s' -n 1 "${range#*..}"
      ;;
    *) git log --no-merges --format='%h%x09%s' "${range}" ;;
  esac
}

main() {
  if [[ $# -ne 1 ]]; then
    echo "usage: $0 RANGE | --stdin" >&2
    return 2
  fi
  local bad=0
  local hash subject
  while IFS=$'\t' read -r hash subject; do
    if [[ ! "${subject}" =~ ${PATTERN} ]]; then
      echo "commit ${hash}: subject does not start with a plan step (N.M:) or one of" \
        "plan:, style:, process:, warnings:, notes:, audits:, agents:, R<n>.<m>: -> ${subject}" >&2
      bad=1
    fi
  done < <(subjects "$1")
  return "${bad}"
}

main "$@"
