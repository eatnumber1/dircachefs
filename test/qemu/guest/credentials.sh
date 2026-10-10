#!/bin/sh
# dcfs step 4.7 acceptance test: backing operations take on the FUSE
# caller's identity.
#
# dcfs runs as root; without taking on the caller's filesystem credentials
# for the backing syscalls, an object a normal user creates through dcfs
# ends up owned by root on the backing filesystem, and every later
# owner-dependent rule (chown/chgrp, truncate, explicit utimes, user xattrs,
# sticky-directory deletion and renames) is then decided against the wrong
# owner. This test acts on the mount as two unprivileged users --
#
#   alice = uid 1000, gid 1000, supplementary groups 1000,2000
#   bob   = uid 1001, gid 1001, supplementary group 1001
#
# (via //tools:testutil's "runas": the guest has no /etc/passwd users) --
# and checks the backing filesystem (/src) and the mount (/mnt) agree with
# what the same operations do on a local filesystem: ownership of every
# create-family op, setgid-directory group inheritance, supplementary
# groups, chown/chgrp rules, sticky-directory rules, truncate, utimes,
# chmod and user xattrs, allowed and denied, and POSIX ACLs (named entries
# that deny or grant access against the mode bits, default ACL inheritance
# and where the umask applies). Finally it checks that the
# daemon itself is back to root afterwards (its own creates are owned by
# root, and its /proc status shows fsuid/fsgid 0 and its original groups).
#
# dcfs always mounts with allow_other (step 15.8), so users other than root
# can reach it, and with default_permissions.
#
# Run as /tests/credentials.sh by guest/init when booted with
# dcfs_test=credentials.sh; prints one "TEST ... PASS/FAIL" line per check
# and exits nonzero if any check failed.
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

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		cat "$LOG" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "credentials.sh: kernel $(uname -r)"

alice() { "$TESTUTIL" runas 1000 1000 1000,2000 -- "$@"; }
bob() { "$TESTUTIL" runas 1001 1001 1001 -- "$@"; }

# expect_ok NAME CMD...: CMD must succeed.
expect_ok() {
	name=$1
	shift
	if out=$("$@" 2>&1); then
		pass "$name"
	else
		fail "$name" "failed: $out"
	fi
}

# expect_fail NAME WANT CMD...: CMD must fail, with WANT (a fragment of the
# strerror() text, or testutil's "ERR <errno-name>") in its output.
expect_fail() {
	name=$1
	shift
	want=$1
	shift
	out=$("$@" 2>&1)
	rc=$?
	if [ "$rc" -eq 0 ]; then
		fail "$name" "unexpectedly succeeded"
		return
	fi
	case "$out" in
	*"$want"*) pass "$name" ;;
	*) fail "$name" "want '$want', got: $out" ;;
	esac
}

# expect_stat NAME FORMAT PATH WANT: `stat -c FORMAT` of PATH (not
# following a symlink) on both /src and /mnt must be WANT.
expect_stat() {
	name=$1
	format=$2
	path=$3
	want=$4
	got_src=$(stat -c "$format" "$SRC/$path" 2>&1)
	got_mnt=$(stat -c "$format" "$MNT/$path" 2>&1)
	if [ "$got_src" = "$want" ] && [ "$got_mnt" = "$want" ]; then
		pass "$name"
	else
		fail "$name" "$path: want $want, got src=$got_src mnt=$got_mnt"
	fi
}

# --- the backing tree, built directly on /src as root ---------------------

mount /dev/vdb /src
umask 022
mkdir /src/pub /src/sgid /src/grp /src/sticky
chmod 0777 /src/pub
chgrp 2000 /src/sgid /src/grp
chmod 2777 /src/sgid
chmod 0770 /src/grp
chmod 1777 /src/sticky
echo root >/src/pub/root644
chmod 0644 /src/pub/root644
echo root >/src/pub/root666
chmod 0666 /src/pub/root666
sync

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

daemon_status() { grep -E '^(Uid|Gid|Groups):' "/proc/$DAEMON_PID/status"; }
status_before=$(daemon_status)

# --- ownership of every create-family op ----------------------------------

expect_ok create-as-alice alice touch "$MNT/pub/af"
expect_stat create-owner '%u:%g' pub/af 1000:1000
expect_ok mkdir-as-alice alice mkdir "$MNT/pub/ad"
expect_stat mkdir-owner '%u:%g' pub/ad 1000:1000
expect_ok mkfifo-as-alice alice mkfifo "$MNT/pub/ap"
expect_stat mkfifo-owner '%u:%g' pub/ap 1000:1000
expect_ok symlink-as-alice alice ln -s af "$MNT/pub/al"
expect_stat symlink-owner '%u:%g' pub/al 1000:1000
expect_ok create-as-bob bob sh -c "echo bob >$MNT/pub/bf"
expect_stat create-owner-bob '%u:%g' pub/bf 1001:1001

# --- setgid directory: group (and, for a directory, g+s) inherited ---------

expect_ok sgid-mkdir-as-alice alice mkdir "$MNT/sgid/sub"
expect_stat sgid-mkdir-inherits '%u:%g:%a' sgid/sub 1000:2000:2755
# bob is not in group 2000; the new file's group is still inherited.
expect_ok sgid-create-as-bob bob touch "$MNT/sgid/bf"
expect_stat sgid-create-inherits '%u:%g:%a' sgid/bf 1001:2000:644

# --- supplementary groups -------------------------------------------------

# grp is root:2000 0770: alice reaches it only through supplementary group
# 2000, bob not at all.
expect_ok group-write-as-alice alice mkdir "$MNT/grp/ad"
expect_stat group-write-owner '%u:%g' grp/ad 1000:1000
expect_fail group-write-denied-bob "Permission denied" bob mkdir "$MNT/grp/bd"

# --- chown/chgrp rules ----------------------------------------------------

expect_ok chgrp-own-to-member alice chgrp 2000 "$MNT/pub/af"
expect_stat chgrp-own-to-member-result '%u:%g' pub/af 1000:2000
expect_fail chgrp-own-to-nonmember "Operation not permitted" \
	alice chgrp 1001 "$MNT/pub/af"
expect_fail chown-own-to-other "Operation not permitted" \
	alice chown 1001 "$MNT/pub/af"
expect_fail chgrp-others "Operation not permitted" \
	alice chgrp 2000 "$MNT/pub/bf"
expect_stat chown-denied-unchanged '%u:%g' pub/af 1000:2000

# --- chmod ----------------------------------------------------------------

expect_ok chmod-own alice chmod 0640 "$MNT/pub/af"
expect_stat chmod-own-result '%a' pub/af 640
expect_fail chmod-others "Operation not permitted" \
	alice chmod 0600 "$MNT/pub/root644"
expect_stat chmod-others-unchanged '%a' pub/root644 644

# --- truncate -------------------------------------------------------------

expect_ok truncate-own alice "$TESTUTIL" truncate "$MNT/pub/af" 100
expect_stat truncate-own-result '%s' pub/af 100
expect_fail truncate-others "ERR EACCES" \
	alice "$TESTUTIL" truncate "$MNT/pub/root644" 0
expect_stat truncate-others-unchanged '%s' pub/root644 5

# --- utimes ---------------------------------------------------------------

expect_ok utimes-own alice "$TESTUTIL" utimens "$MNT/pub/af" 1000000000 0
expect_stat utimes-own-result '%Y' pub/af 1000000000
# Explicit times need ownership; "now" only write permission.
expect_fail utimes-others-explicit "ERR EPERM" \
	alice "$TESTUTIL" utimens "$MNT/pub/root666" 1000000000 0
expect_ok utimes-others-now-writable alice touch "$MNT/pub/root666"
expect_fail utimes-others-now-readonly "Permission denied" \
	alice touch "$MNT/pub/root644"

# --- user xattrs ----------------------------------------------------------

expect_ok setxattr-own alice "$TESTUTIL" setxattr "$MNT/pub/ad" user.k v
got=$("$TESTUTIL" getxattr "$SRC/pub/ad" user.k 2>&1)
if [ "$got" = "v" ]; then
	pass setxattr-own-result
else
	fail setxattr-own-result "got '$got'"
fi
expect_ok removexattr-own alice "$TESTUTIL" removexattr "$MNT/pub/ad" user.k
expect_fail setxattr-others "ERR EACCES" \
	alice "$TESTUTIL" setxattr "$MNT/pub/root644" user.k v

# --- sticky directory -----------------------------------------------------

expect_ok sticky-create-bob bob touch "$MNT/sticky/bf"
expect_stat sticky-create-bob-owner '%u:%g' sticky/bf 1001:1001
expect_fail sticky-unlink-others "Operation not permitted" \
	alice rm -f "$MNT/sticky/bf"
expect_fail sticky-rename-others "ERR EPERM" \
	alice "$TESTUTIL" rename2 "$MNT/sticky/bf" "$MNT/sticky/stolen" 0
if [ -e "$SRC/sticky/bf" ] && [ ! -e "$SRC/sticky/stolen" ]; then
	pass sticky-others-unchanged
else
	fail sticky-others-unchanged "bob's file was removed or renamed"
fi
expect_ok sticky-rename-own bob "$TESTUTIL" rename2 \
	"$MNT/sticky/bf" "$MNT/sticky/bf2" 0
# A file of its own that exists whether or not the rename above worked
# (rm -f of a missing file would "succeed" vacuously).
expect_ok sticky-create-bob-2 bob touch "$MNT/sticky/bf3"
expect_ok sticky-unlink-own bob rm -f "$MNT/sticky/bf3"
if [ ! -e "$SRC/sticky/bf3" ]; then
	pass sticky-unlink-own-result
else
	fail sticky-unlink-own-result "bf3 still exists"
fi
expect_ok sticky-mkdir-alice alice mkdir "$MNT/sticky/ad"
expect_fail sticky-rmdir-others "Operation not permitted" \
	bob rmdir "$MNT/sticky/ad"
expect_ok sticky-rmdir-own alice rmdir "$MNT/sticky/ad"

# --- POSIX ACLs are enforced on the mount ---------------------------------
#
# The ACLs are written in their binary xattr form (testutil setxattrhex;
# the guest has no setfacl): a 4-byte little-endian version (2), then one
# 8-byte entry per tag (u16 tag, u16 perm, u32 id; USER_OBJ 01, USER 02,
# GROUP_OBJ 04, MASK 10, OTHER 20; id ffffffff for the unnamed entries).
# With an ACL, the mode's group bits are the mask, so a kernel that checks
# the mode bits alone gets every named entry wrong.

# acl_deny: alice's 0664 file whose ACL denies bob by name:
# user::rw- user:bob:--- group::r-- mask::rw- other::r--. By the mode
# bits bob is "other" and may read; by the ACL he may not.
ACL_DENY_BOB=02000000\
01000600ffffffff\
02000000e9030000\
04000400ffffffff\
10000600ffffffff\
20000400ffffffff
# acl_grant: root's 0640 file whose ACL grants bob read by name:
# user::rw- user:bob:r-- group::r-- mask::r-- other::---. By the mode bits
# bob is "other" and may not read; by the ACL he may.
ACL_GRANT_BOB=02000000\
01000600ffffffff\
02000400e9030000\
04000400ffffffff\
10000400ffffffff\
20000000ffffffff
# A minimal default ACL, user::rwx group::rwx other::rwx: new objects take
# their permissions from it and from the requested mode, and the creating
# process's umask is NOT applied (POSIX.1e; ext4's posix_acl_create).
ACL_DEFAULT_RWX=02000000\
01000700ffffffff\
04000700ffffffff\
20000700ffffffff

# Set as their owners through the mount, so phase 3's read-back of the
# stored ACL (and of the mode it implies) is exercised too.
expect_ok acl-deny-create alice sh -c "echo secret >$MNT/pub/acl_deny"
expect_ok acl-deny-chmod alice chmod 0664 "$MNT/pub/acl_deny"
expect_ok acl-deny-set alice "$TESTUTIL" setxattrhex "$MNT/pub/acl_deny" \
	system.posix_acl_access "$ACL_DENY_BOB"
expect_stat acl-deny-mode '%a' pub/acl_deny 664
echo secret >"$MNT/pub/acl_grant"
chmod 0640 "$MNT/pub/acl_grant"
expect_ok acl-grant-set "$TESTUTIL" setxattrhex "$MNT/pub/acl_grant" \
	system.posix_acl_access "$ACL_GRANT_BOB"
expect_stat acl-grant-mode '%a' pub/acl_grant 640

# The backing filesystem enforces both ACLs; the mount must agree.
expect_fail acl-deny-bob-src "Permission denied" bob cat "$SRC/pub/acl_deny"
expect_fail acl-deny-bob-mnt "Permission denied" bob cat "$MNT/pub/acl_deny"
expect_ok acl-deny-alice-mnt alice cat "$MNT/pub/acl_deny"
expect_ok acl-deny-other-mnt "$TESTUTIL" runas 1002 1002 1002 -- \
	cat "$MNT/pub/acl_deny"
expect_ok acl-grant-bob-src bob cat "$SRC/pub/acl_grant"
expect_ok acl-grant-bob-mnt bob cat "$MNT/pub/acl_grant"
expect_fail acl-grant-other-mnt "Permission denied" \
	"$TESTUTIL" runas 1002 1002 1002 -- cat "$MNT/pub/acl_grant"

# Default ACL inheritance, with the caller's umask 022: a file created
# 0666 and a directory created 0777 keep every bit, as on a local
# filesystem (the umask applies only where the parent has no default ACL).
expect_ok acl-default-mkdir alice mkdir "$MNT/pub/dacl"
expect_ok acl-default-set alice "$TESTUTIL" setxattrhex "$MNT/pub/dacl" \
	system.posix_acl_default "$ACL_DEFAULT_RWX"
expect_ok acl-default-create alice sh -c \
	"umask 022 && touch $MNT/pub/dacl/f && mkdir $MNT/pub/dacl/d"
expect_stat acl-default-file-mode '%a' pub/dacl/f 666
expect_stat acl-default-dir-mode '%a' pub/dacl/d 777
got=$("$TESTUTIL" getxattrhex "$MNT/pub/dacl/d" system.posix_acl_default 2>&1)
if [ "$got" = "$ACL_DEFAULT_RWX" ]; then
	pass acl-default-dir-inherits
else
	fail acl-default-dir-inherits "got '$got'"
fi
# Without a default ACL the umask still applies.
expect_ok acl-umask-create alice sh -c \
	"umask 027 && touch $MNT/pub/umf && mkdir $MNT/pub/umd"
expect_stat acl-umask-file-mode '%a' pub/umf 640
expect_stat acl-umask-dir-mode '%a' pub/umd 750

# --- the daemon is root again afterwards ----------------------------------

touch "$MNT/pub/rf"
mkdir "$MNT/pub/rd"
expect_stat root-create-after '%u:%g' pub/rf 0:0
expect_stat root-mkdir-after '%u:%g' pub/rd 0:0
status_after=$(daemon_status)
if [ "$status_after" = "$status_before" ]; then
	pass daemon-credentials-restored
else
	fail daemon-credentials-restored "before: $status_before; after: $status_after"
fi

exit "$FAILED"
