# shellcheck shell=sh
# The crash states of a dm-log-writes log for guest/sqlite_durability.sh
# (step 12.14): plain functions of their input, so that
# //test/qemu:sqlite_durability_lib_test runs them on the host over
# synthetic logs.

# epoch_states ENTRIES FROM TO CAP SEED EXHAUSTIVE SPB: the crash states of
# the epoch [FROM, TO) of the log ENTRIES lists (crash_states log-list), one
# per line ("<kind> <entry>[:<from>-<to>]..."), after a first line "COV
# <fua> <ordinary> <subsets> <taken> <exhaustive|sampled> <torn>". CAP is
# the epoch's share of the budget (the FUA prefixes and the torn writes, at
# most half of it, count in it); SEED the random subsets' seed; EXHAUSTIVE
# the number of subsets always taken whole, whatever the share; SPB the
# log's sectors per 4 KiB block.
epoch_states() {
	awk -v from="$2" -v to="$3" -v cap="$4" -v seed="$5" -v all="$6" -v spb="$7" '
		# Counters from 0: an unset one is "" as a subscript, which once
		# filed the size of the first ordinary write under sectors[""] and left
		# it untorn.
		BEGIN { a = 0; b = 0; n = 0 }
		$1 >= from && $1 < to && $3 > 0 && $4 !~ /MARK/ && $4 !~ /DISCARD/ {
			if ($4 ~ /FUA/) fua[a++] = $1
			else { sectors[b] = $3; ord[b++] = $1 }
		}
		function emit(line) { if (!(line in seen)) { seen[line] = 1; out[n++] = line } }
		END {
			srand(seed + from)
			allf = ""
			for (i = 0; i < a; i++) allf = allf " " fua[i]
			# Every prefix of the FUA writes, no ordinary write.
			for (p = 1; p <= a; p++) {
				line = ""
				for (i = 0; i < p; i++) line = line " " fua[i]
				emit("fua" line)
			}
			# A torn write: all FUA writes, and one ordinary write of more
			# than one 4 KiB block of which only some blocks landed (its
			# first, its first half, all but its last, its last, all but its
			# first), with every other ordinary write or with none; in log
			# order, at most half the share.
			tears = 0
			tear_cap = int(cap / 2)
			for (k = 0; k < b && tears < tear_cap; k++) {
				nb = int(sectors[k] / spb)
				if (nb < 2) continue
				cut[1] = "0-" spb; cut[2] = "0-" spb * int(nb / 2)
				cut[3] = "0-" spb * (nb - 1)
				cut[4] = spb * (nb - 1) "-" spb * nb
				cut[5] = spb "-" spb * nb
				for (c = 1; c <= 5 && tears < tear_cap; c++) {
					others = ""
					for (i = 0; i < b; i++) if (i != k) others = others " " ord[i]
					before = n
					emit("tear" allf others " " ord[k] ":" cut[c])
					if (n - before + tears < tear_cap)
						emit("tearalone" allf " " ord[k] ":" cut[c])
					tears += n - before
				}
			}
			# All FUA writes, a proper nonempty subset of the ordinary ones
			# (none: the last FUA prefix; all: the next FLUSH prefix).
			total = b >= 2 ? 2 ^ b - 2 : 0
			if (total <= cap || total <= all) {
				mode = "exhaustive"
				for (mask = 1; mask < 2 ^ b - 1; mask++) {
					line = "subset" allf
					for (i = 0; i < b; i++)
						if (int(mask / 2 ^ i) % 2) line = line " " ord[i]
					emit(line)
				}
			} else {
				mode = "sampled"
				for (k = 0; k < b && n < cap; k++) {
					line = "lose1" allf
					for (i = 0; i < b; i++) if (i != k) line = line " " ord[i]
					emit(line)
				}
				for (k = 0; k < b && n < cap; k++) emit("keep1" allf " " ord[k])
				for (tries = 0; n < cap && tries < 50 * cap; tries++) {
					line = "random" allf
					got = 0
					for (i = 0; i < b; i++) if (rand() < 0.5) { line = line " " ord[i]; got++ }
					if (got > 0 && got < b) emit(line)
				}
			}
			print "COV", a + 0, b + 0, total, n + 0, mode, tears + 0
			for (i = 0; i < n; i++) print out[i]
		}' "$1"
}
