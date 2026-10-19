#!/bin/bash
# SPDX-License-Identifier: MIT
# Plan step 26.13: every commit subject on a CI push range starts with the
# plan step it belongs to (`N.M:` or `N:`, optionally with a letter suffix, a
# `-N.M` / `/N.M` second step and a parenthesized note) or one of the
# prefixes `plan:`, `style:`, `process:`, `warnings:`, `notes:`, `audits:`, `agents:`,
# `R<n>:` and `R<n>.<m>:` (docs/style.md, "Docs and commits"). Co-Authored-By
# lines are trailers, not subjects.
#
# Also (docs/style.md 7, "Commit subject" and "Commit body"): a subject and
# every body line are at most 72 columns, except a body line that is indented
# (4 spaces or a tab), quoted with `>`, contains a URL, or is a trailer. An
# agent commit (its body has a `Claude-Session:` line, or its subject is
# `agents:`) ends with a `Co-Authored-By:` trailer. Columns are bytes (LC_ALL=C),
# which is the same as columns while names and messages are ASCII.
#
# Usage: commit_subjects.sh RANGE   (e.g. "$BEFORE..$GITHUB_SHA"; an empty
#                                    or all-zero base checks the tip only)
#        commit_subjects.sh --stdin (one subject per line; the self-check
#                                    //tools:commit_subjects_test uses it)
#        commit_subjects.sh --stdin-log (one `HASH<TAB>SUBJECT` per line, as
#                                    `git log --format='%h%x09%s'` prints)
#        commit_subjects.sh --stdin-messages (NUL-terminated messages, each
#                                    `HASH<TAB>SUBJECT`, a newline and the
#                                    body, as `git log -z --format='%h%x09%B'`
#                                    prints; RANGE reads these from git)
# Merge commits are skipped. Exit status 1 names every offending commit by
# hash and subject.
set -euo pipefail

readonly STEP='[0-9]+(\.[0-9]+[a-z]?)?([-/][0-9]+(\.[0-9]+[a-z]?)?)*( \([^)]*\))?'
readonly PREFIX='(plan|style|process|warnings|notes|audits|agents|R[0-9]+(\.[0-9]+)?)'
readonly PATTERN="^(${STEP}|${PREFIX}): ."
readonly MAX_COLUMNS=72

# Checks one commit: HASH, SUBJECT and BODY (the lines after the subject,
# possibly empty). Prints each problem and returns 1 if there is one.
check_commit() {
  local hash=$1 subject=$2 body=$3
  local LC_ALL=C line number=0 bad=0 has_trailer=0 has_session=0
  if [[ ! "${subject}" =~ ${PATTERN} ]]; then
    echo "commit ${hash}: subject does not start with a plan step (N.M:) or one of" \
      "plan:, style:, process:, warnings:, notes:, audits:, agents:, R<n>.<m>: -> ${subject}" >&2
    bad=1
  fi
  if ((${#subject} > MAX_COLUMNS)); then
    echo "subject is ${#subject} columns (max ${MAX_COLUMNS}): ${hash} ${subject}" >&2
    bad=1
  fi
  while IFS= read -r line; do
    number=$((number + 1))
    case "${line}" in
      Co-Authored-By:*) has_trailer=1 ;;
      Claude-Session:*) has_session=1 ;;
    esac
    case "${line}" in
      ' '* | $'\t'* | '>'* | *'http://'* | *'https://'* | \
        Co-Authored-By:* | Claude-Session:*) continue ;;
    esac
    if ((${#line} > MAX_COLUMNS)); then
      echo "body line ${number} is ${#line} columns (max ${MAX_COLUMNS}): ${hash} ${line}" >&2
      bad=1
    fi
  done <<<"${body}"
  if ((has_session)) || [[ "${subject}" == agents:* ]]; then
    if ((!has_trailer)); then
      echo "missing Co-Authored-By trailer: ${hash} ${subject}" >&2
      bad=1
    fi
  fi
  return "${bad}"
}

# Subject lines only, `HASH<TAB>SUBJECT` (HASH is `-` where the input has none).
check_lines() {
  local bad=0 hash subject
  while IFS=$'\t' read -r hash subject; do
    check_commit "${hash}" "${subject}" "" || bad=1
  done
  return "${bad}"
}

# NUL-terminated messages, `HASH<TAB>SUBJECT`, a newline, then the body.
check_messages() {
  local bad=0 record rest body
  local hash subject
  while IFS= read -r -d '' record || [[ -n "${record}" ]]; do
    hash=${record%%$'\t'*}
    rest=${record#*$'\t'}
    subject=${rest%%$'\n'*}
    body=""
    if [[ "${rest}" == *$'\n'* ]]; then
      body=${rest#*$'\n'}
    fi
    check_commit "${hash}" "${subject}" "${body}" || bad=1
  done
  return "${bad}"
}

# The messages of RANGE (merge commits are skipped).
messages() {
  local range=$1
  case "${range}" in
    ..* | 0000000000000000000000000000000000000000..*)
      git log --no-merges -z --format='%h%x09%B' -n 1 "${range#*..}"
      ;;
    *) git log --no-merges -z --format='%h%x09%B' "${range}" ;;
  esac
}

main() {
  if [[ $# -ne 1 ]]; then
    echo "usage: $0 RANGE | --stdin | --stdin-log | --stdin-messages" >&2
    return 2
  fi
  case "$1" in
    --stdin) sed 's/^/-\t/' | check_lines ;;
    --stdin-log) check_lines ;;
    --stdin-messages) check_messages ;;
    *) messages "$1" | check_messages ;;
  esac
}

main "$@"
