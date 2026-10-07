# Backing-syscall traces of one operation (step 26.3), for the guest scripts
# that compare them with goldens (guest/syscall_traces.sh). Sourced after
# lib.sh; strace_reduce, strace_backing, strace_counts and strace_compare
# are plain functions of their input, so //test/qemu:strace_lib_test runs
# them on the host over canned strace output.
#
# Observe reality, not dcfs's own accounting (russ): strace is attached to
# the running daemon, the operation runs, and the daemon's syscalls are
# reduced to one line each:
#
#     name(kind)            or     name(kind) !ERRNO   when it failed
#
# where kind says where the syscall's path (the quoted path, else the fd
# strace -y decorates) lies:
#
#     backing   under the source directory ($SRC), or "/": strace -y cannot
#               name a descriptor of an object opened by handle
#               (open_by_handle_at gives a disconnected dentry, which
#               /proc/self/fd shows as "/"), and dcfs never opens "/"
#     cache     under the cache database's directory
#     procfd    /proc/<pid>/fd/N or /proc/self/fd/N: how dcfs reaches a
#               backing object through a descriptor, so a backing call
#     proc      other /proc
#     fuse      /dev/fuse
#     other     anything else: a finding, goldens never contain one
#
# The golden holds the backing, procfd and other lines in strace's order
# (one thread, so the order of completion). The per-kind counts of the whole
# trace (strace_counts) are what step 26.4 ratchets.

# The syscalls whose first argument is a descriptor and that name no path.
STRACE_FD_ONLY='read|write|readv|writev|pread64|pwrite64|close|fsync|fdatasync|fstat|fstatfs|ioctl|getdents64|getdents|copy_file_range|fallocate|ftruncate|fchmod|fchown|flistxattr|fgetxattr|fsetxattr|fremovexattr|lseek|syncfs|fcntl|flock|sendfile|splice|fadvise64|readahead|dup|dup2|dup3|mmap|msync'

# What strace traces: path syscalls, descriptor syscalls, the fstat family,
# and the calls that are not covered by those classes.
STRACE_TRACE='%file,%desc,%fstat,%fstatfs,ioctl,getdents64,copy_file_range,fallocate,fsync,fdatasync,syncfs'

# strace_reduce SRC CACHEDIR: strace -f -y -qq output on stdin; one
# name(kind)[ !ERRNO] line per syscall on stdout, every kind.
strace_reduce() {
	awk -v src="$1" -v cache="$2" -v fdonly="^($STRACE_FD_ONLY)\$" '
	function under(p, dir) {
		return p == dir || substr(p, 1, length(dir) + 1) == dir "/"
	}
	function classify(p) {
		if (under(p, src)) return "backing"
		if (p == "/") return "backing"
		if (under(p, cache)) return "cache"
		if (p ~ /^\/proc\/(self|[0-9]+)\/fd(\/|$)/) return "procfd"
		if (p ~ /^\/proc(\/|$)/) return "proc"
		if (p == "/dev/fuse") return "fuse"
		return "other"
	}
	{
		line = $0
		sub(/^\[pid +[0-9]+\] /, "", line)
		sub(/^[0-9]+ +/, "", line)
		if (match(line, /^[a-z_0-9]+\(/) == 0) next
		name = substr(line, 1, RLENGTH - 1)
		# The first argument, if it is a decorated descriptor (not
		# AT_FDCWD, whose decoration is the working directory).
		fdpath = ""
		if (match(line, /^[a-z_0-9]+\([0-9]+<[^>]*>/)) {
			head = substr(line, 1, RLENGTH)
			fdpath = substr(head, index(head, "<") + 1)
			fdpath = substr(fdpath, 1, length(fdpath) - 1)
		}
		quoted = ""
		if (match(line, /"([^"\\]|\\.)*"/))
			quoted = substr(line, RSTART + 1, RLENGTH - 2)
		path = ""
		if (name ~ fdonly) {
			path = fdpath
		} else if (substr(quoted, 1, 1) == "/") {
			path = quoted
		} else {
			path = fdpath
		}
		kind = (path == "") ? "other" : classify(path)
		err = ""
		if (match(line, / = -1 E[A-Z0-9]+/)) {
			err = substr(line, RSTART + 6, RLENGTH - 6)
			err = " !" err
		}
		print name "(" kind ")" err
	}'
}

# strace_backing: strace_reduce output on stdin; the lines of the golden.
strace_backing() {
	grep -E '^[a-z_0-9]+\((backing|procfd|other)\)'
}

# strace_counts: strace_reduce output on stdin; "kind count" per kind, in a
# fixed order (zero counts included), for step 26.4's ratchets.
strace_counts() {
	awk '
	{ k = $1; sub(/^[^(]*\(/, "", k); sub(/\).*/, "", k); n[k]++ }
	END {
		split("backing procfd other cache proc fuse", kinds, " ")
		for (i = 1; i <= 6; i++) print kinds[i], n[kinds[i]] + 0
	}'
}

# strace_compare GOLDEN ACTUAL: 0 when identical; else prints the unified
# diff (golden first) and returns 1.
strace_compare() {
	if command diff -u "$1" "$2" >/tmp/strace-compare.diff 2>&1; then
		rm -f /tmp/strace-compare.diff
		return 0
	fi
	cat /tmp/strace-compare.diff
	rm -f /tmp/strace-compare.diff
	return 1
}

STRACE_DIR=/tmp/strace

# strace_op NAME COMMAND...: with the daemon (DAEMON_PID, SRC and DB set)
# quiesced, attaches strace to it, runs COMMAND, waits for the daemon to go
# idle again, detaches, and writes $STRACE_DIR/NAME.{raw,all,trace,counts}.
# Returns COMMAND's exit status (1 if strace never attached). The command's
# own output goes to $STRACE_DIR/NAME.out.
strace_op() {
	so_name=$1
	shift
	mkdir -p "$STRACE_DIR"
	quiesce_daemon "$DAEMON_PID"
	rm -f "$STRACE_DIR/$so_name.raw"
	strace -f -y -qq -e "trace=$STRACE_TRACE" -o "$STRACE_DIR/$so_name.raw" \
		-p "$DAEMON_PID" 2>"$STRACE_DIR/$so_name.err" &
	so_pid=$!
	so_n=0
	while [ "$so_n" -lt 100 ]; do
		so_tracer=$(awk '/^TracerPid:/ {print $2}' "/proc/$DAEMON_PID/status")
		[ "$so_tracer" = "$so_pid" ] && break
		sleep 0.1
		so_n=$((so_n + 1))
	done
	if [ "$so_tracer" != "$so_pid" ]; then
		kill "$so_pid" 2>/dev/null || true
		wait "$so_pid" 2>/dev/null || true
		echo "strace_op $so_name: strace did not attach: $(cat "$STRACE_DIR/$so_name.err")"
		return 1
	fi
	if "$@" >"$STRACE_DIR/$so_name.out" 2>&1; then
		so_rc=0
	else
		so_rc=$?
	fi
	quiesce_daemon "$DAEMON_PID"
	kill -TERM "$so_pid" 2>/dev/null || true
	wait "$so_pid" 2>/dev/null || true
	strace_reduce "$SRC" "$(dirname "$DB")" <"$STRACE_DIR/$so_name.raw" >"$STRACE_DIR/$so_name.all"
	strace_backing <"$STRACE_DIR/$so_name.all" >"$STRACE_DIR/$so_name.trace"
	strace_counts <"$STRACE_DIR/$so_name.all" >"$STRACE_DIR/$so_name.counts"
	return "$so_rc"
}

# strace_golden CHECK NAME: compares NAME's reduced trace with the golden on
# stdin (a heredoc; empty for "no backing syscall"); reports TEST CHECK PASS,
# or FAIL with the diff and the counts.
strace_golden() {
	sg_check=$1
	sg_name=$2
	cat >"$STRACE_DIR/$sg_name.golden"
	if sg_diff=$(strace_compare "$STRACE_DIR/$sg_name.golden" "$STRACE_DIR/$sg_name.trace"); then
		pass "$sg_check"
	else
		fail "$sg_check" "backing syscalls differ from the golden (- golden, + observed); counts: $(tr '\n' ' ' <"$STRACE_DIR/$sg_name.counts")"
		echo "$sg_diff"
		echo "--- full trace of $sg_name ---"
		cat "$STRACE_DIR/$sg_name.all"
		echo "--- strace output of $sg_name ---"
		cat "$STRACE_DIR/$sg_name.raw"
	fi
}
