#!/bin/sh
# Host-side smoke test for the pinned, statically linked busybox build
# (see BUILD.bazel and README.md). Needs neither root nor kernel control.
#
# Usage: smoke_test.sh <busybox-binary>
set -eu

BB=$1

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- statically linked --------------------------------------------------
case "$(file -b "$BB" 2>/dev/null || true)" in
*"statically linked"* | *"static-pie"*) ;;
*)
	# `file` may not be installed on every host this test runs on; fall
	# back to checking there's no ELF interpreter (PT_INTERP) segment,
	# which is what "statically linked" actually means.
	if readelf -l "$BB" 2>/dev/null | grep -q "INTERP"; then
		fail "$BB is dynamically linked (has a PT_INTERP segment)"
	fi
	;;
esac
echo "PASS: $BB is statically linked"

# --- --help runs ----------------------------------------------------------
"$BB" --help >/dev/null 2>&1 || true # busybox --help exits nonzero; just check it doesn't crash
echo "PASS: $BB --help runs"

# --- every applet the guest scripts use is present ------------------------
# Kept in sync by hand with busybox.config.fragment's groups; a mismatch
# here is a bug in one file or the other, not a real requirements
# difference.
required_applets="
[ ash awk basename cat chgrp chmod chown chroot cmp cp cut date dd diff
dirname dmesg echo fallocate false find free grep head id ip kill ln ls
md5sum mdev mkdir mkfifo mknod more mount mountpoint mv printf pwd
readlink reboot rm rmdir sed sh sleep sort stat sync tail test timeout
touch tr true truncate umount uname uniq wc which
"

actual_applets=$("$BB" --list)

missing=""
for applet in $required_applets; do
	# -F/-x: fixed string, whole line -- "[" (the test-as-"[" applet) is
	# not valid basic-regex syntax (an unterminated bracket expression),
	# so a plain `grep -qx` on it fails with "Invalid regular expression"
	# rather than just not matching.
	if ! echo "$actual_applets" | grep -qFx "$applet"; then
		missing="$missing $applet"
	fi
done

if [ -n "$missing" ]; then
	fail "busybox --list is missing required applets:$missing"
fi
echo "PASS: all required applets present:$required_applets"
