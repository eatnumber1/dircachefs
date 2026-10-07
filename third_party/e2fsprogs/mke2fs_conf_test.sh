#!/bin/sh
# Host-side test: mke2fs, run with the checked-in
# mke2fs.conf (MKE2FS_CONFIG), produces the ext4 feature set the config
# names -- never whatever the host's /etc/mke2fs.conf or a sandbox-path-baked
# default says (docs/plan/audits/review-2026-10-06-waves-1-2.md, L5/L10).
# `mke2fs -t ext4` is what mkfs.ext4 does, and debugfs (Alpine's too)
# stands in for dumpe2fs -h, so the host's e2fsprogs is not needed at all.
#
# Usage: mke2fs_conf_test.sh <mke2fs> <debugfs> <mke2fs.conf>
set -eu

MKE2FS=$1
DEBUGFS=$2
CONF=$3

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

features_of() { # <image> -> sorted, one per line
	"$DEBUGFS" -R 'show_super_stats -h' "$1" 2>/dev/null |
		sed -n 's/^Filesystem features:[[:space:]]*//p' | tr ' ' '\n' | sort
}

# What the checked-in config asks for: base_features + the ext4 fs_type's
# features. Derived from the file itself, so editing the config without
# updating the expectation below (and vice versa) fails.
want_conf=$(
	{
		sed -n 's/^[[:space:]]*base_features[[:space:]]*=[[:space:]]*//p' "$CONF"
		sed -n '/^[[:space:]]*ext4[[:space:]]*=[[:space:]]*{/,/}/s/^[[:space:]]*features[[:space:]]*=[[:space:]]*//p' "$CONF"
	} | tr ',' '\n' | sort -u
)

# The fixed expectation (1.47.4's own default profile, which the checked-in
# config reproduces): in particular orphan_file and metadata_csum_seed, which
# the host's /etc/mke2fs.conf on this machine lacks.
want_fixed="64bit
dir_index
dir_nlink
ext_attr
extent
extra_isize
filetype
flex_bg
has_journal
huge_file
large_file
metadata_csum
metadata_csum_seed
orphan_file
resize_inode
sparse_super"
want_fixed=$(echo "$want_fixed" | sort)

[ "$want_conf" = "$want_fixed" ] ||
	fail "the checked-in config's ext4 features changed; update this test too.
config: $(echo $want_conf)
test:   $(echo $want_fixed)"

truncate -s 256M "$WORK/test.img"
MKE2FS_CONFIG="$CONF" "$MKE2FS" -q -F -t ext4 "$WORK/test.img" ||
	fail "mke2fs -t ext4 failed"
got=$(features_of "$WORK/test.img")
[ "$got" = "$want_fixed" ] ||
	fail "ext4 features differ from the expected set:
got:  $(echo $got)
want: $(echo $want_fixed)"
echo "PASS: mke2fs -t ext4 with the checked-in config: $(echo $got)"

# R3: test images resemble a real disk: 4 KiB blocks at every size (1.47.4's
# own "small" and "floppy" profiles would give 1 KiB below 512 MiB / 3 MiB).
for size in 2M 64M 256M; do
	truncate -s "$size" "$WORK/bs.img"
	MKE2FS_CONFIG="$CONF" "$MKE2FS" -q -F -t ext4 "$WORK/bs.img" ||
		fail "mke2fs -t ext4 failed on a $size image"
	bs=$("$DEBUGFS" -R 'show_super_stats -h' "$WORK/bs.img" 2>/dev/null |
		sed -n 's/^Block size:[[:space:]]*//p')
	[ "$bs" = 4096 ] || fail "a $size image has block size '$bs', want 4096"
done
echo "PASS: 2M, 64M and 256M images all have 4096-byte blocks"

# The environment variable is what decides, not a host or baked-in file: a
# different config gives a different result.
cat >"$WORK/other.conf" <<'EOT'
[fs_types]
	ext4 = {
		features = has_journal,extent
	}
EOT
truncate -s 256M "$WORK/other.img"
MKE2FS_CONFIG="$WORK/other.conf" "$MKE2FS" -q -F -t ext4 "$WORK/other.img" ||
	fail "mke2fs with a second config failed"
other=$(features_of "$WORK/other.img")
case "$other" in
*orphan_file*) fail "MKE2FS_CONFIG was ignored (orphan_file present with a config lacking it)" ;;
esac
echo "PASS: MKE2FS_CONFIG decides the feature set"

echo "PASS: all checks passed"
