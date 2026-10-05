#!/bin/sh
# Test first (docs/plan/phases/04-pinned-host-tools.md, e2fsprogs
# follow-up): confirms the Debian rootfs image (//third_party/debian:rootfs)
# has *real* root:root ownership and setuid bits, not just whatever uid ran
# the Bazel action that assembled it (see README.md's "Ownership" section).
# Host-side (no root, no kernel -- same spirit as
# //third_party/debian:version_check_test and
# //third_party/e2fsprogs:smoke_test): debugfs's own `stat` reads an ext4
# image's inodes directly, no mount of any kind needed.
#
# Before this step, every file in this image was owned by whatever uid/gid
# ran the genrule (tar --no-same-owner during extraction -- see git log for
# the exact failing output this test produced against that image: User/
# Group were the build uid, not 0, and /bin/mount's setuid bit, while
# present in the mode field tar extraction preserves regardless of owner,
# proved nothing about real root ownership since every test here already
# runs as the same non-root build uid).
#
# Run as a Bazel sh_test (see ../BUILD.bazel's ownership_test):
#   ownership_test.sh <debugfs> <rootfs.ext4>
set -eu

DEBUGFS=$1
IMAGE=$2

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

stat_path() {
	"$DEBUGFS" -R "stat $1" "$IMAGE" 2>&1
}

check_root_owned() {
	path=$1
	out=$(stat_path "$path") || fail "debugfs stat $path failed:
$out"
	echo "$out" | grep -qE 'User:[[:space:]]*0[[:space:]]+Group:[[:space:]]*0' ||
		fail "$path is not owned root:root; debugfs stat said:
$out"
	echo "PASS: $path is owned root:root"
}

# The filesystem root, a plain config file every package-provided
# postinst would normally chown as part of installation, and the two
# NFS-server binaries guest/nfs.sh actually execs as root. /etc/passwd
# itself isn't a stand-in here: this package set never pulls in a
# `passwd`/`base-files`-provided one at all (confirmed: `tar -tf` on
# @debian//:flat has no etc/passwd, etc/shadow or etc/group -- the
# packages this image installs don't need one, and nothing here ever
# logs in). /etc/os-release is shipped by `base-files`'s own data.tar
# (pulled in transitively) and serves the same purpose for this test: an
# ordinary config file, not a binary, not one of mkrootfs.sh's own
# fixups. /bin/mount, not /usr/bin/mount: this bookworm pin predates
# usr-merge taking effect for util-linux's own packaging (confirmed:
# `tar -tf` on @debian//:flat has ./bin/mount, not ./usr/bin/mount).
check_root_owned /
check_root_owned /etc/os-release
check_root_owned /usr/sbin/rpc.nfsd
check_root_owned /bin/mount

# /bin/mount is the one binary in this package set Debian ships setuid
# root (confirmed: `tar -tvf` on @debian//:flat lists it
# "-rwsr-xr-x root/root ... ./bin/mount" -- baked into the .deb's
# data.tar payload directly, unlike the ownership fixups mkrootfs.sh adds
# by hand). A tar extraction that discards ownership still preserves the
# mode bits (tar's mode field is independent of uid/gid), so this check
# alone would not have caught the pre-this-step bug -- it exists to
# confirm the setuid bit *also* survived the switch to `mke2fs -d
# <tarball>`, not as the primary regression test (that's the four
# User:0 Group:0 checks above).
mount_stat=$(stat_path /bin/mount) || fail "debugfs stat /bin/mount failed:
$mount_stat"
echo "$mount_stat" | grep -qE 'Mode:[[:space:]]*04[0-7][0-7][0-7]' ||
	fail "/bin/mount is not setuid root; debugfs stat said:
$mount_stat"
echo "PASS: /bin/mount is setuid root"

# The image is made with the checked-in //third_party/e2fsprogs:mke2fs.conf
# (mkrootfs.sh exports MKE2FS_CONFIG), not whatever profile the host or the
# Bazel-built mke2fs's baked-in path would give: spot-check two features the
# host's /etc/mke2fs.conf on this machine lacks (R3, L5/L10).
features=$("$DEBUGFS" -R 'show_super_stats -h' "$IMAGE" 2>/dev/null |
	sed -n 's/^Filesystem features:[[:space:]]*//p')
for f in metadata_csum_seed orphan_file; do
	case " $features " in
	*" $f "*) ;;
	*) fail "image lacks the ext4 feature $f; features: $features" ;;
	esac
done
echo "PASS: image has metadata_csum_seed and orphan_file (checked-in mke2fs.conf)"

echo "PASS: all checks passed"
