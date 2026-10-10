#!/bin/sh
# Host-side self-check of the identity oracle in guest/fault_lib.sh (step
# 26.14e, the minimal form of 11.7's): identity_take records, for every object
# under a directory, the handle dcfs gives it and the backing filesystem's own
# handle of the same name; identity_check after a recovery accepts a handle
# that opens to the same object (with the backing handle still opening to the
# same backing object) or fails ESTALE, and rejects anything else, naming the
# object. A fake testutil answers handle-save and handle-stat from canned
# files, so every violation can be injected: a different object, another type,
# an answer for something gone (the 23.11 ghost: dcfs opens it, the backing
# handle is ESTALE), a recycled inode number (the backing handle opens a
# different inode), another error, and nonsense.
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
	: >"$MNT/$f"
	: >"$SRC/$f"
done

# The fake: `handle-save PATH FILE` writes "1 KEY", KEY being m_<path under the
# mount> or b_<path under the backing filesystem>, '/' as '_'; `handle-stat DIR
# FILE` prints the canned answer for the KEY in FILE.
cat >"$WORK/testutil" <<E
#!/bin/sh
case "\$1" in
handle-save)
	case "\$2" in
	$MNT*) key=m_\$(echo "\${2#$MNT}" | sed 's|^/||; s|/|_|g') ;;
	*) key=b_\$(echo "\${2#$SRC}" | sed 's|^/||; s|/|_|g') ;;
	esac
	echo "1 \$key" >"\$3"
	;;
handle-stat) cat "$WORK/answers/\$(cut -d' ' -f2 "\$3")" ;;
esac
E
chmod +x "$WORK/testutil"
TESTUTIL=$WORK/testutil
IDENT_TMP=$WORK/ident
# shellcheck disable=SC1090 # the path is an argument
. "$LIB"

# What the objects are now, as dcfs and the backing filesystem would answer
# after a clean recovery: "OK SIZE INO MODE". dcfs's inode numbers are its own
# (1x), the backing filesystem's another set (5x).
answer() { echo "$2" >"$WORK/answers/$1"; }
answers() {
	answer m_t "OK 0 10 40755"
	answer m_t_a "OK 0 11 100644"
	answer m_t_b "OK 0 12 100644"
	answer m_t_e "OK 0 13 40755"
	answer m_t_e_x "OK 0 14 100644"
	answer b_t "OK 0 50 40755"
	answer b_t_a "OK 0 51 100644"
	answer b_t_b "OK 0 52 100644"
	answer b_t_e "OK 0 53 40755"
	answer b_t_e_x "OK 0 54 100644"
}
answers

identity_take "$MNT" "$SRC" "$MNT/t" "$WORK/rec"
[ "$(wc -l <"$WORK/rec")" -eq 5 ] || fail "identity_take recorded $(wc -l <"$WORK/rec") objects, want 5 (t, a, b, e, e/x): $(cat "$WORK/rec")"
grep -q "t/a 1 m_t_a 11 100644 1 b_t_a 51 100644" "$WORK/rec" || fail "the record of t/a is not what the handles answered: $(cat "$WORK/rec")"
echo "PASS: identity_take records both handles and what they open for every object under the directory"

check() { identity_check "$MNT" "$SRC" "$WORK/rec" 2>"$WORK/stderr"; }
got=$(check)
[ -z "$got" ] || fail "an unchanged tree was rejected: $got"
[ "$(cat "$WORK/rec.checked")" -eq 5 ] || fail "identity_check counted $(cat "$WORK/rec.checked") handles, want 5"
echo "PASS: handles that open to the same objects are accepted, and counted"

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

# ESTALE is accepted; for an object the backing filesystem still has it is
# also noted.
answer m_t_b "ERR ESTALE"
got=$(check)
[ -z "$got" ] || fail "ESTALE was rejected: $got"
grep -q "t/b failed ESTALE although the backing filesystem still has" "$WORK/stderr" || fail "no note for an ESTALE of an object the backing filesystem has: $(cat "$WORK/stderr")"
[ "$(cat "$WORK/rec.estale")" = t/b ] || fail "the estale list is '$(cat "$WORK/rec.estale")'"
echo "PASS: a handle that fails ESTALE is accepted, and noted when the backing filesystem still has the object"
answer m_t_b "ERR ESTALE"
answer b_t_b "ERR ESTALE"
got=$(check)
[ -z "$got" ] && [ ! -s "$WORK/stderr" ] || fail "ESTALE of an object that is gone was noted or rejected: $got $(cat "$WORK/stderr")"
echo "PASS: ESTALE of an object that is gone is accepted without a note"
answers

answer m_t_a "OK 0 99 100644"
expect_reject "a handle that opens a different object (another inode number)" "t/a"
answer m_t_b "OK 0 12 40755"
expect_reject "a handle that opens an object of another type" "t/b"
answer m_t_a "ERR ENOENT"
expect_reject "a handle that fails with ENOENT" "ENOENT"
answer m_t_a "ERR EIO"
expect_reject "a handle that fails with EIO" "EIO"
answer m_t_a "garbage here"
expect_reject "an answer that is neither OK nor ERR" "garbage here"
answer m_t_a ""
expect_reject "an empty answer" "t/a"

# The 23.11 ghost: dcfs opens the object, the backing filesystem's own handle
# of it is ESTALE (the object is gone).
answer b_t_a "ERR ESTALE"
expect_reject "a ghost: dcfs opens an object the backing filesystem lost" "an answer for something gone"
# A recycled inode number: the backing filesystem has an object under the
# recorded inode number's name now, but not the one recorded.
answer b_t_a "OK 0 77 100644"
expect_reject "a recycled backing inode (the backing handle opens another object)" "an answer for something gone"
answer b_t_a "OK 0 51 40755"
expect_reject "a backing handle that opens another type" "an answer for something gone"
echo "all checks passed"
