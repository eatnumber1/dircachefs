#!/bin/sh
# dcfs step 23.4: copy_file_range, reflinks, the forwarded ioctls and
# O_TMPFILE, each compared with the backing filesystem.
#
# - copy_file_range through dcfs is the backing filesystem's own
#   copy_file_range on the two backing files (FUSE_COPY_FILE_RANGE): on
#   btrfs and xfs that shares the extents (a reflink), as it does natively;
#   before 23.4 the kernel fell back to copying the data. The copy's size
#   is cached, and a later metadata pass reads nothing.
# - FICLONE/FICLONERANGE/FIDEDUPERANGE never reach a FUSE server: the VFS
#   handles them (do_vfs_ioctl) and FUSE has no remap_file_range, so they
#   fail with EOPNOTSUPP on every backing filesystem (a documented
#   limitation; cp --reflink=auto then uses copy_file_range).
# - FS_IOC_GETFLAGS/SETFLAGS and FSGETXATTR (the VFS's fileattr calls,
#   which FUSE sends as FUSE_IOCTL) and FS_IOC_GETVERSION are forwarded to
#   the backing file (chattr +i works, and keeps the file immutable through
#   dcfs); any other ioctl gets ENOTTY.
# - O_TMPFILE creates an unnamed backing file; linking it into a name (by
#   AT_EMPTY_PATH or through /proc/self/fd) gives it one, an O_EXCL one
#   cannot be linked, and one never linked leaves nothing behind.
#
# References are measured on the backing filesystem before dcfs mounts it.
#
# Run as /tests/copy.sh by guest/init when booted with dcfs_test=copy.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		cat "$LOG" 2>/dev/null
	fi
	"$TESTUTIL" setflags "$SRC/imm" 0 >/dev/null 2>&1 || true
	"$TESTUTIL" setflags "$SRC/imm2" 0 >/dev/null 2>&1 || true
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "copy.sh: kernel $(uname -r)"

expect_same() {
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1" "backing filesystem: '$2'; dcfs: '$3'"
	fi
}

expect_eq() {
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1" "want '$2', got '$3'"
	fi
}

mount /dev/vdb /src
FSTYPE=$(backing_fstype "$SRC")
echo "backing filesystem: $FSTYPE"
# 1 MiB of data: enough for an extent the filesystems can share.
dd if=/dev/urandom of=/src/data bs=64k count=16 2>/dev/null
echo plain >/src/plain
mkdir /src/dir /src/ref
echo plain >/src/ref/plain
sync

# --- what the backing filesystem does ---------------------------------------

ref_copy=$("$TESTUTIL" copyrange /src/data /src/ref/copy 2>&1)
sync
ref_shared=$("$TESTUTIL" shared-extents /src/ref/copy 2>&1)
echo "$FSTYPE copy_file_range: $ref_copy, $ref_shared"
ref_clone=$("$TESTUTIL" clone /src/data /src/ref/clone 2>&1)
echo "$FSTYPE FICLONE: $ref_clone"
ref_flags=$("$TESTUTIL" getflags /src/ref/plain 2>&1)
ref_fsx=$("$TESTUTIL" fsxattr /src/ref/plain 2>&1)
ref_unknown=$("$TESTUTIL" ioctl-unknown /src/ref/plain 2>&1)
# Step 23.7 (M1): on an ext4 with the casefold feature, chattr +F makes an
# empty directory case-insensitive. The raw filesystem allows it; dcfs,
# whose cache is case-sensitive, must refuse it. The test's disk is made with
# the feature (-O casefold -E encoding=utf8, in BUILD.bazel): switching it on
# under the mounted filesystem leaves the kernel without the encoding, and
# the next readdir of a casefolded directory oopses.
casefold=0
if [ "$FSTYPE" = ext4 ]; then
	casefold=1
	mkdir /src/ref/cf /src/cf
	ref_cf=$("$TESTUTIL" getflags /src/ref/cf)
	ref_cf_set=$("$TESTUTIL" setflags /src/ref/cf \
		"$(printf '%x' $((0x$ref_cf | 0x40000000)))" 2>&1 &&
		"$TESTUTIL" getflags /src/ref/cf)
	echo "ext4 chattr +F on the raw filesystem: $ref_cf_set"
	expect_eq casefold-raw-accepts "$(printf '%x' $((0x$ref_cf | 0x40000000)))" "$ref_cf_set"
fi
echo "$FSTYPE flags: $ref_flags; fsxattr: $ref_fsx; $ref_unknown"
for how in empty proc excl none; do
	eval "ref_tmp_$how=\$(\"\$TESTUTIL\" tmpfile /src/ref t_$how $how 2>&1)"
	eval "echo \"$FSTYPE O_TMPFILE ($how): \$ref_tmp_$how\""
done
rm -rf /src/ref
sync

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# --- copy_file_range ----------------------------------------------------------

expect_same copy-file-range "$ref_copy" \
	"$("$TESTUTIL" copyrange "$MNT/data" "$MNT/copy" 2>&1)"
if cmp -s /src/data /src/copy; then
	pass copy-file-range-content
else
	fail copy-file-range-content "/src/copy differs from /src/data"
fi
sync
expect_same copy-file-range-shares-extents "$ref_shared" \
	"$("$TESTUTIL" shared-extents /src/copy 2>&1)"
expect_eq copy-file-range-size "$(stat -c %s /src/copy)" \
	"$(stat -c %s "$MNT/copy")"
expect_eq copy-file-range-mtime "$(stat -c %Y /src/copy)" \
	"$(stat -c %Y "$MNT/copy")"

# --- reflinks: the VFS answers before dcfs ----------------------------------

expect_eq ficlone-eopnotsupp "ERR EOPNOTSUPP" \
	"$("$TESTUTIL" clone "$MNT/data" "$MNT/clone" 2>&1)"

# --- ioctls -------------------------------------------------------------------

expect_same getflags "$ref_flags" "$("$TESTUTIL" getflags "$MNT/plain" 2>&1)"
expect_same fsxattr "$ref_fsx" "$("$TESTUTIL" fsxattr "$MNT/plain" 2>&1)"
expect_eq getversion "$("$TESTUTIL" getversion /src/plain 2>&1)" \
	"$("$TESTUTIL" getversion "$MNT/plain" 2>&1)"
expect_eq getflags-dir "$("$TESTUTIL" getflags /src/dir 2>&1)" \
	"$("$TESTUTIL" getflags "$MNT/dir" 2>&1)"
expect_eq ioctl-unknown-enotty "ioctl getfslabel=ENOTTY" \
	"$("$TESTUTIL" ioctl-unknown "$MNT/plain" 2>&1)"

# chattr +i through dcfs: the backing file is immutable, dcfs refuses to
# change it as the backing filesystem does, and lsattr agrees; then -i.
echo imm >"$MNT/imm"
# A settle, then a stat that caches the file's attributes in the kernel:
# the chattr below lands in a later second than the cached ctime.
sleep 1
stat "$MNT/imm" >/dev/null
flags=$("$TESTUTIL" getflags "$MNT/imm")
imm=$(printf '%x' $((0x$flags | 0x10)))
if "$TESTUTIL" setflags "$MNT/imm" "$imm" >/tmp/setflags.out 2>&1; then
	pass setflags-immutable
else
	fail setflags-immutable "$(cat /tmp/setflags.out)"
fi
expect_eq setflags-backing "$imm" "$("$TESTUTIL" getflags /src/imm 2>&1)"
expect_eq setflags-mnt "$imm" "$("$TESTUTIL" getflags "$MNT/imm" 2>&1)"
if sh -c "echo x >>$MNT/imm" 2>/dev/null; then
	fail immutable-refuses-write "append to an immutable file succeeded"
else
	pass immutable-refuses-write
fi
expect_eq immutable-content "imm" "$(cat /src/imm)"
# The ctime the chattr gave the backing file, seen through dcfs. Fails on
# every run on today's kernel (the settle above), so it is disabled.
immutable_ctime() {
	ic_want=$(stat -c %Z /src/imm)
	ic_got=$(stat -c %Z "$MNT/imm")
	echo "backing $ic_want, through dcfs $ic_got"
	[ "$ic_want" = "$ic_got" ]
}
disabled immutable-ctime "kernel: fuse_fileattr_set and vfs_fileattr_set do not invalidate the FUSE inode's cached attributes after a successful FS_IOC_SETFLAGS, so stat serves the ctime of the GETATTR before the chattr until the attribute timeout; README Limitations" immutable_ctime
"$TESTUTIL" setflags "$MNT/imm" "$flags" >/dev/null 2>&1
expect_eq clearflags-backing "$flags" "$("$TESTUTIL" getflags /src/imm 2>&1)"
if echo x >>"$MNT/imm" 2>/dev/null; then
	pass writable-again
else
	fail writable-again "append after chattr -i failed"
fi

# The same while the file is still open for writing from before the flag
# (dcfs shares one backing descriptor per file, opened read-write before
# the file became immutable): a new writable open is refused all the same,
# as the backing filesystem refuses it.
echo held >"$MNT/imm2"
"$TESTUTIL" writehold "$MNT/imm2" append 0 >/tmp/hold.out 2>&1 &
HOLD_PID=$!
wait_for_line /tmp/hold.out READY "$HOLD_PID" || true
flags=$("$TESTUTIL" getflags "$MNT/imm2")
"$TESTUTIL" setflags "$MNT/imm2" "$(printf '%x' $((0x$flags | 0x10)))"
if sh -c "echo x >>$MNT/imm2" 2>/dev/null; then
	fail immutable-refuses-write-while-open "append succeeded"
else
	pass immutable-refuses-write-while-open
fi
kill "$HOLD_PID" 2>/dev/null || true
wait "$HOLD_PID" 2>/dev/null || true
"$TESTUTIL" setflags "$MNT/imm2" "$flags"
expect_eq immutable-content-while-open "held" "$(cat /src/imm2)"

# --- casefold (M1): chattr +F through dcfs is refused ---------------------------

if [ "$casefold" -eq 1 ]; then
	cf=$("$TESTUTIL" getflags "$MNT/cf")
	expect_eq casefold-refused "ERR EOPNOTSUPP" \
		"$("$TESTUTIL" setflags "$MNT/cf" "$(printf '%x' $((0x$cf | 0x40000000)))" 2>&1)"
	expect_eq casefold-backing-unchanged "$cf" "$("$TESTUTIL" getflags /src/cf)"
	# Other flags still go through.
	expect_eq casefold-other-flags "" \
		"$("$TESTUTIL" setflags "$MNT/cf" "$(printf '%x' $((0x$cf | 0x40)))" 2>&1)"
	"$TESTUTIL" setflags "$MNT/cf" "$cf"
else
	skip casefold-refused "backing filesystem is $FSTYPE, not ext4"
fi

# --- O_TMPFILE ----------------------------------------------------------------

for how in empty proc excl none; do
	eval "want=\$ref_tmp_$how"
	expect_same "tmpfile-$how" "$want" \
		"$("$TESTUTIL" tmpfile "$MNT/dir" "t_$how" "$how" 2>&1)"
done
expect_eq tmpfile-listing "$(ls -1 /src/dir | tr '\n' ' ')" \
	"$(ls -1 "$MNT/dir" | tr '\n' ' ')"
expect_eq tmpfile-dir-mtime "$(stat -c %Y /src/dir)" "$(stat -c %Y "$MNT/dir")"
expect_eq tmpfile-linked-ino "$(stat -c %i /src/dir/t_empty)" \
	"$(stat -c %i "$MNT/dir/t_empty")"

# --- warm: a metadata pass over everything above reads nothing ---------------

find "$MNT" -exec stat {} + >/dev/null 2>&1
drop_caches_quiesced
before=$(sectors_read vdb)
find "$MNT" -exec stat {} + >/dev/null 2>&1
after=$(sectors_read vdb)
expect_eq warm-metadata-vdb "$before" "$after"

exit "$FAILED"
