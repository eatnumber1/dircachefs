#!/bin/sh
# Print, one line per fetched .deb (transitive packages included), the file
# name and sha256 that MODULE.bazel.lock records for rules_distroless's apt
# extension: `<file name> <sha256>`, sorted. third_party/debian/debs.lock is
# this script's output, checked in so a pin change shows up as a reviewable
# diff; version_check.sh compares the two.
#
# Usage: lock_debs.sh <MODULE.bazel.lock>
#
# The lock is pretty-printed JSON and every deb_import repo spec reads
#   "target_name": "...",
#   "urls": [
#     "https://snapshot.debian.org/archive/debian/<timestamp>/pool/.../<file>.deb"
#   ],
#   "sha256": "<hex>",
# in that order, so a line-based awk is enough (no JSON tool on the host).
set -eu

awk '
	/"repoRuleId": ".*deb_import\.bzl%deb_import"/ { in_deb = 1; next }
	in_deb && /"urls": \[/ { want_url = 1; next }
	in_deb && want_url {
		url = $0
		sub(/^[[:space:]]*"/, "", url)
		sub(/".*$/, "", url)
		n = split(url, parts, "/")
		file = parts[n]
		want_url = 0
		next
	}
	in_deb && /"sha256": "/ {
		sum = $0
		sub(/^.*"sha256": "/, "", sum)
		sub(/".*$/, "", sum)
		print file, sum
		in_deb = 0
	}
' "$1" | LC_ALL=C sort
