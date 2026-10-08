# The fsstress/fsx test's checks (step 11.2b), sourced by guest/stress.sh
# after lib.sh and by test/qemu/stress_checks_test.sh on the host, which feeds
# them trees and logs it has damaged on purpose to prove they reject.
#
# TESTUTIL names tools/testutil (default /bin/testutil).

# stress_digest DIR: everything the cache could have wrong about the tree
# under DIR, as lines of text: testutil's names-dump (every name in
# directory order, type, mode, link count, owner, size, symlink target and
# xattrs, in hex) and the md5 of every regular file's whole contents.
stress_digest() {
	"${TESTUTIL:-/bin/testutil}" names-dump "$1" meta || echo "names-dump failed: $?"
	(cd "$1" && find . -type f | sort | while IFS= read -r sd_f; do
		md5sum "$sd_f"
	done) || echo "md5 walk failed: $?"
}

# stress_same_tree NAME A B: the check NAME passes if the trees A (seen
# through dcfs, from its cache) and B (the backing filesystem's own) digest
# the same, and fails, showing the first differences, if not. An empty tree
# fails too: a run that made nothing proves nothing.
stress_same_tree() {
	sst_a=$(stress_digest "$2")
	sst_b=$(stress_digest "$3")
	if [ -z "$sst_a" ]; then
		fail "$1" "$2 is empty: the run made nothing to compare"
	elif [ "$sst_a" = "$sst_b" ]; then
		pass "$1"
	else
		fail "$1" "$2 differs from $3 (first differences below)"
		printf '%s\n' "$sst_a" >"${TMPDIR:-/tmp}/stress-a.txt"
		printf '%s\n' "$sst_b" >"${TMPDIR:-/tmp}/stress-b.txt"
		command diff "${TMPDIR:-/tmp}/stress-a.txt" "${TMPDIR:-/tmp}/stress-b.txt" | head -20
	fi
}

# fsx_verdict NAME LOG OPS: the check NAME passes if fsx's log LOG says it
# completed all OPS operations ("All OPS operations completed A-OK!"); a
# crash, a "mapped read: BAD DATA" or a short run does not say it.
fsx_verdict() {
	if grep -q "^All $3 operations completed A-OK!\$" "$2"; then
		pass "$1"
	else
		fail "$1" "fsx did not report all $3 operations completed A-OK; its log ends:"
		tail -15 "$2"
	fi
}

# fsstress_ops LOG: from fsstress -v's log, one line "OP COUNT" per operation
# it ran (the "<pid>/<opno>: <op> ..." lines), then "total N"; N is 0 for a
# log with none.
fsstress_ops() {
	awk '
		$1 ~ /^[0-9]+\/[0-9]+:$/ { n[$2]++; t++ }
		END {
			for (k in n) print k, n[k]
			print "total", t + 0
		}
	' "$1" | sort
}

# fsstress_verdict NAME LOG MINOPS: the check NAME passes if LOG shows at
# least MINOPS operations ran (a run that died at the start or whose
# operations all failed to start proves nothing).
fsstress_verdict() {
	fv_total=$(fsstress_ops "$2" | awk '$1 == "total" { print $2 }')
	if [ "${fv_total:-0}" -ge "$3" ]; then
		pass "$1"
	else
		fail "$1" "fsstress ran ${fv_total:-0} operations, want at least $3"
	fi
}
