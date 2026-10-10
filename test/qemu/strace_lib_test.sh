#!/bin/bash
# SPDX-License-Identifier: MIT
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
520   poll([{fd=8</dev/fuse>, events=POLLIN}], 1, 0) = 0 (Timeout)
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
poll(fuse)
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
[[ "${got}" == "backing 6 procfd 2 other 2 cache 3 proc 2 fuse 3 log 1 sanitizer 0 sync 2 " ]] ||
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

# A tolerance (step 26.17: CPU time is not an exact count) is a percentage of
# the budget added to it: a count within it passes, one past it fails and the
# message names it. Integer arithmetic only (busybox awk has no surprises, but
# 10 * 1.15 is not 11.5 in a double); a tolerance of 0 is the exact comparison.
printf 'cpu cpu_us 1000\ncpu io_ops 10\n' >"${WORK}/tbudgets"
printf 'cpu_us 1150\nio_ops 10\n' >"${WORK}/tcounts"
strace_budget_compare "${WORK}/tbudgets" "${WORK}/tcounts" cpu cost.txt 15 ||
  fail "a count at budget plus tolerance was refused"
printf 'cpu_us 1151\nio_ops 10\n' >"${WORK}/tcounts"
out=$(strace_budget_compare "${WORK}/tbudgets" "${WORK}/tcounts" cpu cost.txt 15) &&
  fail "a count past budget plus tolerance passed"
[[ "${out}" == "cpu cpu_us: count rose from 1000 to 1151, past the 15% tolerance; raising a budget is a deliberate edit of cost.txt whose commit says why" ]] ||
  fail "tolerance message: '${out}'"
strace_budget_compare "${WORK}/tbudgets" "${WORK}/tcounts" cpu cost.txt 16 ||
  fail "a count within a wider tolerance was refused"
strace_budget_compare "${WORK}/tbudgets" "${WORK}/tcounts" cpu cost.txt 0 >/dev/null &&
  fail "a tolerance of 0 allowed a count above its budget"
echo "PASS: a tolerance allows that percentage above the budget and no more"

# The wall allowance a target's budgets need: 3 x (the CPU budget with its
# tolerance + reads and writes at 2 ms + flushes at 20 ms), in whole seconds,
# rounded up; 0 for a target without budgets or with budgets of no work.
printf 'a cpu_us 1000000\nb cpu_us 100000\nz cpu_us 0\n' >"${WORK}/ac"
printf 'a reads 100\na writes 50\na flushes 10\na sectors_read 99999999\nb flushes 1\nc reads 5\n' >"${WORK}/ai"
# a: 1 s + 1 s tolerance + 150 x 2 ms + 10 x 20 ms = 2.5 s -> x 3 = 7.5 -> 8
[[ "$(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" a)" == 8 ]] || fail "allowance of a: $(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" a)"
# b: 0.2 s + 0.02 s = 0.22 s -> 0.66 -> 1
[[ "$(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" b)" == 1 ]] || fail "allowance of b: $(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" b)"
# c has I/O and no CPU line; z has a CPU budget of zero and nothing else (no work: 0); d has nothing.
[[ "$(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" c)" == 1 ]] || fail "allowance of c: $(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" c)"
[[ "$(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" z)" == 0 ]] || fail "allowance of z"
[[ "$(cost_wall_allowance "${WORK}/ac" "${WORK}/ai" d)" == 0 ]] || fail "allowance of d"
# An exact number of seconds is not rounded up past itself: 1 s of CPU at a
# tolerance of 100% is 2 s, x 3 = 6.
printf 'e cpu_us 1000000\n' >"${WORK}/ae"
[[ "$(cost_wall_allowance "${WORK}/ae" /dev/null e)" == 6 ]] || fail "allowance of e: $(cost_wall_allowance "${WORK}/ae" /dev/null e)"
echo "PASS: the wall allowance is 3 x (CPU with its tolerance + I/Os x 2 ms + flushes x 20 ms), rounded up"

# The CPU budgets are for one kind of run (the plain build on KVM, quiet kernel):
# every other kind of boot line gives its reason, and that run is not held.
[[ -z "$(budget_unheld_reason "console=ttyS0 dcfs_accel=kvm dcfs_test=write.sh")" ]] ||
  fail "a plain KVM run has a reason not to be held to the budgets"
for flagged in "dcfs_accel=tcg:TCG" "dcfs_accel=kvm dcfs_noisy=1:noisy kernel" "dcfs_accel=kvm dcfs_sanitizer=asan:asan build" \
  "dcfs_accel=kvm dcfs_sanitizer=ubsan:ubsan build" "dcfs_accel=kvm dcfs_cov=/dev/vdb:coverage build"; do
  got=$(budget_unheld_reason "console=ttyS0 ${flagged%%:*} dcfs_test=write.sh")
  [[ "${got}" == "${flagged#*:}" ]] || fail "reason for '${flagged%%:*}': '${got}', want '${flagged#*:}'"
done
echo "PASS: a run is held to the CPU budgets only if it is a plain KVM run on the quiet kernel"

# (guest/lib.sh's fail, sourced above, replaced this file's own.)
die() {
  echo "FAIL: $*" >&2
  exit 1
}

# Sanitizer runtime syscalls (step 7.4): UBSan's vptr check probes whether a
# vtable prefix is readable by writing it to a pipe (compiler-rt's
# IsAccessibleMemoryRange: pipe2, fcntl F_GETFL, fcntl F_SETFL O_NONBLOCK,
# write of 16 bytes, close, close), once per new (static type, dynamic type)
# pair. Under DCFS_SANITIZER=ubsan exactly that sequence is kind "sanitizer",
# which the golden leaves out; without it, or for any other use of a pipe, the
# lines stay "other" and fail a golden.
cat >"${WORK}/probe" <<'EOT2'
520   read(8</dev/fuse>, "/\0\0\0\1\0\0\0X\0\0"..., 1052672) = 44
520   pipe2([11<pipe:[1358]>, 12<pipe:[1358]>], 0) = 0
520   fcntl(12<pipe:[1358]>, F_GETFL)   = 0x1 (flags O_WRONLY)
520   fcntl(12<pipe:[1358]>, F_SETFL, O_WRONLY|O_NONBLOCK) = 0
520   write(12<pipe:[1358]>, "\0\0\0\0\0\0\0\0 xq\323\263U\0\0", 16) = 16
520   close(11<pipe:[1358]>)            = 0
520   close(12<pipe:[1358]>)            = 0
520   openat(3</src/t>, "new", O_RDONLY) = 10</src/t/new>
EOT2
probe_want_plain='read(fuse)
pipe2(other)
fcntl(other)
fcntl(other)
write(other)
close(other)
close(other)
openat(backing)'
got=$(DCFS_SANITIZER= strace_reduce /src/t /cache <"${WORK}/probe")
[[ "${got}" == "${probe_want_plain}" ]] || die "probe without a sanitizer: got
${got}"
got=$(DCFS_SANITIZER=asan strace_reduce /src/t /cache <"${WORK}/probe")
[[ "${got}" == "${probe_want_plain}" ]] || die "probe under asan: got
${got}"
echo "PASS: the vptr probe is 'other' without UBSan"

got=$(DCFS_SANITIZER=ubsan strace_reduce /src/t /cache <"${WORK}/probe")
want='read(fuse)
pipe2(sanitizer)
fcntl(sanitizer)
fcntl(sanitizer)
write(sanitizer)
close(sanitizer)
close(sanitizer)
openat(backing)'
[[ "${got}" == "${want}" ]] || die "probe under ubsan: got
${got}"
got=$(DCFS_SANITIZER=ubsan strace_reduce /src/t /cache <"${WORK}/probe" | strace_backing)
[[ "${got}" == "openat(backing)" ]] || die "probe in the golden lines under ubsan: got
${got}"
got=$(DCFS_SANITIZER=ubsan strace_reduce /src/t /cache <"${WORK}/probe" | strace_counts | tr '\n' ' ')
[[ "${got}" == "backing 1 procfd 0 other 0 cache 0 proc 0 fuse 1 log 0 sanitizer 6 sync 0 " ]] ||
  die "counts under ubsan: got '${got}'"
echo "PASS: the vptr probe is 'sanitizer' under UBSan, left out of the golden, counted"

# Any other use of a pipe, under UBSan too, is still "other": a daemon that
# opens a pipe of its own, writes a different size, or leaves one open.
cat >"${WORK}/mine" <<'EOT2'
520   pipe2([11<pipe:[7]>, 12<pipe:[7]>], O_CLOEXEC) = 0
520   write(12<pipe:[7]>, "abcdefgh", 8) = 8
520   write(12<pipe:[7]>, "\0\0\0\0\0\0\0\0 xq\323\263U\0\0", 16) = 16
520   read(11<pipe:[7]>, "abcdefgh", 4096) = 8
520   fcntl(12<pipe:[7]>, F_SETFD, FD_CLOEXEC) = 0
520   close(11<pipe:[7]>)               = 0
EOT2
got=$(DCFS_SANITIZER=ubsan strace_reduce /src/t /cache <"${WORK}/mine" | tr '\n' ' ')
[[ "${got}" == "pipe2(other) write(other) write(other) read(other) fcntl(other) close(other) " ]] ||
  die "a daemon's own pipe under ubsan: got '${got}'"
echo "PASS: only the probe's exact sequence is 'sanitizer'; other pipe use stays 'other'"
