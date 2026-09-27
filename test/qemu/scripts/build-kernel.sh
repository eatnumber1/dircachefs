#!/bin/sh
# Adapted from fuse-generation-qemu; that repo stays the LKML patch's test
# suite.
#
# Build a dcfs QEMU test kernel out-of-tree from $LINUX (default
# $HOME/Sources/linux) into $DCFS_KERNEL_BUILD (default
# $HOME/.cache/dcfs/kernel-build). Unlike fuse-generation-qemu's kernel,
# this one is not patched: it just needs a wide enough set of filesystems
# and virtio drivers for dcfs's own QEMU end-to-end tests (ext4, btrfs,
# xfs, NFSv4 export, tmpfs, FUSE with FUSE_IO_URING, ...).
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

{
	cd "$LINUX"

	# Note: these must be separate invocations; passing several config
	# targets to a single O= make trips kbuild's "source tree is not
	# clean" check.
	make O="$OUT" x86_64_defconfig
	make O="$OUT" kvm_guest.config

	# Enable what the dcfs guest needs on top of defconfig+kvm_guest:
	# the filesystems under test, FUSE (+ io_uring passthrough), NFSv4
	# server+client (for the NFS-export scenarios), virtio, and the
	# scratch-disk plumbing (loop, tmpfs, crc32c for btrfs).
	"$LINUX/scripts/config" --file "$OUT/.config" \
		-e EXT4_FS \
		-e EXT4_FS_POSIX_ACL \
		-e BTRFS_FS \
		-e BTRFS_FS_POSIX_ACL \
		-e XFS_FS \
		-e XFS_POSIX_ACL \
		-e FUSE_FS \
		-e FUSE_IO_URING \
		-e IO_URING \
		-e NFSD \
		-e NFSD_V4 \
		-e NFS_FS \
		-e NFS_V4 \
		-e NFS_V4_1 \
		-e SUNRPC \
		-e TMPFS \
		-e TMPFS_XATTR \
		-e VIRTIO_BLK \
		-e VIRTIO_NET \
		-e VIRTIO_PCI \
		-e 9P_FS \
		-e DEVTMPFS \
		-e DEVTMPFS_MOUNT \
		-e BLK_DEV_LOOP \
		-e CRYPTO_CRC32C \
		-e LIBCRC32C \
		-e BLK_DEV_INITRD \
		-e EXPORTFS \
		-d MODULES

	make O="$OUT" olddefconfig
	nice -n 19 make O="$OUT" -j"$JOBS" bzImage

	echo "Kernel: $OUT/arch/x86/boot/bzImage"
} >|"$LOG" 2>&1
