#!/bin/sh
# Assemble the dcfs QEMU NFS test's Debian rootfs image from the flattened
# package tree rules_distroless's apt extension resolved and fetched (see
# third_party/debian/README.md and ../BUILD.bazel's :rootfs genrule).
# Replaces test/qemu/scripts/mkrootfs-debian.sh's mmdebstrap run: every
# package comes from a pinned snapshot.debian.org timestamp (checked into
# MODULE.bazel) instead of a live mirror, and assembly is a Bazel action
# instead of a step run once by hand into ~/.cache/dcfs.
#
# Invoked as: mkrootfs.sh <out.ext4> <flat.tar> <size>
#
#   <flat.tar>: @debian//:flat -- every resolved package's files merged
#   into one tar by rules_distroless's own `flatten` rule (bsdtar,
#   deduplicate=True for directory entries). Ownership/permissions in this
#   tar are exactly what each .deb shipped (e.g. root:root on most files),
#   taken directly from the package's data.tar metadata -- no chown(2) was
#   involved in producing it, so no privilege was needed up to this point.
#
#   <size>: mke2fs -d's image size (truncate(1) syntax, e.g. 768M).
#
# Ownership: extracted with `tar --no-same-owner`, so every file in the
# resulting image ends up owned by whatever uid/gid ran this Bazel action,
# not the root:root (or other system-user) ownership the .deb payloads
# themselves specify. This is deliberate, not a shortcut taken for
# expedience alone:
#
#   - A real chown(2) to an arbitrary uid/gid needs a privilege this
#     action does not have and must not be given (the plan's "no root"
#     constraint), and the natural workaround -- fakeroot's LD_PRELOAD,
#     which only *pretends* chown() succeeded -- does not work inside a
#     Bazel sandboxed action: the sandbox already runs the action inside
#     its own single-entry user namespace (confirmed via
#     /proc/self/uid_map: "<uid> <uid> 1", i.e. only the invoking uid
#     itself is mapped, not even 0). fakeroot tries the real chown(2)
#     first and only fakes success on the specific failure a genuine
#     non-root process gets (EPERM); inside this namespace the kernel
#     instead rejects the call with EINVAL (the target uid/gid has no
#     mapping at all), which fakeroot does not treat as the expected case
#     and passes straight through as a real, fatal tar error.
#   - It does not matter for what this image is used for. Every test that
#     uses it (see test/qemu/guest/nfs.sh) chroots into it and runs
#     entirely as root (the guest kernel's PID 1 is already root, and
#     nothing here ever su's to or logs in as another user) -- root's own
#     DAC bypass makes a file's nominal owner irrelevant to whether root
#     can read, write or execute it. The one case where owner identity
#     usually matters on its own -- a setuid/setgid binary -- only matters
#     for privilege *escalation* from a non-root caller, which does not
#     happen here either. mkrootfs-debian.sh's old mmdebstrap-based image
#     went through real unprivileged-namespace uid mapping specifically to
#     preserve this ownership; that effort has no payoff for how this
#     image is actually used, so it is not reproduced.
#
# mke2fs (e2fsprogs) is a host tool, not yet built by Bazel (see
# README.md's "Host tools" section): -d <dir> builds the ext4 image
# directly from a real directory's own lstat() data (regular/dir/symlink
# mode, uid, gid, mtime) with no mount, loop device or root needed -- a
# plain userspace libext2fs operation, not a VFS one.
#
# Nothing here touches the network, and nothing is mounted (real or loop):
# both of the plan's "no root, no network, no loop mounts" constraints hold
# throughout.
set -eu

# Bazel genrules run with a minimal PATH that lacks the sbin directories
# mke2fs lives in (same fix as test/qemu/scripts/run-qemu.sh's).
export PATH="$PATH:/usr/sbin:/sbin"

OUT=$1
FLAT_TAR=$2
SIZE=$3

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ROOTDIR="$WORK/root"
mkdir -p "$ROOTDIR"
IMG_TMP="$WORK/rootfs.ext4"
truncate -s "$SIZE" "$IMG_TMP"

tar --no-same-owner -C "$ROOTDIR" -xf "$FLAT_TAR"

# See test/qemu/scripts/mkrootfs-debian.sh's comment (kept for history) for
# why this one file matters: rpc.mountd's crossmnt handling does
# openat("/etc/mtab", O_RDONLY|O_CLOEXEC) unconditionally and mishandles
# ENOENT (a reproducible NULL-deref SIGSEGV) when it's missing. A stock
# Debian install gets this symlink for free from the mount package's
# postinst; rules_distroless never runs any package's maintainer scripts
# (deb_import only ever unpacks data.tar -- no dpkg database, no postinst,
# matching mmdebstrap's own --variant=apt, which also skipped it), so it is
# recreated here by hand, exactly as the old script did.
ln -sf /proc/self/mounts "$ROOTDIR/etc/mtab"

# /bin/sh: this package set resolves to no shell at all unless `bash` is
# explicitly requested (see README.md's "Packages" section -- rules_distroless
# only pulls in what the named packages' own Depends/Pre-Depends reach, and
# none of them need a shell at runtime), and even with `bash` present,
# nothing sets up the conventional /bin/sh symlink: a stock Debian install
# gets it from dash's postinst (`update-alternatives --install /bin/sh sh
# ...`), which, like every other postinst, never runs here. guest/init's
# dcfs_rootfs= branch does `chroot /newroot /bin/sh "/tests/$TEST"`, so this
# is not optional.
ln -sf /bin/bash "$ROOTDIR/bin/sh"

# /usr/local/bin: guest/init's dcfs_rootfs= branch copies dcfs/fhtest/
# testutil here before chrooting in. A complete Debian install gets this
# (and the rest of the /usr/local hierarchy) from base-files, which nothing
# in this package set depends on, directly or transitively.
mkdir -p "$ROOTDIR/usr/local/bin"

# /usr/bin/awk: same update-alternatives gap as /bin/sh above. `mawk`
# (Debian's default awk provider) ships only /usr/bin/mawk; the generic
# /usr/bin/awk symlink is normally `update-alternatives --install
# /usr/bin/awk awk /usr/bin/mawk ...` in mawk's own postinst.
ln -sf mawk "$ROOTDIR/usr/bin/awk"

# /etc/exports: nfs-kernel-server's data.tar does not ship this file at
# all (confirmed: `tar -tvf` on its content.tar.gz has no etc/exports) --
# a stock install gets an empty one from the package's postinst. exportfs(8)
# refuses outright ("can't open /etc/exports for reading") if it is
# missing entirely; guest/nfs.sh's own `exportfs -o ...` call adds the one
# export it needs at runtime, so an empty file is all this needs.
touch "$ROOTDIR/etc/exports"

mke2fs -q -t ext4 -L dcfs-rootfs -d "$ROOTDIR" -F "$IMG_TMP"

cp "$IMG_TMP" "$OUT"
