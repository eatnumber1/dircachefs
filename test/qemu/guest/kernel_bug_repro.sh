#!/bin/sh
# dcfs step 26.15: runs the standalone reproducer of a Linux kernel bug
# (tools/kernel_bugs/<name>/reproduce.sh, installed under
# /kernel_bugs/<name>/ by scripts/mkinitramfs.sh) as a DISABLED_ check, the
# way guest/casefold_tune_oops.sh and guest/fault_recover.sh keep theirs. The
# bug to run is the `dcfs_kernel_bug=<name>` word of the kernel command line
# (qemu_test's cmdline); the disk is /dev/vdb, made by the harness (reproduce.sh
# runs with MKFS=0).
#
# The reproducer exits 0 when the bug does not show and 1 when the kernel
# logged the failure (it then prints "kernel: <first line>" last), so the
# check would PASS on a fixed kernel and would FAIL (kernel: ...) on one with
# the bug; run-qemu.sh tolerates the kernel failure only then (qemu_test's
# kernel_failure = "expected"). Nothing of dcfs is involved: this is the raw
# kernel, and the reproducer is the same script a bug report points to.
#
# Run as /tests/kernel_bug_repro.sh by guest/init when booted with
# dcfs_test=kernel_bug_repro.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

BUG=$(sed -n 's/.*dcfs_kernel_bug=\([^ ]*\).*/\1/p' /proc/cmdline)
DISK=/dev/vdb
OUT=/tmp/kernel_bug.out

echo "kernel_bug_repro.sh: kernel $(uname -r), bug '$BUG'"

# run_repro NAME: runs /kernel_bugs/NAME/reproduce.sh over the disk, its output
# on this script's own output (the serial log), and records the exit status in
# REPRO_RC and what to say about a failure in REPRO_DETAIL ("kernel: <first
# line of the kernel's report>" if the kernel logged one).
run_repro() {
	MKFS=0 DMSETUP=/sbin/dmsetup "/kernel_bugs/$1/reproduce.sh" "$DISK" >"$OUT" 2>&1
	REPRO_RC=$?
	cat "$OUT"
	if grep -q '^kernel: ' "$OUT"; then
		REPRO_DETAIL=$(grep -m 1 '^kernel: ' "$OUT")
	else
		REPRO_DETAIL="reproduce.sh exited $REPRO_RC: $(tail -n 1 "$OUT")"
	fi
}

# repro_verdict: the check `disabled` runs: succeeds if the bug did not show.
repro_verdict() {
	[ "$REPRO_RC" -eq 0 ] && return 0
	echo "$REPRO_DETAIL"
	return 1
}

case "$BUG" in
ext4_casefold_tune)
	run_repro ext4_casefold_tune
	disabled casefold-tune-online-oops \
		"kernel: EXT4_IOC_SET_TUNE_SB_PARAM enables the casefold feature without loading sb->s_encoding, so the next readdir of a +F directory dereferences NULL; tools/kernel_bugs/ext4_casefold_tune/README.md" \
		repro_verdict
	;;
btrfs_failed_inode_read)
	run_repro btrfs_failed_inode_read
	disabled BTRFS_FAILED_INODE_READ_WARNS \
		"kernel: btrfs_destroy_inode warns when open_by_handle_at fails to read a cold inode; tools/kernel_bugs/btrfs_failed_inode_read/README.md" \
		repro_verdict
	;;
*)
	fail kernel-bug-named "no dcfs_kernel_bug=<name> on the kernel command line (got '$BUG')"
	;;
esac

exit "$FAILED"
