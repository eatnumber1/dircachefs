#!/bin/sh
# dcfs --help and --version, run under the installed name `dcfs` (Abseil's
# --help only lists flags from files it thinks are "main" files, by default
# those named after the program, so the installed name used to matter).
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs

# Abseil exits 1 after --help whatever happens, so the status is not checked.
out=$("$DCFS" --help 2>&1)

# The flags dcfs/main.cc defines.
for f in source cache_db attr_timeout_sec entry_timeout_sec \
	sync_interval_sec foreground allow_other fuse_opt; do
	if echo "$out" | grep -q -- "--$f"; then
		pass "help-lists-$f"
	else
		fail "help-lists-$f" "--$f not in --help output"
	fi
done

if echo "$out" | grep -q "No flags matched"; then
	fail help-has-flags "output says 'No flags matched': $out"
else
	pass help-has-flags
fi

vout=$("$DCFS" --version 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass version-exit-status
else
	fail version-exit-status "exit status $rc, want 0"
fi
if echo "$vout" | grep -q '^dcfs [0-9A-Za-z]'; then
	pass version-line
else
	fail version-line "output: $vout"
fi

exit "$FAILED"
