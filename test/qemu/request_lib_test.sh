#!/bin/bash
# Self-check of the request-count gate (step 26.4b): the real
# guest/request_lib.sh delta over canned cost-counter snapshots
# (dcfs/testonly/cost_counter.h's format), and strace_lib.sh's budget
# comparison failing a request count above its budget, naming the budgets
# file. Without this, a delta that read nothing would pass every budget.
#
# Usage: request_lib_test.sh <strace_lib.sh> <request_lib.sh>
set -euo pipefail

STRACE_LIB=$(readlink -f "$1")
REQUEST_LIB=$(readlink -f "$2")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# shellcheck source=/dev/null
. "${STRACE_LIB}"
# shellcheck source=/dev/null
. "${REQUEST_LIB}"

printf 'steps 10\ntransactions 1\ndurable_transactions 0\nbacking_calls 3\nrequest.GETATTR 4\nrequest.LOOKUP 2\n' >"${WORK}/before"
printf 'steps 50\ntransactions 2\ndurable_transactions 0\nbacking_calls 9\nrequest.GETATTR 5\nrequest.LOOKUP 12\nrequest.READDIRPLUS 3\n' >"${WORK}/after"
request_delta "${WORK}/before" "${WORK}/after" >"${WORK}/counts"
expected='LOOKUP 10
GETATTR 1
READDIRPLUS 3
READDIR 0
OPENDIR 0
OPEN 0'
[[ "$(cat "${WORK}/counts")" == "${expected}" ]] ||
  fail "request_delta: '$(cat "${WORK}/counts")'"
echo "PASS: the delta of each opcode between two snapshots"

printf 'op LOOKUP 10\nop GETATTR 1\nop READDIRPLUS 3\n' >"${WORK}/budgets"
strace_budget_compare "${WORK}/budgets" "${WORK}/counts" op request_budgets.txt ||
  fail "counts equal to the budgets were refused"
printf 'op LOOKUP 9\nop GETATTR 1\n' >"${WORK}/tight"
out=$(strace_budget_compare "${WORK}/tight" "${WORK}/counts" op request_budgets.txt) &&
  fail "a request count above its budget passed"
[[ "${out}" == "op LOOKUP: count rose from 9 to 10; raising a budget is a deliberate edit of request_budgets.txt whose commit says why" ]] ||
  fail "budget message: '${out}'"
echo "PASS: a request count above its budget fails, naming the file"
