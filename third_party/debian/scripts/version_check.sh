#!/bin/sh
# Test first (docs/plan/phases/04-pinned-host-tools.md, part c): a host-side
# check (no root, no kernel, no guest boot -- same spirit as
# third_party/qemu/busybox's own smoke_test.sh) that the exact package
# versions rules_distroless resolved from the pinned snapshot.debian.org
# timestamp (MODULE.bazel's apt.install) still match
# third_party/debian/packages.lock, the checked-in lock this project keeps
# for the packages it explicitly asked for (not the full ~136-package
# transitive closure -- see README.md's "Lockfile" section for why).
#
# Run as a Bazel sh_test (see ../BUILD.bazel's version_check_test):
#   version_check.sh <dpkg_status.tar> <packages.lock>
set -eu

STATUS_TAR=$1
LOCK=$2

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

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
	echo "If this is an intentional snapshot/package-list update, refresh" \
		"$LOCK to match and update README.md's pin." >&2
	exit 1
fi

echo "version_check.sh: $(wc -l <"$LOCK") package versions match $LOCK"
