# SPDX-License-Identifier: MIT
# shellcheck shell=sh
# What a test costs (step 26.17): budgets in counts and CPU time, not wall time,
# which is the host's load as much as the test's. Sourced by guest/init, which
# prints the result as the `COST` line run-qemu.sh reads beside the `MEM` line
# (and holds to guest/cpu_budgets.txt and guest/io_budgets.txt), and by the
# guest scripts that read the same counters around an operation
# (request_counts.sh) or check them (boot.sh, ram_disks.sh).
#
# The CPU of the processes that run dcfs's code (the daemon of an e2e test, the
# test binary of a unit test) is read from a cgroup they are started in
# (/tests/cost_exec.sh, which init writes; $COST_CGROUP is its directory), which
# the kernel adds every thread of every process to, exited ones included: a
# daemon that was unmounted and started again, or killed, still counts, and
# /proc/<pid>/stat could not say so (it is gone with the process, and ticks are
# 10 ms). Exact to the microsecond. The I/O of the test's disks is read from
# /sys/block/<disk>/stat, and what the same processes caused from the cgroup's
# io.stat. guest/lib.sh's start_daemon starts the daemon with $COST_EXEC. A guest
# that systemd starts (guest/systemd_run.sh) has neither the variable nor the
# line: its tests have no budgets.

# cost_cpu_us: the CPU time, in microseconds, of everything that has run in the
# cgroup so far (awk fails on the missing file if there is no cgroup).
cost_cpu_us() {
	awk '/^usage_usec / { print $2 }' "$COST_CGROUP/cpu.stat"
}

# cost_disks: the sysfs directories of the disks of the test: the virtio disks
# and the SCSI disks behind the virtio-scsi controller (all but the coverage
# disk, which the profiles are written to after the test, and the disk of a
# Debian root file system, which is read for the programs the test runs: $COV
# and $ROOTFS, guest/init's) and the loop devices, which are the RAM disks (a
# loop device that is not attached counts nothing). Device-mapper devices are
# not here: they sit on these, and their I/O is these disks' I/O.
cost_disks() {
	for cd_dir in /sys/block/vd* /sys/block/sd* /sys/block/loop*; do
		# A guest with no virtio or SCSI disk and no loop device (a unit test
		# without disks): the pattern is left as it is.
		[ -e "$cd_dir/stat" ] || continue
		[ "/dev/${cd_dir##*/}" = "${COV:-}" ] && continue
		[ "/dev/${cd_dir##*/}" = "${ROOTFS:-}" ] && continue
		echo "$cd_dir"
	done
}

# cost_sum_disk_stats: /sys/block/<disk>/stat lines on stdin, one per disk;
# "reads writes sectors_read sectors_written flushes" summed over them: fields 1
# and 2, 5 and 6, 3, 7 and 16 (Documentation/admin-guide/iostats.rst; the times,
# the I/Os in flight and the discards are not counted). Reads and writes are the
# requests issued, completed plus merged: the block layer merges neighbours by
# the timing of its queue, which moves the completed count and not what the test
# did. test/qemu/README.md, "Budgets in three parts", has the measurements.
cost_sum_disk_stats() {
	awk '{ r += $1 + $2; w += $5 + $6; sr += $3; sw += $7; f += $16 } END { print r + 0, w + 0, sr + 0, sw + 0, f + 0 }'
}

# cost_disk_counts: the disks' counters now, as cost_sum_disk_stats prints them.
cost_disk_counts() {
	for cdc_dir in $(cost_disks); do
		cat "$cdc_dir/stat"
	done | cost_sum_disk_stats
}

# cost_disk_delta BEFORE AFTER: two cost_disk_counts, as the I/O of the test in
# the COST line's words.
cost_disk_delta() {
	echo "$1 $2" | awk '{ printf "reads=%d writes=%d sectors_read=%d sectors_written=%d flushes=%d\n", $6 - $1, $7 - $2, $8 - $3, $9 - $4, $10 - $5 }'
}

# cost_cgroup_io DEVICES: what the tasks of the cgroup (the daemon, or a unit
# test's binary) caused on the disks of DEVICES ("major:minor" words), from the
# io controller's io.stat: the I/Os and bytes they read and wrote, exited
# tasks included (the same reason as the CPU time: /proc/<pid>/io goes with the
# process). A device-mapper device on a disk has a line of its own, for the same
# bio, so only the disks are summed.
cost_cgroup_io() {
	awk -v devs="$1" 'BEGIN { n = split(devs, d, " "); for (i = 1; i <= n; i++) want[d[i]] = 1 }
		($1 in want) { for (i = 2; i <= NF; i++) { split($i, kv, "="); t[kv[1]] += kv[2] } }
		END { printf "dcfs_reads=%d dcfs_writes=%d dcfs_read_bytes=%d dcfs_write_bytes=%d\n", t["rios"], t["wios"], t["rbytes"], t["wbytes"] }' "$COST_CGROUP/io.stat"
}

# cost_report: the COST line, after a sync (the writes the test left in the
# page cache are the test's I/O too, and happen at a moment that depends on
# nothing but the sync). COST_DISKS_BEFORE is cost_disk_counts at the start.
cost_report() {
	sync
	cost_devs=$(for cr_dir in $(cost_disks); do cat "$cr_dir/dev"; done)
	echo "COST cpu_us=$(cost_cpu_us) $(cost_disk_delta "$COST_DISKS_BEFORE" "$(cost_disk_counts)") $(cost_cgroup_io "$cost_devs")"
}

# cost_field NAME WORDS...: the value of NAME in words of name=value.
cost_field() {
	cf_name=$1
	shift
	echo "$@" | tr ' ' '\n' | awk -F= -v k="$cf_name" '$1 == k { print $2 }'
}

# cost_selfcheck DIR MAJOR:MINOR: the accounting sees real I/O. DIR is a file
# system on the disk MAJOR:MINOR; as a member of the cgroup, dd writes 1 MiB to
# a file in it with direct I/O (so that the write is the writer's own, not a
# flusher's), and the disks' counters must have risen by that many sectors and
# at least one write, and the cgroup's by that many bytes. Prints what is
# wrong, if anything, and fails: counters that read nothing would make every
# budget pass (a count below its budget always does).
cost_selfcheck() {
	cs_disks=$(cost_disk_counts)
	cs_io=$(cost_cgroup_io "$2")
	${COST_EXEC:-} dd if=/dev/zero of="$1/cost-selfcheck" bs=1048576 count=1 oflag=direct 2>/dev/null || {
		echo "dd of 1 MiB to $1 failed"
		return 1
	}
	sync
	cs_delta=$(cost_disk_delta "$cs_disks" "$(cost_disk_counts)")
	cs_io_after=$(cost_cgroup_io "$2")
	cs_sectors=$(cost_field sectors_written "$cs_delta")
	cs_writes=$(cost_field writes "$cs_delta")
	cs_bytes=$(($(cost_field dcfs_write_bytes "$cs_io_after") - $(cost_field dcfs_write_bytes "$cs_io")))
	cs_wios=$(($(cost_field dcfs_writes "$cs_io_after") - $(cost_field dcfs_writes "$cs_io")))
	[ "$cs_sectors" -ge 2048 ] && [ "$cs_writes" -ge 1 ] &&
		[ "$cs_bytes" -ge 1048576 ] && [ "$cs_wios" -ge 1 ] && return 0
	echo "1 MiB written with direct I/O as a member of the cgroup counted as: disks $cs_delta; cgroup $cs_wios writes, $cs_bytes bytes (device $2)"
	return 1
}
