#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Build a MINIMAL dcfs QEMU test kernel out-of-tree from $LINUX (default
# $HOME/Sources/linux) into $DCFS_KERNEL_BUILD (default
# $HOME/.cache/dcfs/kernel-build). Unlike fuse-generation-qemu's kernel,
# this one is not patched: it just needs a narrow set of filesystems and
# virtio drivers for dcfs's own QEMU tests (ext4, btrfs, xfs, NFSv4
# export, tmpfs, FUSE with FUSE_IO_URING, ...), and nothing else -- every
# unit test now boots this kernel, so its boot time is on the critical
# path of `bazel test //...` and it is trimmed accordingly (see
# test/qemu/README.md for the boot-time budget and how this config was
# chosen). PVH direct boot (CONFIG_PVH) lets QEMU's microvm machine start
# the kernel with no firmware at all; virtio-mmio (not virtio-pci) is the
# only bus, since microvm has no PCI.
set -eu

LINUX="${LINUX:-$HOME/Sources/linux}"
OUT="${DCFS_KERNEL_BUILD:-$HOME/.cache/dcfs/kernel-build}"
JOBS="${JOBS:-3}"

if [ -e "$LINUX/.config" ]; then
	echo "error: $LINUX has an in-tree .config; run 'make mrproper' there first" >&2
	exit 1
fi

mkdir -p "$OUT"
LOG="$OUT/../kernel-build.log"

before_yes=0
if [ -f "$OUT/.config" ]; then
	before_yes=$(grep -c '=y' "$OUT/.config" || true)
fi

{
	cd "$LINUX"

	# Note: these must be separate invocations; passing several config
	# targets to a single O= make trips kbuild's "source tree is not
	# clean" check. This script is idempotent: it always rebuilds the
	# .config from a fresh defconfig+kvm_guest baseline rather than
	# patching the previous one, so re-running it after editing this
	# script's -e/-d list is safe.
	make O="$OUT" x86_64_defconfig
	make O="$OUT" kvm_guest.config

	# Enable exactly what the dcfs guest needs: the filesystems under
	# test, FUSE (+ io_uring passthrough), NFSv4 server+client (for the
	# NFS-export scenarios), virtio-mmio (microvm has no PCI), PVH
	# (firmware-less direct kernel boot on microvm), and the scratch-disk
	# plumbing (loop, tmpfs, crc32c/zlib/lzo/zstd for btrfs). Then disable
	# every bulky subsystem the guest never touches, so boot -- now on
	# the critical path of every `bazel test` -- has as little to probe
	# as possible.
	#
	# NFSD_LEGACY_CLIENT_TRACKING (step 5.3): without this, nfsd has no
	# way to persist NFSv4 client-recovery state itself and upcalls to a
	# userland tracking daemon instead (nfsdcld, or a legacy usermode-
	# helper/rpc.mountd upcall). On this guest -- diagnosed while
	# building test/qemu:nfs_test -- that upcall path is what's actually
	# used regardless of whether nfsdcld is running (see "NFSD: Unable
	# to initialize client recovery tracking! (-110)" in dmesg), and
	# rpc.mountd's own handling of that specific legacy upcall segfaults
	# reproducibly on the very first client's SETCLIENTID (observed as
	# "rpc.mountd[N]: segfault at 0 ... in libc.so.6", right after it
	# logs "vX.Y client attached"), hanging the client's mount(2) call
	# forever waiting for a reply that will now never come.
	# LEGACY_CLIENT_TRACKING makes nfsd manage /var/lib/nfs/v4recovery
	# itself, entirely in-kernel, with no upcall and thus nothing for
	# rpc.mountd to crash handling.
	"$LINUX/scripts/config" --file "$OUT/.config" \
		-e 64BIT \
		-e SMP \
		-e PARAVIRT \
		-e KVM_GUEST \
		-e HIGH_RES_TIMERS \
		-e PVH \
		-e VIRTIO_MMIO \
		-e VIRTIO_MMIO_CMDLINE_DEVICES \
		-e VIRTIO_BLK \
		-e VIRTIO_NET \
		-e VIRTIO_CONSOLE \
		-e SERIAL_8250 \
		-e SERIAL_8250_CONSOLE \
		-e BLK_DEV_INITRD \
		-e RD_GZIP \
		-e DEVTMPFS \
		-e DEVTMPFS_MOUNT \
		-e TMPFS \
		-e TMPFS_XATTR \
		-e TMPFS_POSIX_ACL \
		-e PROC_FS \
		-e SYSFS \
		-e FHANDLE \
		-e EXPORTFS \
		-e EXT4_FS \
		-e EXT4_FS_POSIX_ACL \
		-e EXT4_FS_SECURITY \
		-e BTRFS_FS \
		-e BTRFS_FS_POSIX_ACL \
		-e XFS_FS \
		-e XFS_POSIX_ACL \
		-e FUSE_FS \
		-e FUSE_IO_URING \
		-e IO_URING \
		-e NFSD \
		-e NFSD_V4 \
		-e NFSD_LEGACY_CLIENT_TRACKING \
		-e NFS_FS \
		-e NFS_V4 \
		-e SUNRPC \
		-e UNIX \
		-e INET \
		-e CRYPTO_CRC32C \
		-e LIBCRC32C \
		-e ZLIB_INFLATE \
		-e ZLIB_DEFLATE \
		-e LZO_COMPRESS \
		-e LZO_DECOMPRESS \
		-e ZSTD_COMPRESS \
		-e ZSTD_DECOMPRESS \
		-e BLK_DEV_LOOP \
		-e EVENTFD \
		-e EPOLL \
		-e SIGNALFD \
		-e TIMERFD \
		-e INOTIFY_USER \
		-e SHMEM \
		-e FILE_LOCKING \
		-d VIRTIO_PCI \
		-d 9P_FS \
		-d DRM \
		-d FB \
		-d VGA_CONSOLE \
		-d FRAMEBUFFER_CONSOLE \
		-d SOUND \
		-d USB \
		-d INPUT \
		-d HID \
		-d I2C \
		-d THERMAL \
		-d WATCHDOG \
		-d BT \
		-d NFC \
		-d WLAN \
		-d CFG80211 \
		-d MEDIA_SUPPORT \
		-d PCMCIA \
		-d ATA \
		-d SCSI \
		-d ETHERNET \
		-d NETFILTER \
		-d IPV6 \
		-d HPET \
		-d HPET_TIMER \
		-d ACPI \
		-d MODULES \
		-d IKCONFIG \
		-d IKCONFIG_PROC \
		-d KEXEC \
		-d KEXEC_FILE \
		-d HIBERNATION \
		-d CPU_FREQ \
		-d CPU_IDLE \
		-d XEN \
		-d HYPERV \
		-d VMWARE_BALLOON \
		-d VMWARE_VMCI \
		-d POSIX_MQUEUE \
		-d DEBUG_INFO

	make O="$OUT" olddefconfig
	nice -n 19 make O="$OUT" -j"$JOBS" bzImage

	echo "Kernel: $OUT/arch/x86/boot/bzImage"
} >|"$LOG" 2>&1

after_yes=$(grep -c '=y' "$OUT/.config" || true)
size=$(stat -c '%s' "$OUT/arch/x86/boot/bzImage" 2>/dev/null || echo unknown)
{
	echo "enabled config symbols before: $before_yes, after: $after_yes"
	echo "bzImage size: $size bytes"
} | tee -a "$LOG"
