#!/bin/sh
# dcfs step 23.7: the reproducer of a kernel bug, kept as a DISABLED_ check.
#
# EXT4_IOC_SET_TUNE_SB_PARAM (Linux 6.18+) can switch the casefold feature on
# under a mounted ext4 filesystem, but it never loads the filesystem's
# in-memory encoding (sb->s_encoding is set only at mount), while chattr +F
# checks only the feature bit. The next readdir of the casefolded directory
# then dereferences NULL in utf8byte (ext4fs_dirhash <- ext4_readdir). The
# kernel should refuse the ioctl, or load the encoding.
#
# Kernel only: dcfs is not involved, the raw mount is used. The check is the
# only one, the last, and runs under `disabled`: it would PASS when listing
# the directory works, or when the kernel refuses the ioctl or +F (EOPNOTSUPP
# or EINVAL: also a correct kernel), and would FAIL (kernel: <the oops>) as
# long as the kernel has the bug. run-qemu.sh is told to expect the oops
# (qemu_test's kernel_failure = "expected"); no other test may have one.
#
# Run as /tests/casefold_tune_oops.sh by guest/init when booted with
# dcfs_test=casefold_tune_oops.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

TESTUTIL=/bin/testutil

echo "casefold_tune_oops.sh: kernel $(uname -r)"

# casefold_tune_online_oops: succeeds if the kernel survived listing a
# directory made case-insensitive under an online-enabled casefold feature;
# on failure says why (the oops, first line of the kernel's report).
casefold_tune_online_oops() {
	mount /dev/vdb /src || {
		echo "mount failed"
		return 1
	}
	mkdir /src/d
	if ! out=$("$TESTUTIL" ext4-tune-casefold /src 2>&1); then
		case "$out" in
		"ERR EOPNOTSUPP" | "ERR EINVAL")
			echo "the kernel refuses the ioctl: $out"
			return 0
			;;
		esac
		echo "ioctl: $out"
		return 1
	fi
	flags=$("$TESTUTIL" getflags /src/d)
	if ! out=$("$TESTUTIL" setflags /src/d "$(printf '%x' $((0x$flags | 0x40000000)))" 2>&1); then
		case "$out" in
		"ERR EOPNOTSUPP" | "ERR EINVAL")
			echo "the kernel refuses chattr +F: $out"
			return 0
			;;
		esac
		echo "chattr +F: $out"
		return 1
	fi
	# The oops kills ls; the kernel goes on. Its report is in the kernel log.
	# (The group's redirection hides the shell's own "Killed" message.)
	if { ls /src/d >/dev/null; } 2>/dev/null; then
		echo "the listing works"
		return 0
	fi
	oops=$(dmesg | grep -m 1 -E 'BUG:|Oops' | sed 's/^\[[^]]*\] *//')
	echo "kernel: ${oops:-no oops in the kernel log (ls failed)}"
	return 1
}

disabled casefold-tune-online-oops \
	"kernel: EXT4_IOC_SET_TUNE_SB_PARAM enables the casefold feature without loading sb->s_encoding, so the next readdir of a +F directory dereferences NULL; test/qemu/README.md" \
	casefold_tune_online_oops

exit "$FAILED"
