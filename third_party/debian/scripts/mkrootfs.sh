#!/bin/sh
# Assemble the dcfs QEMU NFS test's Debian rootfs image from the flattened
# package tree rules_distroless's apt extension resolved and fetched (see
# third_party/debian/README.md and ../BUILD.bazel's :rootfs genrule).
# Replaces test/qemu/scripts/mkrootfs-debian.sh's mmdebstrap run: every
# package comes from a pinned snapshot.debian.org timestamp (checked into
# MODULE.bazel) instead of a live mirror, and assembly is a Bazel action
# instead of a step run once by hand into ~/.cache/dcfs.
#
# Invoked as: mkrootfs.sh <out.ext4> <flat.tar> <size> <mke2fs>
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
#   <mke2fs>: the Bazel-built, static //third_party/e2fsprogs:mke2fs --
#   see that package's README.md. Passed in explicitly (not found via
#   PATH) since this is now the *only* tool this script needs that isn't
#   already an implicit host dependency of this project's Bazel actions
#   (tar, same as test/qemu/scripts/mkinitramfs.sh's cpio/gzip).
#
# Ownership: real root:root (and whatever setuid/setgid bits each .deb's
# payload set), not the uid/gid that ran this Bazel action. This is new as
# of this step (docs/plan/phases/04-pinned-host-tools.md's e2fsprogs
# follow-up) -- see README.md's "Ownership" section for the full history:
# earlier phases of this same image extracted @debian//:flat with `tar
# --no-same-owner` (discarding real ownership, since a sandboxed Bazel
# action has no privilege to chown(2) to an arbitrary uid/gid -- fakeroot
# doesn't help either, see README.md) and accepted that as fine for
# nfs_test (which only ever chroots in as root). `mke2fs -d <tarball>`
# (e2fsprogs >= 1.47.1) sidesteps the whole problem: it reads each tar
# entry's own uid/gid/mode header directly into the ext4 image's inodes,
# with no chown(2), no extraction, no privilege of any kind -- so this
# script never materializes @debian//:flat as a real directory tree at
# all any more.
#
# Nothing here touches the network, and nothing is mounted (real or loop):
# both of the plan's "no root, no network, no loop mounts" constraints hold
# throughout; nothing here needs root either, despite the image coming out
# root-owned -- mke2fs -d never chown()s anything, it just copies tar
# header fields into inodes it already has full access to create.
set -eu

OUT=$1
FLAT_TAR=$2
SIZE=$3
MKE2FS=$4

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# e2fsprogs's own tar-import code (misc/create_inode_libarchive.c's
# __populate_fs_from_tar) is not a general-purpose tar extractor: it walks
# entries in stream order and does a plain ext2fs_namei lookup for each
# entry's parent directory, which must therefore already exist in the
# filesystem being built by that point -- unlike a real `tar -x`
# extraction (what every earlier version of this script did), it never
# auto-creates a missing intermediate directory. @debian//:flat does not
# reliably provide one: confirmed two distinct, unrelated gaps by hand
# before writing this fix --
#   - ./var/lib/dpkg/status (rules_distroless's own synthesized dpkg
#     status file, scripts/version_check.sh's input) has no
#     ./var/lib/dpkg directory entry anywhere in the tar at all.
#   - ./etc/ld.so.conf.d/<file> appears before any ./etc/ld.so.conf.d
#     directory entry survives the flatten rule's own directory-entry
#     deduplication (merging many packages' individual data.tars can
#     reorder which package's copy of a shared directory entry "wins").
# Both failed identically: mke2fs aborted outright ("File not found by
# ext2_lookup cannot find directory ... to create ...", confirmed
# reproducible with `mke2fs -d` run directly against @debian//:flat with
# no fixups at all -- see this step's commit log for the exact output).
#
# The general, robust fix: derive the *complete* set of directory paths
# every entry in FLAT_TAR implies (every ancestor of every path, not just
# the ones @debian//:flat happens to declare explicitly) and prepend them
# -- as real root-owned directory entries, built from an actual directory
# tree so a recursive `tar -c` naturally emits each parent before any of
# its children, which is what mke2fs's sequential walk requires -- before
# FLAT_TAR's own content in the combined tar. This makes the combined tar
# strictly more complete than FLAT_TAR alone, never changes any path FLAT_TAR
# already declares (mke2fs -d treats a later duplicate directory entry as a
# no-op, confirmed manually), and does not depend on knowing in advance
# which specific directories are missing.
DIRS_LIST="$WORK/dirs.list"
tar -tf "$FLAT_TAR" | sed -e 's#^\./##' -e 's#/$##' | awk -F/ '
	{
		n = NF - 1
		p = ""
		for (i = 1; i <= n; i++) {
			p = (p == "" ? $i : p "/" $i)
			print p
		}
	}
' | sort -u >"$DIRS_LIST"

SKEL="$WORK/skel"
mkdir -p "$SKEL"
while IFS= read -r d; do
	mkdir -p "$SKEL/$d"
done <"$DIRS_LIST"

COMBINED="$WORK/combined.tar"
tar --owner=0 --group=0 --numeric-owner -cf "$COMBINED" -C "$SKEL" .
tar --concatenate --file="$COMBINED" "$FLAT_TAR"

# The five fixups this image's assembly has always needed (none of them
# shipped by any package's data.tar -- see README.md's "Image assembly"
# for why each one is missing), added the same way as the directory
# skeleton above: real tar entries, built from a small scratch directory
# and tarred with explicit --owner=0 --group=0, then appended onto the
# combined tar -- not by extracting, symlinking by hand and repacking as
# this action's own (non-root) uid, which is exactly the approach that
# discarded ownership in the first place. None of these five paths exist
# in @debian//:flat already (confirmed in README.md's "Image assembly"/
# "Packages" sections), so appending them is pure addition, never an
# overwrite of an already-populated path.
FIXUPS_ROOT="$WORK/fixups"
mkdir -p "$FIXUPS_ROOT/etc" "$FIXUPS_ROOT/usr/local/bin" "$FIXUPS_ROOT/usr/bin" \
	"$FIXUPS_ROOT/bin"

# See test/qemu/scripts/mkrootfs-debian.sh's comment (kept for history) for
# why this one file matters: rpc.mountd's crossmnt handling does
# openat("/etc/mtab", O_RDONLY|O_CLOEXEC) unconditionally and mishandles
# ENOENT (a reproducible NULL-deref SIGSEGV) when it's missing. A stock
# Debian install gets this symlink for free from the mount package's
# postinst; rules_distroless never runs any package's maintainer scripts
# (deb_import only ever unpacks data.tar -- no dpkg database, no postinst,
# matching mmdebstrap's own --variant=apt, which also skipped it), so it is
# recreated here by hand, exactly as the old script did.
ln -s /proc/self/mounts "$FIXUPS_ROOT/etc/mtab"

# /bin/sh: this package set resolves to no shell at all unless `bash` is
# explicitly requested (see README.md's "Packages" section -- rules_distroless
# only pulls in what the named packages' own Depends/Pre-Depends reach, and
# none of them need a shell at runtime), and even with `bash` present,
# nothing sets up the conventional /bin/sh symlink: a stock Debian install
# gets it from dash's postinst (`update-alternatives --install /bin/sh sh
# ...`), which, like every other postinst, never runs here. guest/init's
# dcfs_rootfs= branch does `chroot /newroot /bin/sh "/tests/$TEST"`, so this
# is not optional.
ln -s /bin/bash "$FIXUPS_ROOT/bin/sh"

# /usr/bin/awk: same update-alternatives gap as /bin/sh above. `mawk`
# (Debian's default awk provider) ships only /usr/bin/mawk; the generic
# /usr/bin/awk symlink is normally `update-alternatives --install
# /usr/bin/awk awk /usr/bin/mawk ...` in mawk's own postinst.
ln -s mawk "$FIXUPS_ROOT/usr/bin/awk"

# /etc/exports: nfs-kernel-server's data.tar does not ship this file at
# all (confirmed: `tar -tvf` on its content.tar.gz has no etc/exports) --
# a stock install gets an empty one from the package's postinst. exportfs(8)
# refuses outright ("can't open /etc/exports for reading") if it is
# missing entirely; guest/nfs.sh's own `exportfs -o ...` call adds the one
# export it needs at runtime, so an empty file is all this needs.
touch "$FIXUPS_ROOT/etc/exports"

# /usr/local/bin: guest/init's dcfs_rootfs= branch copies dcfs/fhtest/
# testutil here before chrooting in. A complete Debian install gets this
# (and the rest of the /usr/local hierarchy) from base-files, which nothing
# in this package set depends on, directly or transitively. (The directory
# itself was already created above, alongside etc/ and usr/bin/; tar picks
# it up as part of the `usr` subtree below.)
FIXUPS_TAR="$WORK/fixups.tar"
tar --owner=0 --group=0 --numeric-owner -cf "$FIXUPS_TAR" -C "$FIXUPS_ROOT" \
	etc usr bin

tar --concatenate --file="$COMBINED" "$FIXUPS_TAR"

IMG_TMP="$WORK/rootfs.ext4"
truncate -s "$SIZE" "$IMG_TMP"

"$MKE2FS" -q -t ext4 -L dcfs-rootfs -d "$COMBINED" -F "$IMG_TMP"

cp "$IMG_TMP" "$OUT"
