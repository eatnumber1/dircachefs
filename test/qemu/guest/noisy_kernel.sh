#!/bin/sh
# Step 26.14e: the noisy run took effect. The inverse of guest/quiet_kernel.sh:
# under DCFS_NOISY=1 (bazel test --test_env) run-qemu.sh puts dcfs_noisy=1 on
# the kernel command line and gives the guest two vCPUs, and guest/init leaves
# the quiet-kernel sysctls at the kernel's defaults. This reads all of it back
# (guest/kernel_mode_lib.sh; test/qemu/kernel_mode_test.sh runs the same check
# over canned trees). (The flusher's own writeback is not waited for: it has no
# event to wait on, and the sysctls it follows are read back.)
#
# It FAILS without the knob, which is the point: a noisy job in which this
# target passes is a noisy job whose guests were noisy. The target is tagged
# manual (the push gate runs the quiet default and must not see it); the noisy
# job names it (.github/workflows/ci.yml, job noisy).
#
# Run as /tests/noisy_kernel.sh by guest/init (qemu_test noisy_kernel_test).
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/kernel_mode_lib.sh"

mode_problems=$(kernel_mode_problems noisy)
if [ -z "$mode_problems" ]; then
	pass kernel-mode-noisy
else
	fail kernel-mode-noisy "$mode_problems (is DCFS_NOISY=1 set for this test: bazel test --test_env=DCFS_NOISY=1?)"
fi

exit "$FAILED"
