#!/bin/sh
# dcfs step 17.1 acceptance test: xfstests' generic tests against dcfs.
#
# Runs in the xfstests root file system (test/qemu/scripts/mkxfstests_rootfs.py:
# bash, GNU coreutils, util-linux's mount, xfs_io, perl and the rest of what
# `check` and the tests call, xfstests under /xfstests), which guest/init
# chroots into; this script is /tests/xfstests.sh there. vdb and vdc are the
# backing devices of xfstests' TEST_DEV and SCRATCH_DEV; xfstests mounts them
# with `mount -t fuse.dcfs` (FSTYP=fuse, FUSE_SUBTYP=.dcfs), which runs
# mount.dcfs, whose native mount of the device picks up ext4, xfs or btrfs
# (the test's matrix variant). xfstests' own checks decide what does not apply
# to a FUSE file system and report it "not run".
#
# The tests of the generic group "auto" less the exclusions (guest
# xfstests.excluded: tests that cannot apply or cannot run in this guest, each
# with its reason) are divided into shards by position (xfstests_<n>.sh set
# XFSTESTS_SHARD and XFSTESTS_SHARDS and source this file), and each is run by
# its own `check` under a time limit. A test whose result is not the listed
# one fails the run (xfstests_lib.sh: xfstests_gate): a failure is listed, with
# its reason, in xfstests.<fstype>.expected_failures, and a listed test that
# passes is a failure too, so the list stays honest.
#
# Kernel command line, for working on the suite (run-qemu.sh --cmdline):
#   xfstests_run=generic/001,generic/002   run just these tests, no gate
#   xfstests_native=1                      run them on the native file system
#                                          (no dcfs), to see what the backing
#                                          answers; no gate
#   xfstests_timeout=<seconds>             the per-test limit
#   xfstests_budget=<seconds>              start no test after this long
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/xfstests_lib.sh"

XF=/xfstests
LISTS=$XF/dcfs
GROUP_LIST=$XF/tests/generic/group.list
RESULTS=/tmp/xfstests_results.txt
SHARD_LIST=/tmp/xfstests_shard.txt
TEST_DEV=/dev/vdb
SCRATCH_DEV=/dev/vdc
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
LIMIT=$(cmdline_word xfstests_timeout)
LIMIT=${LIMIT:-200}
# No new test is started after this many seconds of the run, so that the
# guest's own timeout (the target's) never cuts a run off before its summary.
BUDGET=$(cmdline_word xfstests_budget)
BUDGET=${BUDGET:-3000}
DEBUGCHECK=$(cmdline_word xfstests_debug)

cleanup() {
	rc=$?
	umount "$SCRATCH_MNT" 2>/dev/null || true
	umount "$TEST_DIR" 2>/dev/null || true
	return "$rc"
}
trap cleanup EXIT

echo "xfstests.sh: kernel $(uname -r)"
require_commands bash perl xfs_io mount findmnt timeout

FSTYPE=$(blkid "$TEST_DEV" 2>/dev/null | sed -n 's/.*TYPE="\([^"]*\)".*/\1/p')
echo "xfstests.sh: backing file system: ${FSTYPE:-unknown}"
case "$FSTYPE" in
ext4 | xfs | btrfs) ;;
*)
	fail xfstests-backing "TEST_DEV $TEST_DEV is '$FSTYPE', not ext4, xfs or btrfs"
	exit "$FAILED"
	;;
esac
[ "$(blkid "$SCRATCH_DEV" 2>/dev/null | sed -n 's/.*TYPE="\([^"]*\)".*/\1/p')" = "$FSTYPE" ] ||
	fail xfstests-backing "SCRATCH_DEV $SCRATCH_DEV is not $FSTYPE"

# A new, empty file system on the scratch device (and a new cache for it)
# before each test, as xfstests makes one with its mkfs: for a FUSE file
# system xfstests only deletes the files, which would leave the previous test's
# directories, attributes and hidden files behind.
mkfs_dev() {
	case "$FSTYPE" in
	ext4) mkfs.ext4 -F -q "$1" ;;
	xfs) mkfs.xfs -f -q "$1" ;;
	btrfs) mkfs.btrfs -f -q "$1" ;;
	esac >/dev/null 2>&1
}
reset_scratch() {
	umount "$SCRATCH_MNT" 2>/dev/null
	rm -f /cache/vdc.db /cache/vdc.db-wal /cache/vdc.db-shm
	mkfs_dev "$SCRATCH_DEV"
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
syslogd -C256 2>/dev/null
cat >$XF/local.config <<EOC
export TEST_DEV=$TEST_DEV
export TEST_DIR=$TEST_DIR
export SCRATCH_DEV=$SCRATCH_DEV
export SCRATCH_MNT=$SCRATCH_MNT
EOC
if [ "$NATIVE" = 1 ]; then
	echo "export FSTYP=$FSTYPE" >>$XF/local.config
	# The native file systems are made by xfstests (mkfs of $FSTYP).
	mkfs_dev "$TEST_DEV"
else
	cat >>$XF/local.config <<EOC
export FSTYP=fuse
export FUSE_SUBTYP=.dcfs
EOC
fi

# --- the tests of this shard ---------------------------------------------------

if ! xfstests_lists_valid "$LISTS/xfstests.$FSTYPE.expected_failures" "$LISTS/xfstests.excluded" "$GROUP_LIST"; then
	exit "$FAILED"
fi
if [ -n "$RUN_ONLY" ]; then
	echo $RUN_ONLY | tr ' ' '\n' >$SHARD_LIST
	GATE=0
else
	xfstests_select "$GROUP_LIST" auto "$LISTS/xfstests.excluded" |
		xfstests_shard "${XFSTESTS_SHARD:?run a xfstests_<n>.sh wrapper}" "${XFSTESTS_SHARDS:?}" >$SHARD_LIST
	GATE=1
fi
[ "$NATIVE" = 1 ] && GATE=0
TOTAL=$(wc -l <$SHARD_LIST)
echo "xfstests.sh: shard ${XFSTESTS_SHARD:-}/${XFSTESTS_SHARDS:-}: $TOTAL tests, limit ${LIMIT}s each"

# --- run them --------------------------------------------------------------------

# `check` costs several seconds of CPU in this guest before its first test
# (it sources 6000 lines of shell and forks a few hundred times), as much as
# most "not run" tests take, so it is given BATCH tests at a time. It has no
# time limit of its own: this script watches its output, and the test that
# has been running for LIMIT seconds is killed (check then goes on to the next
# one and reports it as failed; the result here is "timeout").
BATCH=$(cmdline_word xfstests_batch)
BATCH=${BATCH:-10}

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

# report_failure ID STATUS: enough of the evidence of a failed test to triage
# from the serial log.
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

# run_batch ID...: runs the tests with one check, appends "ID status seconds"
# to $RESULTS for each that has a result, and writes the ones that have none
# (check was interrupted before them) to /tmp/missing.txt.
run_batch() {
	for rb_id in "$@"; do rm -f "results/generic/${rb_id#generic/}".*; done
	reset_scratch
	: >/tmp/durations
	: >/tmp/killed
	: >/tmp/missing.txt
	bash ./check "$@" >/tmp/check.out 2>&1 </dev/null &
	rb_pid=$!
	rb_cur=""
	rb_since=$(date +%s)
	rb_kill_at=0
	while kill -0 "$rb_pid" 2>/dev/null; do
		sleep 2
		rb_now=$(date +%s)
		rb_last=$(grep -o '^generic/[0-9]*' /tmp/check.out | tail -n 1)
		if [ "$rb_last" != "$rb_cur" ]; then
			[ -n "$rb_cur" ] && echo "$rb_cur $((rb_now - rb_since))" >>/tmp/durations
			rb_cur=$rb_last
			rb_since=$rb_now
		elif [ -n "$rb_cur" ] && [ $((rb_now - rb_since)) -ge "$LIMIT" ] && [ "$rb_now" -ge "$rb_kill_at" ]; then
			# (Again after 20 s if a killed test's children kept it going.)
			echo "$rb_cur" >>/tmp/killed
			stall_report "$rb_cur"
			kill_test "${rb_cur#generic/}"
			rb_kill_at=$((rb_now + 20))
		fi
	done
	wait "$rb_pid"
	rb_now=$(date +%s)
	[ -n "$rb_cur" ] && echo "$rb_cur $((rb_now - rb_since))" >>/tmp/durations
	umount -l "$SCRATCH_MNT" 2>/dev/null
	rb_failures=$(sed -n 's/^Failures: *//p' /tmp/check.out | tail -n 1)
	rb_ran=$(sed -n 's/^Ran: *//p' /tmp/check.out | tail -n 1)
	for rb_id in "$@"; do
		rb_seq=${rb_id#generic/}
		rb_secs=$(awk -v id="$rb_id" '$1 == id { print $2 }' /tmp/durations | tail -n 1)
		if grep -qx "$rb_id" /tmp/killed; then
			rb_status=timeout
		elif [ -f "results/generic/$rb_seq.notrun" ]; then
			rb_status=notrun
		elif case " $rb_failures " in *" $rb_id "*) true ;; *) false ;; esac; then
			rb_status=fail
		elif case " $rb_ran " in *" $rb_id "*) true ;; *) false ;; esac; then
			rb_status=pass
		else
			echo "$rb_id" >>/tmp/missing.txt
			continue
		fi
		echo "$rb_id $rb_status ${rb_secs:-0}" >>$RESULTS
		if [ "$rb_status" = notrun ]; then
			# Why, for the reasons in the report (and for what the runtime lacks).
			echo "xfstests: $rb_id notrun ${rb_secs:-0}s: $(head -n 1 "results/generic/$rb_seq.notrun" | cut -c1-110)"
		else
			echo "xfstests: $rb_id $rb_status ${rb_secs:-0}s"
		fi
		if [ "$rb_status" = fail ] || [ "$rb_status" = timeout ]; then
			report_failure "$rb_id"
		fi
	done
}

: >$RESULTS
cd $XF || exit 1
t0=$(date +%s)
set --
while IFS= read -r id; do
	if [ $(($(date +%s) - t0)) -ge "$BUDGET" ]; then
		echo "xfstests: $id not started: the ${BUDGET}s budget of the run is spent"
		continue
	fi
	set -- "$@" "$id"
	if [ "$#" -ge "$BATCH" ]; then
		run_batch "$@"
		cp /tmp/missing.txt /tmp/retry.txt
		set --
		# Tests check was interrupted before: each alone.
		while IFS= read -r id2; do
			run_batch "$id2"
			if [ -s /tmp/missing.txt ]; then
				echo "$id2 fail 0" >>$RESULTS
				echo "xfstests: $id2 fail (check ran nothing; its output follows)"
				sed -n '1,30p' /tmp/check.out | sed 's/^/xfstests:   | /'
			fi
		done </tmp/retry.txt
	fi
done <$SHARD_LIST
if [ "$#" -gt 0 ]; then
	run_batch "$@"
	cp /tmp/missing.txt /tmp/retry.txt
	while IFS= read -r id2; do
		run_batch "$id2"
		if [ -s /tmp/missing.txt ]; then
			echo "$id2 fail 0" >>$RESULTS
			echo "xfstests: $id2 fail (check ran nothing; its output follows)"
			sed -n '1,30p' /tmp/check.out | sed 's/^/xfstests:   | /'
		fi
	done </tmp/retry.txt
fi
t1=$(date +%s)

passed=$(grep -c ' pass ' $RESULTS)
failed=$(grep -c ' fail ' $RESULTS)
notrun=$(grep -c ' notrun ' $RESULTS)
timedout=$(grep -c ' timeout ' $RESULTS)
echo "xfstests.sh: SUMMARY fstype=$FSTYPE native=${NATIVE:-0} tests=$TOTAL pass=$passed fail=$failed notrun=$notrun timeout=$timedout seconds=$((t1 - t0))"
awk '{ print $3, $1 }' $RESULTS | sort -rn | head -n 8 | sed 's/^/xfstests: slowest: /'

if [ "$GATE" = 1 ]; then
	xfstests_gate $RESULTS "$LISTS/xfstests.$FSTYPE.expected_failures" $SHARD_LIST
	# Nothing here touches the backing file systems except through dcfs, so
	# dcfs (backing.cc's out-of-band detection) must have noticed no change
	# it did not make itself.
	oob=$(logread 2>/dev/null | grep -c 'out-of-band')
	echo "xfstests.sh: out-of-band warnings in syslog: ${oob:-0}"
	if [ "${oob:-0}" -gt 0 ]; then
		logread | grep 'out-of-band' | head -n 5
		fail xfstests-no-out-of-band "$oob warning(s): xfstests only reaches the backing file system through dcfs"
	else
		pass xfstests-no-out-of-band
	fi
fi
exit "$FAILED"
