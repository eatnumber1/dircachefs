#!/bin/sh
# SPDX-License-Identifier: MIT
# Host-side syntax check of every shell script the guests run (step 26.33):
# `sh -n` under the guest's own shell, Alpine's busybox-static (ash), which
# runs on the host because it is a static binary. A parse error stops a guest
# script at that line without a word, and everything after it never runs
# (names_random.sh did not run past line 118 for most of the project).
#
# Usage: guest_syntax_test.sh <busybox-binary> <script>...
set -u

BB=$1
shift
FAILED=0
COUNT=0
for script in "$@"; do
	COUNT=$((COUNT + 1))
	if ! out=$("$BB" sh -n "$script" 2>&1); then
		echo "FAIL: $script: $out" >&2
		FAILED=1
	fi
done
if [ "$COUNT" -eq 0 ]; then
	echo "FAIL: no scripts given" >&2
	exit 1
fi
[ "$FAILED" -eq 0 ] && echo "PASS: $COUNT guest scripts parse under busybox ash"
exit "$FAILED"
