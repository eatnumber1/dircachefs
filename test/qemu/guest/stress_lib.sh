# The fsstress/fsx test's checks (step 11.2b), sourced by guest/stress.sh
# after lib.sh and by test/qemu/stress_checks_test.sh on the host, which feeds
# them trees and logs it has damaged on purpose to prove they reject.
#
# TESTUTIL names tools/testutil (default /bin/testutil).

# stress_digest DIR: what the comparison covers, as lines of text: testutil's
# names-dump (every name, sorted, with its type, mode, link count, owner,
# size, symlink target, device numbers and xattrs, in hex), every object's
# mtime and ctime (seconds), and the md5 of every regular file's whole
# contents (reading changes only atime).
#
# What it leaves out: atime; directory sizes; st_blocks; inode numbers and
# generations; the records of names that are absent (an absent name shows
# only through a listing that lacks it, and through ENOENT on lookups the run
# made itself); extended attributes of a file nobody listed (it lists every
# name's).
stress_digest() {
	"${TESTUTIL:-/bin/testutil}" names-dump "$1" meta
	(cd "$1" && find . | sort | while IFS= read -r sd_f; do
		stat -c '%n mtime=%Y ctime=%Z' "$sd_f"
	done) || echo "stat walk failed: $?"
	(cd "$1" && find . -type f | sort | while IFS= read -r sd_f; do
		md5sum "$sd_f"
	done) || echo "md5 walk failed: $?"
}

# stress_same_tree NAME A B: the check NAME passes if the trees A (seen
# through dcfs) and B (the backing filesystem's own) digest the same
# (stress_digest), and fails, showing the first differences, if not. An empty
# tree fails too: a run that made nothing proves nothing.
stress_same_tree() {
	sst_a=$(stress_digest "$2")
	sst_b=$(stress_digest "$3")
	if [ -z "$sst_a" ] || [ "$(printf '%s\n' "$sst_a" | wc -l)" -lt 3 ]; then
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

# fsx_disabled LOG: the features fsx turned off ("... <feature>, disabling!"
# lines of its output), one per line, sorted: "<feature>".
fsx_disabled() {
	sed -n 's/^.*: \(.*\), disabling!$/\1/p' "$1" | sort -u
}

# fsx_disabled_verdict NAME LOG EXPECTED: the check NAME passes if the set of
# features fsx disabled (fsx_disabled) is exactly the lines of file EXPECTED:
# a feature that stops working shows as a new line, one that starts working
# as a missing one (and the day dcfs supports it, the expectation moves).
fsx_disabled_verdict() {
	fsx_disabled "$2" >"${TMPDIR:-/tmp}/fsx-disabled.txt"
	if command diff "$3" "${TMPDIR:-/tmp}/fsx-disabled.txt" >"${TMPDIR:-/tmp}/fsx-disabled.diff"; then
		pass "$1"
	else
		fail "$1" "the features fsx disabled are not the expected ones (< expected, > actual)"
		cat "${TMPDIR:-/tmp}/fsx-disabled.diff"
	fi
}

# fsstress_results LOG: from fsstress -v's log, one line "OP ERRNO COUNT" per
# operation and errno (0: it succeeded), sorted. The operation lines are
# "<pid>/<opno>: <op> <args> <errno>" (creat: "... <errno> <errno of the
# attribute call>"; some print "error <errno>"; copyrange and splice print
# no number when they succeed). Left out: lines that did not run an operation
# ("<op> - no filename", "... add id=", "... del entry:", "... source
# entry:", "<op> - ... zero size"), since the run does not choose them.
fsstress_results() {
	awk '
		$1 !~ /^[0-9]+\/[0-9]+:$/ { next }
		/ - / || /id=[0-9-]+,parent=/ || /entry: id=/ { next }
		{
			op = $2
			sub(/\(.*$/, "", op)
			if ($0 ~ / error [0-9]+$/) e = $NF
			else if (op == "creat" && $(NF - 1) ~ /^-?[0-9]+$/) e = $(NF - 1)
			else if ($NF ~ /^-?[0-9]+$/) e = $NF
			else e = 0
			n[op " " e]++
		}
		END { for (k in n) print k, n[k] }
	' "$1" | sort
}

# fsstress_verdict NAME LOG MIN: the check NAME passes if, in LOG, each of
# creat, mkdir, link, symlink, rename, unlink and write succeeded at least
# MIN times (a run whose operations all fail proves nothing: fsstress itself
# exits 0 whatever the operations returned) and nothing failed with EIO (5).
fsstress_verdict() {
	fsstress_results "$2" | awk -v min="$3" '
		{ if ($2 == 0) ok[$1] += $3; if ($2 == 5) eio += $3 }
		END {
			split("creat mkdir link symlink rename unlink write", need, " ")
			for (i = 1; i <= 7; i++)
				if (ok[need[i]] < min) low = low " " need[i] "(" ok[need[i]] + 0 ")"
			if (low != "") { print "too few successes:" low; bad = 1 }
			if (eio > 0) { print eio " operations failed with EIO"; bad = 1 }
			exit bad
		}
	' >"${TMPDIR:-/tmp}/fsstress-verdict.txt"
	if [ $? -eq 0 ]; then
		pass "$1"
	else
		fail "$1" "$(tr '\n' ';' <"${TMPDIR:-/tmp}/fsstress-verdict.txt") want at least $3 successes of each"
	fi
}

# stress_seed: a number from the kernel's random pool, for the random mode.
stress_seed() {
	printf '%d\n' "0x$(cut -c1-7 /proc/sys/kernel/random/uuid)"
}
