#!/bin/sh
# Build the dcfs QEMU NFS test's Debian rootfs image: an ext4 filesystem
# image containing a small Debian chroot (nfs-kernel-server, nfs-common,
# attr, strace, util-linux, coreutils, e2fsprogs, procps) that the guest
# chroots into for test/qemu:nfs_test (see guest/init's dcfs_rootfs=
# branch and guest/nfs.sh). Not built by Bazel -- same reasoning as
# scripts/build-kernel.sh: it downloads ~200 MB over the network and takes
# the better part of a minute, neither of which belongs in the Bazel
# action graph. Run it once with:
#
#   test/qemu/scripts/mkrootfs-debian.sh
#
# and test/qemu/kernel.bzl's kernel_image repo rule picks up the result
# the same way it already does for the kernel: a symlink to
# $DCFS_ROOTFS_IMAGE (default $HOME/.cache/dcfs/rootfs-debian.ext4) if
# present, a clearly labeled placeholder file otherwise.
#
# Recipe notes (the two non-obvious parts the plan asked to be documented):
#
# 1. Signature verification ("NO_PUBKEY"). mmdebstrap needs a keyring to
#    verify bookworm's InRelease/Release signatures. This host's own
#    debian-archive-keyring (installed via Ubuntu's package, or fetched
#    with `apt-get download debian-archive-keyring`) turned out to be too
#    OLD: Debian periodically adds a new per-release "Automatic Signing
#    Key" as a co-signer (observed here: bookworm's InRelease is
#    additionally signed by a "(13/trixie)" key not present in a
#    2023-vintage keyring), and apt-get/mmdebstrap report this as
#    NO_PUBKEY for every key in the signature, which looks like a mirror
#    or keyring-path problem but is really just a stale key. Fixed by
#    fetching the current debian-archive-keyring .deb directly from
#    Debian's own pool (plain HTTPS, no apt/keyring needed to do that) and
#    caching it; delete $DCFS_ROOTFS_KEYRING and re-run this script if
#    mmdebstrap ever reports NO_PUBKEY again.
#
# 2. Turning the mmdebstrap tarball into an ext4 image, unprivileged.
#    `mkfs.ext4 -d <dir>` needs a real directory, not a tarball, and it
#    preserves whatever ownership the directory's files have -- but a
#    plain `tar -x` run as an unprivileged user either leaves everything
#    owned by that user (wrong: Debian's own file ownership, e.g.
#    /etc/shadow's group, must be preserved) or, if you naively reach for
#    `unshare --user --map-root-user` to fix that, still fails: mapping
#    your uid to 0 only maps ONE id, so tar's chown() calls for every
#    other uid/gid the tarball mentions (etc/shadow's group 42, var/mail's
#    group 8, ...) get EINVAL ("Cannot change ownership ... Invalid
#    argument") because those ids have no mapping at all. The fix is to
#    combine two unshare(1) mapping options that DO compose (unlike two
#    --map-users, where "the last occurrence takes precedence" per
#    unshare(1)): `--map-user=0` (maps our own real uid to inner 0) plus
#    `--map-users=1:<subuid-start>:<subuid-count>` (maps inner 1..N to our
#    delegated /etc/subuid range), and the same pair of options for
#    groups. That gives the namespace a full 0..N id space to chown into,
#    all backed by ids this user is actually allowed to use.
#
#    One thing that combination still cannot do: create real character/
#    block device nodes (mknod for S_IFCHR/S_IFBLK). That specific
#    operation checks capable(CAP_MKNOD) against the INIT user namespace,
#    not the current one -- by design, no amount of unprivileged
#    namespace nesting grants it, so `tar -x`'s attempts to recreate
#    /dev/null, /dev/console etc. fail with EPERM regardless of the id
#    mapping. This doesn't matter here: guest/init bind-mounts the real
#    /dev over the chroot's /dev before entering it (see the dcfs_rootfs=
#    branch), so the image never needs working device nodes of its own.
#    This script excludes ./dev/* from the tar extraction entirely (and
#    recreates an empty /dev as a mount point) rather than let tar fail
#    and stop the whole extraction.
#
#    The directory itself is extracted onto a tmpfs (mounted from inside
#    the same user+mount namespace, so it inherits that namespace's id
#    mapping) rather than a real disk directory, both because it's
#    faster and because it sidesteps host-filesystem ownership entirely.
set -eu

SUITE="${DCFS_ROOTFS_SUITE:-bookworm}"
OUT="${DCFS_ROOTFS_IMAGE:-$HOME/.cache/dcfs/rootfs-debian.ext4}"
SIZE="${DCFS_ROOTFS_SIZE:-1024M}"
KEYRING_CACHE="${DCFS_ROOTFS_KEYRING:-$HOME/.cache/dcfs/debian-archive-keyring.gpg}"
# "mount" is its own binary package (built from the util-linux source) --
# util-linux itself no longer ships /bin/mount or /bin/umount in bookworm,
# so it must be listed explicitly alongside util-linux.
PACKAGES="nfs-kernel-server,nfs-common,attr,strace,util-linux,mount,coreutils,e2fsprogs,procps"

mkdir -p "$(dirname "$OUT")"
LOG="$(dirname "$OUT")/rootfs-debian-build.log"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- keyring -------------------------------------------------------------

if [ ! -s "$KEYRING_CACHE" ]; then
	echo "mkrootfs-debian.sh: fetching debian-archive-keyring..." | tee -a "$LOG"
	deb_name=$(curl -fsSL http://deb.debian.org/debian/pool/main/d/debian-archive-keyring/ |
		grep -o 'debian-archive-keyring_[^"]*\.deb' | sort -V | tail -1)
	if [ -z "$deb_name" ]; then
		echo "mkrootfs-debian.sh: could not find a debian-archive-keyring .deb to download" >&2
		exit 1
	fi
	curl -fsSL "http://deb.debian.org/debian/pool/main/d/debian-archive-keyring/$deb_name" \
		-o "$WORK/keyring.deb"
	dpkg-deb -x "$WORK/keyring.deb" "$WORK/keyring-extract"
	cat "$WORK/keyring-extract/usr/share/keyrings/debian-archive-keyring.gpg" >"$WORK/keyring.gpg"
	chmod 644 "$WORK/keyring.gpg"
	mv "$WORK/keyring.gpg" "$KEYRING_CACHE"
fi

# --- id mapping for the unprivileged tar-extract + mke2fs step -----------

me=$(id -un)
subuid_range=$(awk -F: -v u="$me" '$1==u{print $2":"$3; exit}' /etc/subuid 2>/dev/null || true)
subgid_range=$(awk -F: -v u="$me" '$1==u{print $2":"$3; exit}' /etc/subgid 2>/dev/null || true)
if [ -z "$subuid_range" ] || [ -z "$subgid_range" ]; then
	echo "mkrootfs-debian.sh: no /etc/subuid or /etc/subgid entry for $me;" >&2
	echo "  ask your admin for one (e.g. 'usermod --add-subuids 165536-231071" >&2
	echo "  --add-subgids 165536-231071 $me'), needed to chown the extracted" >&2
	echo "  Debian tree's non-root files (see the recipe comment at the top" >&2
	echo "  of this script)." >&2
	exit 1
fi
subuid_start=${subuid_range%%:*}
subuid_count=${subuid_range##*:}
subgid_start=${subgid_range%%:*}
subgid_count=${subgid_range##*:}

# --- mmdebstrap: suite -> tarball -----------------------------------------
#
# mmdebstrap's own --mode=unshare sandbox reads --keyring from inside its
# internal apt sandbox, which (like the tar-extract/mke2fs step below)
# only has access to files reachable through world-traversable
# directories -- $KEYRING_CACHE typically isn't, since it lives under
# $HOME/.cache, and $HOME/.cache itself is mode 0700 by XDG convention.
# Stage a world-readable copy directly under /tmp (not under $WORK, which
# mktemp -d also makes 0700) rather than loosen $HOME/.cache's
# permissions.
KEYRING_TMP=$(mktemp /tmp/dcfs-rootfs-keyring.XXXXXX)
trap 'rm -rf "$WORK" "$KEYRING_TMP"' EXIT
cp "$KEYRING_CACHE" "$KEYRING_TMP"
chmod 644 "$KEYRING_TMP"

TAR="$WORK/rootfs.tar"
echo "mkrootfs-debian.sh: mmdebstrap $SUITE -> $(basename "$TAR") (downloads ~200MB)..." |
	tee -a "$LOG"
start=$(date +%s)
mmdebstrap --mode=unshare --variant=apt \
	--keyring="$KEYRING_TMP" \
	--include="$PACKAGES" \
	"$SUITE" "$TAR" >>"$LOG" 2>&1
mmdebstrap_secs=$(($(date +%s) - start))

# --- tarball -> ext4 image, unprivileged (see recipe comment above) ------

ROOTDIR="$WORK/root"
mkdir -p "$ROOTDIR"
IMG_TMP="$WORK/rootfs.ext4"
truncate -s "$SIZE" "$IMG_TMP"

INNER="$WORK/inner.sh"
cat >"$INNER" <<'EOF'
set -eu
mount -t tmpfs -o size="$SIZE" tmpfs "$ROOTDIR"
tar -C "$ROOTDIR" --numeric-owner --exclude="./dev/*" -xf "$TAR"
mkdir -p "$ROOTDIR/dev"
# mmdebstrap's unshare mode bind-mounts the HOST's /etc/resolv.conf into its
# build chroot so apt-get update can resolve mirror hostnames -- but that
# leaves the *host's* resolv.conf content (nameserver, search domain, all
# of it) baked into the tarball it produces. Reset it to empty: the guest
# never does a real DNS lookup (everything here is 127.0.0.1 by IP), and a
# leftover nameserver line pointing at an address nothing in the guest is
# listening on is exactly the kind of thing that makes an NSS/resolver
# codepath (e.g. rpc.mountd's reverse-lookup-for-logging on a client
# connection) hang or misbehave for no reason connected to dcfs itself.
: >"$ROOTDIR/etc/resolv.conf"
# /etc/mtab: a "complete" Debian system gets this for free (a symlink to
# /proc/self/mounts, set up by the mount package's postinst, which never
# runs in an --variant=apt bootstrap). Its absence is not cosmetic: found
# by straceing rpc.mountd through a reproducible SIGSEGV (SEGV_MAPERR,
# NULL deref) -- it does `openat("/etc/mtab", O_RDONLY|O_CLOEXEC)` right
# after successfully stat-ing the export path (crossmnt needs /etc/mtab
# to find local mounts below the export), gets ENOENT, and dereferences
# whatever that failure leaves behind instead of checking it. Symlinking
# it here fixes every caller of setmntent("/etc/mtab", ...) in this
# image, not just this one.
ln -sf /proc/self/mounts "$ROOTDIR/etc/mtab"
mke2fs -q -t ext4 -L dcfs-rootfs -d "$ROOTDIR" -F "$IMG_TMP"
EOF

start=$(date +%s)
SIZE="$SIZE" ROOTDIR="$ROOTDIR" TAR="$TAR" IMG_TMP="$IMG_TMP" \
	unshare --user \
	--map-user=0 --map-users="1:${subuid_start}:${subuid_count}" \
	--map-group=0 --map-groups="1:${subgid_start}:${subgid_count}" \
	--mount -- sh "$INNER" >>"$LOG" 2>&1
mke2fs_secs=$(($(date +%s) - start))

mv "$IMG_TMP" "$OUT"
chmod 644 "$OUT"

size=$(stat -c '%s' "$OUT")
{
	echo "Rootfs: $OUT"
	echo "mmdebstrap: ${mmdebstrap_secs}s, tar->ext4: ${mke2fs_secs}s"
	echo "image size: $size bytes ($SIZE nominal)"
} | tee -a "$LOG"
