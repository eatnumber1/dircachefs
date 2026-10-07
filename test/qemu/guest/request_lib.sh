# shellcheck shell=sh
# Step 26.4b: FUSE requests per user-level operation, from the testonly
# checking daemon's cost counter (dcfs/testonly/cost_counter.h: it rewrites
# $DCFS_COUNTERS_FILE after every request, "request.<OPCODE> <n>" lines),
# budgeted like the syscall budgets (guest/request_budgets.txt, compared by
# strace_lib.sh's strace_budget_compare). Sourced by guest/request_counts.sh
# and by //test/qemu:request_lib_test.

# The opcodes counted and budgeted.
REQUEST_KINDS="LOOKUP GETATTR READDIRPLUS READDIR OPENDIR OPEN"

# request_value FILE OPCODE: the count of OPCODE in a counters snapshot (0 if
# it has none).
request_value() {
	awk -v k="request.$2" '$1 == k { v = $2 } END { print v + 0 }' "$1" 2>/dev/null || echo 0
}

# request_delta BEFORE AFTER: one "OPCODE N" line per REQUEST_KINDS, N the
# requests of that opcode between the two snapshots.
request_delta() {
	for rd_kind in $REQUEST_KINDS; do
		echo "$rd_kind $(($(request_value "$2" "$rd_kind") - $(request_value "$1" "$rd_kind")))"
	done
}
