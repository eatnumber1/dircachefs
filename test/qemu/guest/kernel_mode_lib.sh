# The two kernel modes of a guest (steps 26.14 and 26.14e), as one check that
# guest/quiet_kernel.sh (the quiet default) and guest/noisy_kernel.sh (the
# noisy run) share, and that test/qemu/kernel_mode_test.sh runs over canned
# /proc trees so that each direction is known to fail where it should.
#
#   quiet  guest/init set the sysctls that take the kernel's spontaneous
#          activity away and the guest has one vCPU (qemu_test `cpus`, the
#          default);
#   noisy  DCFS_NOISY=1 reached run-qemu.sh (bazel test --test_env), which put
#          dcfs_noisy=1 on the kernel command line and gave the guest two
#          vCPUs; guest/init left the sysctls at the kernel's defaults.
#
# kernel_mode_problems MODE [ROOT]: what is not as MODE says, one line each
# (nothing: the guest is in that mode). ROOT is "" for the running kernel's
# /proc, or a directory with proc/sys/vm/*, proc/cpuinfo and proc/cmdline.

# kernel_mode_sysctl ROOT NAME: the value of vm.NAME.
kernel_mode_sysctl() {
	cat "$1/proc/sys/vm/$2" 2>&1
}

# kernel_mode_want ROOT NAME WANT: a line unless vm.NAME reads WANT.
kernel_mode_want() {
	kmw_got=$(kernel_mode_sysctl "$1" "$2")
	[ "$kmw_got" = "$3" ] || echo "vm.$2 reads '$kmw_got', want '$3'"
}

kernel_mode_problems() {
	kmp_mode=$1
	kmp_root=${2:-}
	kmp_cmdline=$(cat "$kmp_root/proc/cmdline" 2>&1)
	kmp_cpus=$(grep -c '^processor' "$kmp_root/proc/cpuinfo")
	case "$kmp_mode" in
	quiet)
		# guest/init's values: no periodic writeback, nothing old enough to
		# write for a day, laptop mode off, drop_caches still effective.
		kernel_mode_want "$kmp_root" dirty_writeback_centisecs 0
		kernel_mode_want "$kmp_root" dirty_expire_centisecs 8640000
		kernel_mode_want "$kmp_root" laptop_mode 0
		kernel_mode_want "$kmp_root" vfs_cache_pressure 100
		[ "$kmp_cpus" = 1 ] || echo "the guest has $kmp_cpus vCPUs, want 1 (qemu_test's cpus default)"
		case " $kmp_cmdline " in
		*" dcfs_noisy=1 "*) echo "the kernel command line has dcfs_noisy=1: the guest is noisy" ;;
		esac
		;;
	noisy)
		# The kernel's own defaults: the flushers wake every 5 s and write
		# pages 30 s old.
		kernel_mode_want "$kmp_root" dirty_writeback_centisecs 500
		kernel_mode_want "$kmp_root" dirty_expire_centisecs 3000
		kernel_mode_want "$kmp_root" laptop_mode 0
		kernel_mode_want "$kmp_root" vfs_cache_pressure 100
		[ "$kmp_cpus" = 2 ] || echo "the guest has $kmp_cpus vCPUs, want 2 (run-qemu.sh under DCFS_NOISY=1)"
		case " $kmp_cmdline " in
		*" dcfs_noisy=1 "*) ;;
		*) echo "the kernel command line lacks dcfs_noisy=1: DCFS_NOISY=1 did not reach run-qemu.sh" ;;
		esac
		;;
	*)
		echo "kernel_mode_problems: unknown mode '$kmp_mode'"
		;;
	esac
}
