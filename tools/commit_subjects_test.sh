#!/bin/bash
# SPDX-License-Identifier: MIT
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

# --stdin-log carries a hash per subject, and the failure names the hash
# of the offending commit and only that one.
if out=$(printf '%s\t%s\n' abc1234 "26.8: fine" def5678 "N4: review finding" 0a1b2c3 "plan: fine" |
  "${script}" --stdin-log 2>&1); then
  echo "FAIL: a bad subject in a log passed" >&2
  exit 1
fi
[[ "${out}" == *"commit def5678"* && "${out}" == *"-> N4: review finding"* &&
  "${out}" != *abc1234* && "${out}" != *0a1b2c3* ]] || {
  echo "FAIL: wrong hash or subject named: ${out}" >&2
  exit 1
}
printf '%s\t%s\n' abc1234 "26.8: fine" | "${script}" --stdin-log

# --stdin-messages carries whole messages: `HASH<TAB>SUBJECT`, a newline,
# the body, each message NUL-terminated (as `git log -z --format='%h%x09%B'`).
message() {
  printf 'abc1234\t%s\n%s\n\0' "$1" "$2"
}

accepts_message() {
  if ! message "$1" "$2" | "${script}" --stdin-messages; then
    echo "FAIL: rejected a good message: $1" >&2
    exit 1
  fi
}

rejects_message() {
  local out
  if out=$(message "$1" "$2" | "${script}" --stdin-messages 2>&1); then
    echo "FAIL: accepted a bad message: $1" >&2
    exit 1
  fi
  if [[ "${out}" != *"$3"* ]]; then
    echo "FAIL: wrong failure for $1: ${out}" >&2
    exit 1
  fi
}

# A string of N ASCII letters: N columns.
filler() {
  printf "%${1}s" '' | tr ' ' x
}

# A 72-column subject passes, a 73-column one fails and is named.
accepts_message "26.13: $(filler 65)" "Plain body."
rejects_message "26.13: $(filler 66)" "Plain body." \
  "subject is 73 columns (max 72): abc1234 26.13: $(filler 66)"

# A 73-column body line fails and is named by its number; the same
# width is fine at 72.
accepts_message "26.13: short" "Para one.
$(filler 72)"
rejects_message "26.13: short" "Para one.
$(filler 73)" "body line 2 is 73 columns (max 72): abc1234 $(filler 73)"

# Exempt body lines: a URL, a quote, an indented block, a tab block.
accepts_message "26.13: short" "See https://example.com/$(filler 80) for it.
> quoted $(filler 80)
Example:

    $(filler 90)
	$(filler 90)"

# A missing Co-Authored-By trailer fails on an agent commit (a Claude-Session
# line or an agents: subject) and is fine once the trailer is there.
rejects_message "26.13: short" "Did the work.

Claude-Session: https://claude.ai/code/session_x" \
  "missing Co-Authored-By trailer: abc1234 26.13: short"
accepts_message "26.13: short" "Did the work.

Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_x"
rejects_message "agents: short" "Did the work." \
  "missing Co-Authored-By trailer: abc1234 agents: short"
# A human commit with no session and no trailer passes.
accepts_message "26.13: short" "Did the work."
echo "PASS"
