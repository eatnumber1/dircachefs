#!/bin/sh
# SPDX-License-Identifier: MIT
# dcfs step 17.1 acceptance test: xfstests' generic tests against dcfs.
#
# Runs in the xfstests root file system (test/qemu/scripts/mkxfstests_rootfs.py:
# bash, GNU coreutils, util-linux's mount, xfs_io, perl and the rest of what
# `check` and the tests call, xfstests under /xfstests), which guest/init
# chroots into; this script is /tests/xfstests.sh there. vdb and vdc are the
# backing devices (ext4, xfs or btrfs, the test's matrix variant). dcfs serves
# a directory, not a device (step 15.11), so this script mounts each device
# natively, on /backing/test and /backing/scratch, and makes xfstests'
# TEST_DEV and SCRATCH_DEV the directory "fs" of each: xfstests mounts them
# with `mount -t fuse.dcfs` (FSTYP=fuse, FUSE_SUBTYP=.dcfs), which runs
# mount.dcfs, and finds the mount again by its source and unmounts it by name,
# both of which are the directory. (The directory cannot be the native mount's
# root: `umount ROOT` would unmount the native file system, not dcfs.) With
# xfstests_native=1 xfstests mounts the devices itself. xfstests' own checks
# decide what does not apply to a FUSE file system and report it "not run".
#
# The sets of tests:
#   normal  the generic group "auto" less xfstests.excluded and
#           xfstests.<fstype>.excluded (tests that cannot run in this guest,
#           each with its reason) and less the slow tests (xfstests.slow and
#           xfstests.<fstype>.slow: tests that take most of the limit or more
#           through dcfs; plan step 17.3 owns them);
#   slow    the slow tests, with a limit of 1500 s each (the weekly run: the
#           xfstests_slow_<n>.sh wrappers; lists xfstests.slow.<fstype>.*).
# They are divided into shards by position (xfstests_<n>.sh set XFSTESTS_SHARD
# and XFSTESTS_SHARDS and source this file), and run by `check` a few at a time
# under a time limit. The outcome of each test must be the listed one
# (xfstests_lib.sh: xfstests_gate): a failure is listed, with its reason, in
# <set>.<fstype>.expected_failures, a "not run" test with xfstests' reason in
# <set>.<fstype>.notrun; a test that does something else, including a listed
# one that passes, fails the run, so the lists stay honest.
#
# Kernel command line, for working on the suite (run-qemu.sh --cmdline):
#   xfstests_run=generic/001,generic/002   run just these tests, no gate
#   xfstests_native=1                      run them on the native file system
#                                          (no dcfs), to see what the backing
#                                          answers; no gate
#   xfstests_timeout=<seconds>             the per-test limit (200; 1500 slow)
#   xfstests_batch=<n>                     tests per check (10)
#   xfstests_budget=<seconds>              the run's own time (3400): no batch
#                                          is started that could not finish
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/xfstests_lib.sh"
. "$(dirname "$0")/cost_lib.sh"

XF=/xfstests
LISTS=$XF/dcfs
GROUP_LIST=$XF/tests/generic/group.list
RESULTS=/tmp/xfstests_results.txt
SHARD_LIST=/tmp/xfstests_shard.txt
RAW_TEST=/dev/vdb
RAW_SCRATCH=/dev/vdc
BACKING_TEST=/backing/test
BACKING_SCRATCH=/backing/scratch
TEST_DIR=/mnt/test
SCRATCH_MNT=/mnt/scratch

export PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# The tests run as root, and as the fsgqa users (/etc/passwd in the image).
export HOME=/root
# glibc's error messages from the musl tools (third_party/xfstests/shim/glibc_strerror.c).
export LD_PRELOAD=/usr/lib/glibc_strerror.so

cmdline_word() { sed -n "s/.*\\b$1=\\([^ ]*\\).*/\\1/p" /proc/cmdline; }
RUN_ONLY=$(cmdline_word xfstests_run | tr ',' ' ')
NATIVE=$(cmdline_word xfstests_native)
SET=${XFSTESTS_SET:-normal}
LIMIT=$(cmdline_word xfstests_timeout)
if [ "$SET" = slow ]; then LIMIT=${LIMIT:-1500}; else LIMIT=${LIMIT:-200}; fi
BATCH=$(cmdline_word xfstests_batch)
BATCH=${BATCH:-10}
BUDGET=$(cmdline_word xfstests_budget)
BUDGET=${BUDGET:-3400}

# What xfstests calls TEST_DEV and SCRATCH_DEV: the devices themselves when it
# mounts them (xfstests_native=1), else the directory of the native mount that
# dcfs serves (the header says why it is a directory below the mount's root).
if [ "$NATIVE" = 1 ]; then
	TEST_DEV=$RAW_TEST
	SCRATCH_DEV=$RAW_SCRATCH
else
	TEST_DEV=$BACKING_TEST/fs
	SCRATCH_DEV=$BACKING_SCRATCH/fs
fi

cleanup() {
	rc=$?
	umount "$SCRATCH_MNT" 2>/dev/null || true
	umount "$TEST_DIR" 2>/dev/null || true
	if [ "$NATIVE" != 1 ]; then
		umount "$BACKING_SCRATCH" 2>/dev/null || true
		umount "$BACKING_TEST" 2>/dev/null || true
	fi
	return "$rc"
}
trap cleanup EXIT

echo "xfstests.sh: kernel $(uname -r)"
require_commands bash perl xfs_io mount findmnt flock timeout logger logread

# The file system type of a device, as blkid says (empty for none).
fstype_of() {
	blkid "$1" 2>/dev/null | sed -n 's/.*TYPE="\([^"]*\)".*/\1/p'
}

FSTYPE=$(fstype_of "$RAW_TEST")
echo "xfstests.sh: backing file system: ${FSTYPE:-unknown}"
case "$FSTYPE" in
ext4 | xfs | btrfs) ;;
*)
	fail xfstests-backing "$RAW_TEST is '$FSTYPE', not ext4, xfs or btrfs"
	exit "$FAILED"
	;;
esac
[ "$(fstype_of "$RAW_SCRATCH")" = "$FSTYPE" ] ||
	fail xfstests-backing "$RAW_SCRATCH is not $FSTYPE"

# A new, empty file system on the device. Its status counts: a device that is
# still held (a daemon that did not exit, a mount left behind) would otherwise
# keep the previous test's file system, with a new cache over it.
mkfs_dev() {
	case "$FSTYPE" in
	ext4) mkfs.ext4 -F -q "$1" ;;
	xfs) mkfs.xfs -f -q "$1" ;;
	btrfs) mkfs.btrfs -f -q "$1" ;;
	esac >/tmp/mkfs.out 2>&1
	md_rc=$?
	if [ "$md_rc" -ne 0 ]; then
		fail xfstests-mkfs "mkfs on $1 failed ($md_rc): $(head -n 3 /tmp/mkfs.out)"
		exit "$FAILED"
	fi
}
# backing_mount DEVICE DIR: the native mount dcfs serves a directory of (the
# header says why dcfs gets "fs" below DIR). The mount's status counts: a
# device that did not mount would leave xfstests a directory of the guest's
# own root file system to test.
backing_mount() {
	mkdir -p "$2"
	if ! mount "$1" "$2" >/tmp/mount.out 2>&1 || ! mkdir -p "$2/fs"; then
		bm_why=$(head -n 3 /tmp/mount.out)
		fail xfstests-backing "mounting $1 on $2 failed: $bm_why"
		exit "$FAILED"
	fi
}
# A new scratch file system (and a new cache for it) before each batch of
# tests, as xfstests makes one with its mkfs: for a FUSE file system xfstests
# only deletes the files, which would leave the previous test's directories,
# attributes and hidden files behind. Under dcfs the native mount goes first,
# and its status counts like mkfs's: a daemon that did not exit (or a test's
# process left in the mount) keeps it busy, which is the failure to report.
reset_scratch() {
	umount "$SCRATCH_MNT" 2>/dev/null
	if [ "$NATIVE" = 1 ]; then
		mkfs_dev "$RAW_SCRATCH"
		return
	fi
	if ! umount "$BACKING_SCRATCH" >/tmp/umount.out 2>&1; then
		rs_why=$(head -n 3 /tmp/umount.out)
		fail xfstests-backing "umount $BACKING_SCRATCH failed: $rs_why"
		exit "$FAILED"
	fi
	for rs_suffix in "" -wal -shm; do
		rm -f "/cache/backing_scratch_fs.db$rs_suffix"
	done
	mkfs_dev "$RAW_SCRATCH"
	backing_mount "$RAW_SCRATCH" "$BACKING_SCRATCH"
}

# mdev -s made the device nodes root-only; the tests run as other users too.
chmod 666 /dev/null /dev/zero /dev/full /dev/random /dev/urandom /dev/tty 2>/dev/null
# bash's process substitution (xfstests uses it) needs /dev/fd.
ln -sf /proc/self/fd /dev/fd
ln -sf /proc/self/fd/0 /dev/stdin
ln -sf /proc/self/fd/1 /dev/stdout
ln -sf /proc/self/fd/2 /dev/stderr
mkdir -p /cache "$TEST_DIR" "$SCRATCH_MNT"
# The cache databases on tmpfs, as in every other test: dcfs commits to them
# durably, and on the image's disk each commit waited for the host.
mount -t tmpfs -o size=256m tmpfs /cache
mount -t tmpfs -o size=128m tmpfs /tmp
mount -t fusectl fusectl /sys/fs/fuse/connections
# umount.fuse.dcfs waits for the daemon only where fusectl is mounted: it asks
# fusectl whether the unmount ended the superblock (test/qemu/guest/
# mount_dcfs.sh). Without it xfstests' umount returns while the daemon is still
# shutting down, and the next mount of the same device finds the cache
# database locked: generic/464, 524, 610 and 754 failed that way when the
# guest's CPUs were busy (step 26.22).
if grep -q ' - fusectl ' /proc/self/mountinfo; then
	pass xfstests-fusectl
else
	fail xfstests-fusectl "no fusectl mount: umount.fuse.dcfs would not wait for the daemon"
fi
syslogd -C256 2>/dev/null
cat >$XF/local.config <<EOC
export TEST_DEV=$TEST_DEV
export TEST_DIR=$TEST_DIR
export SCRATCH_DEV=$SCRATCH_DEV
export SCRATCH_MNT=$SCRATCH_MNT
EOC
if [ "$NATIVE" != 1 ]; then
	# The daemon of each mount joins the cgroup its CPU time is counted in
	# (guest/cost_lib.sh), as start_daemon's do: mount(8) runs
	# mount.fuse.dcfs, which this puts in the cgroup and then becomes what it
	# was, dcfs under the name it was called by (what dcfs dispatches on). The
	# daemon is a child of that process and stays in the cgroup; the CPU of a
	# test is then the difference of two readings, and a daemon that exits
	# between them (every unmount) still counts, which the ticks of the live
	# daemons (the first results lines of 17.3) did not.
	mv /sbin/mount.fuse.dcfs /sbin/mount.fuse.dcfs.real
	cat >/sbin/mount.fuse.dcfs <<'EOW'
#!/bin/bash
echo $$ >"${COST_CGROUP:?}/cgroup.procs" && exec -a "$0" /sbin/mount.fuse.dcfs.real "$@"
EOW
	chmod +x /sbin/mount.fuse.dcfs
fi
if [ "$NATIVE" = 1 ]; then
	echo "export FSTYP=$FSTYPE" >>$XF/local.config
	# The native file systems are made by xfstests (mkfs of $FSTYP).
	mkfs_dev "$RAW_TEST"
else
	backing_mount "$RAW_TEST" "$BACKING_TEST"
	backing_mount "$RAW_SCRATCH" "$BACKING_SCRATCH"
	cat >>$XF/local.config <<EOC
export FSTYP=fuse
export FUSE_SUBTYP=.dcfs
EOC
fi

# --- the tests of this shard ---------------------------------------------------

# The lists of this set and file system.
cat "$LISTS/xfstests.excluded" "$LISTS/xfstests.$FSTYPE.excluded" >/tmp/excluded.txt
cat "$LISTS/xfstests.slow" "$LISTS/xfstests.$FSTYPE.slow" >/tmp/slow.txt
if [ "$SET" = slow ]; then
	LISTPFX=xfstests.slow
	xfstests_list_ids /tmp/slow.txt >/tmp/selected.txt
	EXCLUDED_ALL=/tmp/excluded.txt
else
	LISTPFX=xfstests
	cat /tmp/excluded.txt /tmp/slow.txt >/tmp/excluded_and_slow.txt
	xfstests_select "$GROUP_LIST" auto /tmp/excluded_and_slow.txt >/tmp/selected.txt
	EXCLUDED_ALL=/tmp/excluded_and_slow.txt
fi
EXPECTED="$LISTS/$LISTPFX.$FSTYPE.expected_failures"
NOTRUN="$LISTS/$LISTPFX.$FSTYPE.notrun"
if ! xfstests_lists_valid "$EXPECTED" "$EXCLUDED_ALL" "$GROUP_LIST" "$NOTRUN" /tmp/selected.txt; then
	exit "$FAILED"
fi
if [ -n "$RUN_ONLY" ]; then
	echo $RUN_ONLY | tr ' ' '\n' >$SHARD_LIST
	GATE=0
else
	xfstests_shard "${XFSTESTS_SHARD:?run a xfstests_<n>.sh wrapper}" "${XFSTESTS_SHARDS:?}" </tmp/selected.txt >$SHARD_LIST
	GATE=1
fi
[ "$NATIVE" = 1 ] && GATE=0
TOTAL=$(wc -l <$SHARD_LIST)
echo "xfstests.sh: $SET set, shard ${XFSTESTS_SHARD:-}/${XFSTESTS_SHARDS:-}: $TOTAL tests, limit ${LIMIT}s each"

# --- run them --------------------------------------------------------------------

# `check` costs several seconds of CPU in this guest before its first test
# (it sources 6000 lines of shell and forks a few hundred times), as much as
# most "not run" tests take, so it is given BATCH tests at a time. It has no
# time limit of its own, and a hung test cannot say so: run_batch reads
# check's output from a pipe, a line at a time (check prints a test's line when
# the test ends), and the test that has not finished LIMIT seconds after the
# previous one did is killed (check then goes on to the next and reports the
# killed one as failed; the result here is "timeout"). LIMIT is the one timer
# of the run: a deadline for a test that cannot announce its own end, not a
# wait for something that can be announced.

# running_test: the number of the generic test that is running, if one is.
running_test() {
	for rt_pid in $(pgrep -f 'tests/generic/[0-9]'); do
		rt_cmd=$(tr '\0' ' ' <"/proc/$rt_pid/cmdline" 2>/dev/null)
		case "$rt_cmd" in
		*tests/generic/[0-9]*)
			rt_rest=${rt_cmd##*tests/generic/}
			echo "${rt_rest%% *}"
			return 0
			;;
		esac
	done
	return 1
}

# stall_report ID: what the guest is doing while test ID has run for the whole
# limit, for telling a hang from slowness: the processes with their state and
# wait channel, and the daemon's CPU time, threads and kernel stack (a daemon
# that is waiting for the kernel, or the kernel for the daemon, is a hang; a
# daemon that is using CPU is slow).
stall_report() {
	echo "xfstests: $1 has run for ${LIMIT}s; stall report:"
	ps -o pid,ppid,stat,wchan:14,time,args 2>/dev/null | awk '$2 != 2 && $1 != 2' | cut -c1-170 | sed 's/^/xfstests:   ps: /'
	for sr_pid in $(pidof dcfs.real dcfs mount.dcfs 2>/dev/null); do
		echo "xfstests:   dcfs $sr_pid: utime/stime ticks $(cut -d' ' -f14,15 /proc/$sr_pid/stat), $(grep -c . /proc/$sr_pid/task/*/stat 2>/dev/null | wc -l) threads"
		for sr_task in /proc/$sr_pid/task/*; do
			echo "xfstests:   dcfs task ${sr_task##*/}: $(cut -d' ' -f3 $sr_task/stat) wchan $(cat $sr_task/wchan 2>/dev/null)"
		done
		sed 's/^/xfstests:   dcfs stack: /' /proc/$sr_pid/stack 2>/dev/null | head -n 12
	done
	grep -e fuse /proc/self/mountinfo | cut -c1-120 | sed 's/^/xfstests:   mount: /'
}

# kill_test SEQ: kills the running test generic/SEQ and what it started.
kill_test() {
	for kt_pid in $(pgrep -f "tests/generic/$1\$"); do
		pkill -KILL -P "$kt_pid" 2>/dev/null
		kill -KILL "$kt_pid" 2>/dev/null
	done
	killall -9 fsstress fsx xfs_io fio 2>/dev/null
}

# report_failure ID: enough of the evidence of a failed test to triage from the
# serial log.
report_failure() {
	rf_seq=${1#generic/}
	if [ -f "results/generic/$rf_seq.out.bad" ]; then
		command diff -u "tests/generic/$rf_seq.out" "results/generic/$rf_seq.out.bad" 2>/dev/null |
			sed -n '3,40p' | cut -c1-200 | sed 's/^/xfstests:   > /'
	fi
	for rf_ext in mountfail full dmesg; do
		if [ -s "results/generic/$rf_seq.$rf_ext" ]; then
			tail -n 12 "results/generic/$rf_seq.$rf_ext" | cut -c1-200 | sed "s/^/xfstests:   $rf_ext: /"
		fi
	done
}

# run_batch ID...: runs the tests with one check, appends "ID status seconds
# [reason]" to $RESULTS for each that has a result, and writes the ones that
# have none (check was interrupted before them) to /tmp/missing.txt.
run_batch() {
	for rb_id in "$@"; do rm -f "results/generic/${rb_id#generic/}".*; done
	echo "xfstests: batch of $#: $* (resetting the scratch device)"
	reset_scratch
	echo "xfstests: scratch device reset"
	: >/tmp/durations
	: >/tmp/killed
	: >/tmp/missing.txt
	: >/tmp/check.out
	rm -f /tmp/check.fifo
	mkfifo /tmp/check.fifo
	# The subshell says when check is done: a daemon that check started keeps
	# the pipe's write end open (it inherited it), so its end of file would
	# not come.
	{
		bash ./check "$@" 2>&1 </dev/null
		echo "xfstests-check-done $?"
	} >/tmp/check.fifo &
	rb_pid=$!
	exec 3</tmp/check.fifo
	rb_prev=$(date +%s)
	rb_cpu=$(cost_cpu_us)
	rb_io=$(cost_disk_counts)
	while :; do
		rb_read_at=$(date +%s)
		if read -r -t "$LIMIT" rb_line <&3; then
			case "$rb_line" in
			xfstests-check-done*) break ;;
			esac
			echo "$rb_line" >>/tmp/check.out
			case "$rb_line" in
			generic/[0-9]*)
				rb_now=$(date +%s)
				rb_cpu_now=$(cost_cpu_us)
				rb_io_now=$(cost_disk_counts)
				echo "${rb_line%% *} $((rb_now - rb_prev)) $((rb_cpu_now - rb_cpu)) $(cost_disk_delta "$rb_io" "$rb_io_now")" >>/tmp/durations
				rb_prev=$rb_now
				rb_cpu=$rb_cpu_now
				rb_io=$rb_io_now
				;;
			esac
		else
			# LIMIT seconds with no complete line, or the pipe closed: busybox's
			# read says the same for both (status 1), so the time it took tells.
			kill -0 "$rb_pid" 2>/dev/null || break
			[ $(($(date +%s) - rb_read_at)) -lt $((LIMIT - 1)) ] && break
			if rb_seq=$(running_test); then
				echo "generic/$rb_seq" >>/tmp/killed
				stall_report "generic/$rb_seq"
				kill_test "$rb_seq"
			else
				# Not in a test: check itself is stuck.
				echo "xfstests: check made no progress for ${LIMIT}s and runs no test: killed"
				kill -KILL "$rb_pid" 2>/dev/null
			fi
			rb_prev=$(date +%s)
		fi
	done
	exec 3<&-
	wait "$rb_pid"
	umount -l "$SCRATCH_MNT" 2>/dev/null
	rb_failures=$(sed -n 's/^Failures: *//p' /tmp/check.out | tail -n 1)
	rb_ran=$(sed -n 's/^Ran: *//p' /tmp/check.out | tail -n 1)
	for rb_id in "$@"; do
		rb_seq=${rb_id#generic/}
		rb_secs=$(awk -v id="$rb_id" '$1 == id { print $2 }' /tmp/durations | tail -n 1)
		# What the test cost between the previous test's end and its own (the
		# last line for it, if check ran it twice): the CPU of the dcfs
		# processes and the I/O of the disks, in the words
		# test/qemu/scripts/xfstests_costs.py reads; a test with no line (killed
		# before check printed its result) cost nothing we saw.
		rb_cost=$(awk -v id="$rb_id" 'BEGIN { line = "dcfs-cpu=0.000s reads=0 writes=0 sectors_read=0 sectors_written=0 flushes=0" }
			$1 == id { line = sprintf("dcfs-cpu=%.3fs", $3 / 1000000); for (i = 4; i <= NF; i++) line = line " " $i }
			END { print line }' /tmp/durations)
		rb_reason=""
		if grep -qx "$rb_id" /tmp/killed; then
			rb_status=timeout
		elif [ -f "results/generic/$rb_seq.notrun" ]; then
			rb_status=notrun
			rb_reason=$(xfstests_reason "$(head -n 1 "results/generic/$rb_seq.notrun")")
		elif case " $rb_failures " in *" $rb_id "*) true ;; *) false ;; esac; then
			rb_status=fail
		elif case " $rb_ran " in *" $rb_id "*) true ;; *) false ;; esac; then
			rb_status=pass
		else
			echo "$rb_id" >>/tmp/missing.txt
			continue
		fi
		echo "$rb_id $rb_status ${rb_secs:-0} $rb_reason" >>$RESULTS
		echo "xfstests: $rb_id $rb_status ${rb_secs:-0}s $rb_cost${rb_reason:+: $rb_reason}"
		if [ "$rb_status" = fail ] || [ "$rb_status" = timeout ]; then
			report_failure "$rb_id"
		fi
	done
}

# retry_missing: each test check was interrupted before, alone.
retry_missing() {
	cp /tmp/missing.txt /tmp/retry.txt
	while IFS= read -r id2; do
		run_batch "$id2"
		if [ -s /tmp/missing.txt ]; then
			echo "$id2 fail 0" >>$RESULTS
			echo "xfstests: $id2 fail (check ran nothing; its output follows)"
			sed -n '1,30p' /tmp/check.out | sed 's/^/xfstests:   | /'
		fi
	done </tmp/retry.txt
}

# Tests that write to the raw scratch device: the device then holds what they
# made, not the file system of this run, for every test after them in the batch
# (generic/740 makes one of every kind). Under dcfs the scratch "device" is a
# directory and xfstests does not run them; with xfstests_native=1 it does.
# They end their batch, and the next one starts with a new scratch file system.
SOLO="generic/740"

: >$RESULTS
cd $XF || exit 1
t0=$(date +%s)
done_n=0
while [ "$done_n" -lt "$TOTAL" ]; do
	# As many tests as could all reach the limit before the run's own time is
	# up (at most BATCH): the guest's timeout then never cuts a batch off.
	left=$((BUDGET - ($(date +%s) - t0)))
	n=$((left / LIMIT))
	[ "$n" -gt "$BATCH" ] && n=$BATCH
	if [ "$n" -lt 1 ]; then
		sed -n "$((done_n + 1)),\$p" $SHARD_LIST | sed 's|^|xfstests: not started (the time of the run is spent): |'
		break
	fi
	set -- $(sed -n "$((done_n + 1)),$((done_n + n))p" $SHARD_LIST)
	cut=0
	i=0
	for id in "$@"; do
		i=$((i + 1))
		case " $SOLO " in
		*" $id "*)
			cut=$i
			break
			;;
		esac
	done
	[ "$cut" -gt 0 ] && set -- $(echo "$@" | cut -d' ' -f1-"$cut")
	done_n=$((done_n + $#))
	run_batch "$@"
	retry_missing
done
t1=$(date +%s)

passed=$(grep -c ' pass ' $RESULTS)
failed=$(grep -c ' fail ' $RESULTS)
notrun=$(grep -c ' notrun ' $RESULTS)
timedout=$(grep -c ' timeout ' $RESULTS)
echo "xfstests.sh: SUMMARY fstype=$FSTYPE native=${NATIVE:-0} tests=$TOTAL pass=$passed fail=$failed notrun=$notrun timeout=$timedout seconds=$((t1 - t0))"
awk '{ print $3, $1 }' $RESULTS | sort -rn | head -n 8 | sed 's/^/xfstests: slowest: /'

if [ "$GATE" = 1 ]; then
	xfstests_gate $RESULTS "$EXPECTED" $SHARD_LIST "$NOTRUN"
	# Nothing here touches the backing file systems except through dcfs, so
	# dcfs (backing.cc's out-of-band detection) must have noticed no change
	# it did not make itself. That check reads what syslogd collected, so a
	# syslogd that collected nothing would pass it: a marker through the same
	# path first. syslogd handles it asynchronously and announces nothing, so
	# each of up to 20 tries sends the marker, then follows the log for a
	# second (`logread -f`, which returns as soon as the marker shows) and
	# reads it once more.
	sm_ok=0
	sm_tries=0
	while [ "$sm_tries" -lt 20 ]; do
		sm_tries=$((sm_tries + 1))
		logger -p daemon.warning xfstests-syslog-marker
		if timeout 1 logread -f | grep -q -m 1 xfstests-syslog-marker || logread | grep -q xfstests-syslog-marker; then
			sm_ok=1
			break
		fi
	done
	if [ "$sm_ok" = 1 ]; then
		pass xfstests-syslog
	else
		fail xfstests-syslog "syslogd did not log a marker line in 20 tries: the out-of-band check below would see nothing"
	fi
	oob=$(logread 2>/dev/null | grep -c 'out-of-band')
	echo "xfstests.sh: out-of-band warnings in syslog: ${oob:-0}"
	if [ "${oob:-0}" -gt 0 ]; then
		logread | grep 'out-of-band' | head -n 5
		fail xfstests-no-out-of-band "$oob warning(s): xfstests only reaches the backing file system through dcfs"
	else
		pass xfstests-no-out-of-band
	fi
fi
if [ "$FAILED" -ne 0 ]; then
	# What dcfs said: its warnings and errors go to syslog.
	echo "xfstests.sh: the last lines dcfs logged:"
	logread 2>/dev/null | grep -i 'dcfs' | tail -n 40 | sed 's/^/xfstests:   syslog: /'
fi
exit "$FAILED"
