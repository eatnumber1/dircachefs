#!/bin/sh
# Test first (docs/plan/phases/04-pinned-host-tools.md, part c; extended by
# docs/plan/review-fixes.md R3/L9): a host-side check (no root, no kernel, no
# guest boot) that the Debian packages the build actually uses are exactly the
# ones checked in:
#
#   1. the packages MODULE.bazel names (apt.install): the live resolution's
#      versions, from rules_distroless's synthesized dpkg status file, equal
#      packages.lock (name and full Debian version, epoch included);
#   2. every package fetched, transitive ones too: the list of .deb files and
#      their sha256, as recorded in MODULE.bazel.lock (which Bazel verifies at
#      download time and, with `common --lockfile_mode=error`, never rewrites
#      silently), equals debs.lock.
#
# Run as a Bazel sh_test (see ../BUILD.bazel's version_check_test):
#   version_check.sh <dpkg_status.tar> <packages.lock> <lock_debs.sh> \
#                    <MODULE.bazel.lock> <debs.lock>
set -eu
export LC_ALL=C

STATUS_TAR=$1
LOCK=$2
LOCK_DEBS=$3
MODULE_LOCK=$4
DEBS_LOCK=$5

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

stale=0

tar -xOf "$STATUS_TAR" ./var/lib/dpkg/status >"$WORK/status"

awk '
	/^Package: / { pkg = $2 }
	/^Version: / { print pkg, $2 }
' "$WORK/status" | sort >"$WORK/actual.lock"

sort "$LOCK" >"$WORK/expected.lock"

if ! diff -u "$WORK/expected.lock" "$WORK/actual.lock"; then
	echo
	echo "version_check.sh: resolved package versions no longer match" \
		"$LOCK (see diff above)." >&2
	stale=1
fi

sh "$LOCK_DEBS" "$MODULE_LOCK" >"$WORK/actual.debs"
if ! diff -u "$DEBS_LOCK" "$WORK/actual.debs"; then
	echo
	echo "version_check.sh: the .deb files and checksums in MODULE.bazel.lock" \
		"no longer match $DEBS_LOCK (see diff above)." >&2
	stale=1
fi

if [ "$stale" -ne 0 ]; then
	echo "If this is an intentional snapshot/package-list update, run" \
		"\`bazel mod deps --lockfile_mode=update\`, refresh $LOCK and" \
		"$DEBS_LOCK (scripts/lock_debs.sh MODULE.bazel.lock >" \
		"third_party/debian/debs.lock) and update README.md's pin." >&2
	exit 1
fi

echo "version_check.sh: $(wc -l <"$LOCK") named package versions match $LOCK;" \
	"$(wc -l <"$DEBS_LOCK") .deb checksums match $DEBS_LOCK"
