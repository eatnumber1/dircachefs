#!/bin/sh
# Host-side self-check of the identity oracle in guest/fault_lib.sh (step
# 26.14e, the minimal form of 11.7's): identity_take records the handle of
# every object under a directory, identity_check after a recovery accepts a
# handle that opens to the same object or fails ESTALE and rejects anything
# else, naming the object. A fake fhtest answers from canned files, so every
# violation can be injected: a handle that opens a different object, one that
# opens an object the backing filesystem does not have (an answer for
# something gone: the 23.11 ghost), one that fails with another error, and
# one that answers nonsense.
#
# Usage: identity_oracle_test.sh <guest/fault_lib.sh>
set -eu

LIB=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

MNT=$WORK/mnt
SRC=$WORK/src
mkdir -p "$MNT/t/e" "$SRC/t/e" "$WORK/answers"
for f in t/a t/b t/e/x; do
	echo "$f" >"$MNT/$f"
	echo "$f" >"$SRC/$f"
done

# The fake: `handle PATH` is "1 4 <hex of the path's name>" (a handle per
# object); `stat MOUNT TYPE HEX` prints the canned answer for HEX.
cat >"$WORK/fhtest" <<E
#!/bin/sh
case "\$1" in
handle) echo "1 4 \$(echo "\$2" | sed 's|.*/mnt/||; s|/|_|g')" ;;
stat) cat "$WORK/answers/\$4" ;;
esac
E
chmod +x "$WORK/fhtest"
FHTEST=$WORK/fhtest
# shellcheck disable=SC1090 # the path is an argument
. "$LIB"

# What the objects are now, as dcfs would answer after a clean recovery.
# (The inode numbers are the fake mount's own: the oracle looks a name up by
# inode number under the mount.)
ino() { stat -c %i "$MNT/$1"; }
answers() {
	echo "OK $(ino t/a) 100644 /" >"$WORK/answers/t_a"
	echo "OK $(ino t/b) 100644 /" >"$WORK/answers/t_b"
	echo "OK $(ino t/e) 40755 $MNT/t/e" >"$WORK/answers/t_e"
	echo "OK $(ino t/e/x) 100644 /" >"$WORK/answers/t_e_x"
	echo "OK $(ino t) 40755 $MNT/t" >"$WORK/answers/t"
}
answers

identity_take "$MNT" "$MNT/t" "$WORK/rec"
[ "$(wc -l <"$WORK/rec")" -eq 5 ] || fail "identity_take recorded $(wc -l <"$WORK/rec") objects, want 5 (t, a, b, e, e/x): $(cat "$WORK/rec")"
echo "PASS: identity_take records every object under the directory"

check() { identity_check "$MNT" "$SRC" "$WORK/rec"; }
got=$(check)
[ -z "$got" ] || fail "an unchanged tree was rejected: $got"
echo "PASS: handles that open to the same objects are accepted"

# expect_reject NAME WORDS: after the change the test made, the check says
# something containing WORDS.
expect_reject() {
	got=$(check)
	[ -n "$got" ] || fail "$1: accepted"
	case "$got" in
	*"$2"*) ;;
	*) fail "$1: rejected without saying '$2': $got" ;;
	esac
	echo "PASS: $1 is rejected"
	answers
}

echo "ERR ESTALE" >"$WORK/answers/t_b"
got=$(check)
[ -z "$got" ] || fail "ESTALE was rejected: $got"
echo "PASS: a handle that fails ESTALE is accepted (the object may be gone)"
answers

echo "OK 99 100644 /" >"$WORK/answers/t_a"
expect_reject "a handle that opens a different object (another inode number)" "t/a"
echo "OK $(ino t/b) 40755 /" >"$WORK/answers/t_b"
expect_reject "a handle that opens an object of another type" "t/b"
echo "ERR ENOENT" >"$WORK/answers/t_a"
expect_reject "a handle that fails with ENOENT" "ENOENT"
echo "ERR EIO" >"$WORK/answers/t_a"
expect_reject "a handle that fails with EIO" "EIO"
echo "garbage" >"$WORK/answers/t_a"
expect_reject "an answer that is neither OK nor ERR" "garbage"
: >"$WORK/answers/t_a"
expect_reject "an empty answer" "t/a"

# The 23.11 ghost: dcfs still opens (and serves under a name) a file the
# backing filesystem lost.
mv "$SRC/t/a" "$WORK/a.saved"
expect_reject "a ghost: the handle opens a file the backing filesystem lost" "does not have"
mv "$WORK/a.saved" "$SRC/t/a"
# An object with no name at all under the mount (served nowhere).
mv "$MNT/t/b" "$WORK/b.saved"
got=$(check)
case "$got" in
*"t/b opened inode"*"has no name"*) echo "PASS: a handle that opens an object no name under the mount has is rejected" ;;
*) fail "an object with no name under the mount: '$got'" ;;
esac
mv "$WORK/b.saved" "$MNT/t/b"
# A renamed object still has a name, on both sides: accepted.
mv "$MNT/t/a" "$MNT/t/a2"
mv "$SRC/t/a" "$SRC/t/a2"
got=$(check)
[ -z "$got" ] || fail "an object renamed on both sides was rejected: $got"
echo "PASS: a handle of an object renamed on both sides is accepted"
echo "all checks passed"
