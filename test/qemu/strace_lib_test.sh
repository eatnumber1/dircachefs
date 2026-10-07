#!/bin/bash
# Self-check of the syscall-trace gate (step 26.3): the real
# guest/strace_lib.sh reducer and golden comparison over canned strace -f -y
# output. The reduction must classify each syscall (a backing path, the
# cache database, /dev/fuse, /proc, a descriptor of an object opened by
# handle, a path that is none of those), and a golden that differs from the
# observed trace must fail with the diff printed. Without this, a reducer
# that dropped everything would pass every golden of an empty trace.
#
# Usage: strace_lib_test.sh <lib.sh> <strace_lib.sh>
set -euo pipefail

LIB=$(readlink -f "$1")
STRACE_LIB=$(readlink -f "$2")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cat >"${WORK}/raw" <<'EOT'
520   read(8</dev/fuse>, "/\0\0\0\1\0\0\0X\0\0"..., 1052672) = 44
520   openat(3</src/t>, "new", O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0100666) = 10</src/t/new>
520   statx(10</src/t/new>, "", AT_EMPTY_PATH, STATX_ALL, {stx_mask=STATX_ALL, ...}) = 0
520   open_by_handle_at(3</src/t>, {handle_bytes=8, handle_type=1, f_handle="\x11"}, O_RDONLY|O_PATH) = 9</>
520   ioctl(9</>, FS_IOC_GETVERSION, 0x7ffd) = 0
520   openat(AT_FDCWD</>, "/proc/self/fd/9", O_RDONLY|O_CLOEXEC) = 11</>
520   openat(AT_FDCWD</>, "/proc/1118/task/1118/status", O_RDONLY) = 9</proc/1118/task/1118/status>
520   read(9</proc/1118/task/1118/status>, "Name:\tchmod\n"..., 1024) = 1024
520   fcntl(5</cache/dcfs.db>, F_SETLK, {l_type=F_RDLCK}) = 0
520   pwrite64(5</cache/dcfs.db-wal>, "\0\0", 8, 0) = 8
520   getxattr("/proc/self/fd/9", "security.capability", NULL, 0) = -1 ENODATA (No data available)
520   openat(AT_FDCWD</>, "/etc/passwd", O_RDONLY) = 12</etc/passwd>
520   statx(AT_FDCWD</>, "relative", 0, STATX_ALL, 0x7ffd) = -1 ENOENT (No such file or directory)
520   unlinkat(AT_FDCWD</>, "/src/t/victim", 0) = 0
520   write(8</dev/fuse>, "/x", 2) = 2
520   write(2</tmp/dcfs-2.log>, "I1007 sqlite3_step: BEGIN", 25) = 25
520   fsync(5</cache/dcfs.db-wal>) = 0
520   syncfs(3</src/t>)             = 0
520   mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f00
EOT

# shellcheck disable=SC1090
. "${LIB}" >/dev/null 2>&1 || true
FAILED=0
# shellcheck disable=SC1090
. "${STRACE_LIB}"

want='read(fuse)
openat(backing)
statx(backing)
open_by_handle_at(backing)
ioctl(backing)
openat(procfd)
openat(proc)
read(proc)
fcntl(cache)
pwrite64(cache)
getxattr(procfd) !ENODATA
openat(other)
statx(other) !ENOENT
unlinkat(backing)
write(fuse)
write(log)
fsync(cache)
syncfs(backing)'
got=$(strace_reduce /src/t /cache <"${WORK}/raw")
[[ "${got}" == "${want}" ]] || fail "reduction: got
${got}
want
${want}"
echo "PASS: every syscall is classified"

got=$(strace_reduce /src/t /cache <"${WORK}/raw" | strace_backing)
want='openat(backing)
statx(backing)
open_by_handle_at(backing)
ioctl(backing)
openat(procfd)
getxattr(procfd) !ENODATA
openat(other)
statx(other) !ENOENT
unlinkat(backing)
syncfs(backing)'
[[ "${got}" == "${want}" ]] || fail "golden lines: got
${got}
want
${want}"
echo "PASS: backing, procfd and other lines are kept, cache/proc/fuse dropped"

got=$(strace_reduce /src/t /cache <"${WORK}/raw" | strace_counts | tr '\n' ' ')
[[ "${got}" == "backing 6 procfd 2 other 2 cache 3 proc 2 fuse 2 log 1 sync 2 " ]] ||
  fail "counts: got '${got}'"
echo "PASS: counts per kind"

# A golden that differs from the observed trace fails, printing the diff.
export STRACE_DIR="${WORK}/strace"
mkdir "${STRACE_DIR}"
strace_reduce /src/t /cache <"${WORK}/raw" >"${STRACE_DIR}/op.all"
strace_backing <"${STRACE_DIR}/op.all" >"${STRACE_DIR}/op.trace"
strace_counts <"${STRACE_DIR}/op.all" >"${STRACE_DIR}/op.counts"
cp "${WORK}/raw" "${STRACE_DIR}/op.raw"

out=$(strace_golden same op <"${STRACE_DIR}/op.trace")
[[ "${out}" == "TEST same PASS" && "${FAILED}" -eq 0 ]] ||
  fail "identical golden: '${out}' FAILED=${FAILED}"
echo "PASS: an identical golden passes"

grep -v '^statx(backing)$' "${STRACE_DIR}/op.trace" >"${WORK}/golden"
out=$(strace_golden dropped op <"${WORK}/golden"; echo "FAILED=${FAILED}")
echo "${out}" | grep -q '^TEST dropped FAIL (backing syscalls differ' ||
  fail "differing golden did not fail: ${out}"
echo "${out}" | grep -q '^+statx(backing)$' ||
  fail "the diff does not show the missing line: ${out}"
# strace_golden ran in a subshell above; run it again here for FAILED.
strace_golden dropped op <"${WORK}/golden" >/dev/null || true
[[ "${FAILED}" -eq 1 ]] || fail "FAILED is ${FAILED} after a differing golden"
echo "PASS: a differing golden fails and prints the diff"

# Budgets: counts above a budget fail, naming both numbers; equal or below
# pass; an operation without a budget fails.
printf 'backing 6\nprocfd 2\nsync 2\nsql_stmts 10\n' >"${WORK}/counts"
printf '# comment\nop backing 6\nop procfd 2\nop sync 2\nop sql_stmts 10\nother backing 0\n' >"${WORK}/budgets"
strace_budget_compare "${WORK}/budgets" "${WORK}/counts" op ||
  fail "counts equal to the budgets were refused"
printf 'op backing 7\nop sync 3\nop sql_stmts 11\n' >"${WORK}/loose"
strace_budget_compare "${WORK}/loose" "${WORK}/counts" op ||
  fail "counts below the budgets were refused"
printf 'op backing 5\nop procfd 2\nop sync 2\nop sql_stmts 10\n' >"${WORK}/tight"
out=$(strace_budget_compare "${WORK}/tight" "${WORK}/counts" op) &&
  fail "a count above its budget passed"
[[ "${out}" == "op backing: count rose from 5 to 6; raising a budget is a deliberate edit of syscall_budgets.txt whose commit says why" ]] ||
  fail "budget message: '${out}'"
strace_budget_compare "${WORK}/budgets" "${WORK}/counts" nobudget >/dev/null &&
  fail "an operation without a budget passed"
echo "PASS: budgets fail above, pass at or below"
