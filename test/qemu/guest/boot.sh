#!/bin/sh
# dcfs's own QEMU boot smoke test (not adapted from fuse-generation-qemu;
# that repo's guest/run-tests.sh is the LKML patch's test suite and tests
# a different daemon).
#
# Checks that dcfs and fhtest are present and runnable and that the vdb
# scratch disk (mkfs'd ext4 by run-qemu.sh) mounts. Run as /tests/boot.sh
# by guest/init when booted with dcfs_test=boot.sh; prints one "TEST ...
# PASS/FAIL" line per check and exits nonzero if any check failed. init
# turns that into the final ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
. "$(dirname "$0")/lib.sh"

KVER=$(uname -r)
echo "boot.sh: kernel $KVER"

# Step 3.1a: when booted with --//test/qemu:kernel=stock, this is
# //third_party/linux:bzImage, the pinned upstream kernel (see
# third_party/linux/README.md for the pin); check its version and that a
# couple of third_party/linux/kernel.config's fragment options actually
# took effect, by behavior (no /proc/config.gz: CONFIG_IKCONFIG is off, see
# the fragment's size-minimization goals). Skipped entirely against the
# default patched kernel, which has its own, different version.
case "$KVER" in
7.2.9*)
	pass stock-kernel-version

	# CONFIG_NAMESPACES + CONFIG_NET_NS/CONFIG_USER_NS: procfs only
	# exposes a namespace's /proc/self/ns/<type> entry when that
	# namespace type is actually compiled in.
	if [ -e /proc/self/ns/net ] && [ -e /proc/self/ns/user ]; then
		pass stock-kernel-namespaces
	else
		fail stock-kernel-namespaces "missing /proc/self/ns/{net,user}"
	fi

	# CONFIG_CGROUPS: mounting cgroup2 fails outright without it.
	mkdir -p /cgroup_test
	if mount -t cgroup2 cgroup2 /cgroup_test; then
		pass stock-kernel-cgroups
		umount /cgroup_test
	else
		fail stock-kernel-cgroups "mount -t cgroup2 failed"
	fi
	;;
*)
	echo "boot.sh: not the stock kernel, skipping stock-kernel-* checks"
	;;
esac

# Step 4.4: busybox is now //third_party/busybox:busybox_build (pinned
# 1.38.0, built by Bazel), not a host binary symlinked in by the removed
# test/qemu/kernel.bzl repo rule. `busybox` with no arguments prints its
# own version banner as its first line; a host-installed busybox (Debian's
# is 1.36.1 as of this writing) would fail this check.
BBVER=$(busybox | head -n 1)
case "$BBVER" in
*"v1.38.0"*) pass busybox-version ;;
*) fail busybox-version "expected busybox v1.38.0, got: $BBVER" ;;
esac

if [ -x /bin/dcfs ]; then
	pass dcfs-present
else
	fail dcfs-present "not found or not executable"
fi

# dcfs uses Abseil flags: --help can exit nonzero after printing usage
# (Abseil treats an unrecognized/help flag as a parse error for --help;
# --helpfull is the well-known one), so accept either a zero exit or
# usage-looking output.
OUT=$(/bin/dcfs --help 2>&1)
RC=$?
if [ "$RC" -eq 0 ] || printf '%s' "$OUT" | grep -q -e '--source' -e '[Uu]sage'; then
	pass dcfs-help
else
	fail dcfs-help "rc=$RC: $OUT"
fi

if [ -x /bin/fhtest ]; then
	pass fhtest-present
else
	fail fhtest-present "not found or not executable"
fi

if [ -b /dev/vdb ]; then
	pass vdb-present
else
	fail vdb-present "/dev/vdb missing"
fi

if mount -t ext4 /dev/vdb /src; then
	pass vdb-mount
	umount /src
else
	fail vdb-mount "mount failed"
fi

exit "$FAILED"
