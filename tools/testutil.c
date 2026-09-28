/*
 * testutil - small helpers for the dcfs QEMU guest tests, for syscalls
 * busybox's own applets can't drive precisely enough:
 *
 *   - busybox `truncate -s N FILE` does open(O_WRONLY)+ftruncate(2), which
 *     dcfs currently refuses at open() (Setattr's own ftruncate is a
 *     separate step-4.4-independent path, but Open() still rejects any
 *     non-read-only open until step 4.4) -- so it never reaches
 *     Setattr(FUSE_SET_ATTR_SIZE) at all. `testutil truncate` calls
 *     truncate(2) directly (no open()), which is exactly the path a real
 *     NFS client or `truncate(1)` on most systems takes.
 *   - busybox `touch -d` cannot set nanosecond-precision timestamps.
 *   - busybox `chmod` has no -h/--no-dereference, so it can never target a
 *     symlink itself (chmod(2) always follows symlinks); confirming that
 *     Linux has no way to chmod a symlink's own mode (EOPNOTSUPP) needs
 *     fchmodat(2) with AT_SYMLINK_NOFOLLOW.
 *   - busybox has no setfattr/getfattr/fallocate applets at all.
 *
 * Subcommands:
 *   testutil truncate <path> <size>
 *       truncate(2) <path> to <size> bytes.
 *   testutil utimens <path> <sec> <nsec>
 *       utimensat(AT_FDCWD, <path>, {{sec,nsec},{sec,nsec}},
 *       AT_SYMLINK_NOFOLLOW): sets both atime and mtime to the same
 *       timestamp, without following a symlink.
 *   testutil lchmod <path> <octal-mode>
 *       fchmodat(AT_FDCWD, <path>, mode, AT_SYMLINK_NOFOLLOW): chmod
 *       without following a symlink. On Linux this always fails with
 *       EOPNOTSUPP when <path> is itself a symlink (there is no lchmod
 *       syscall) -- verified even on a plain tmpfs symlink, so this is
 *       generic kernel behavior, not something specific to dcfs.
 *   testutil rename2 <old> <new> <0|noreplace|exchange>
 *       renameat2(2) (via syscall(SYS_renameat2), both paths relative to
 *       AT_FDCWD) with flags 0, RENAME_NOREPLACE or RENAME_EXCHANGE.
 *       busybox has no way to ask for either flag, and busybox mv falls
 *       back to copy+delete on EXDEV, hiding the raw error.
 *   testutil setxattr <path> <name> <value>
 *       lsetxattr(2): sets extended attribute <name> to <value>, without
 *       following a symlink.
 *   testutil getxattr <path> <name>
 *       lgetxattr(2): prints <name>'s value (no trailing newline) without
 *       following a symlink.
 *   testutil listxattr <path>
 *       llistxattr(2): prints every xattr name, one per line, without
 *       following a symlink.
 *   testutil removexattr <path> <name>
 *       lremovexattr(2): removes xattr <name>, without following a
 *       symlink.
 *   testutil fallocate <path> <mode> <offset> <len>
 *       fallocate(2), where <mode> is "0", "keep_size" (FALLOC_FL_KEEP_SIZE)
 *       or "punch_hole" (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE --
 *       punching a hole always requires KEEP_SIZE).
 *   testutil writehold <path> <append|create> <nbytes>
 *       open(2)s <path> O_WRONLY|O_APPEND ("append") or
 *       O_WRONLY|O_CREAT|O_TRUNC, 0644 ("create"), writes <nbytes> bytes of
 *       'x' to it, prints "READY", and then sleeps forever WITHOUT closing
 *       the fd, until killed. A shell cannot do this: every way of writing
 *       to a file from the shell closes some descriptor for it afterwards
 *       (a child's exit, a builtin's redirection being undone), and on FUSE
 *       every such close sends a FLUSH -- this is how crash.sh gets writes
 *       the daemon has not been told about by any flush or release.
 *
 * Every subcommand prints "ERR <errno-name>" (via glibc's strerrorname_np)
 * and exits 1 on failure; on success it prints nothing (except
 * getxattr/listxattr, which print their result) and exits 0. All output is
 * single-line (or, for listxattr, one name per line) so the guest test
 * scripts can capture it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

static void print_err(int err)
{
	const char *name = strerrorname_np(err);

	printf("ERR %s\n", name ? name : "UNKNOWN");
}

static int cmd_truncate(const char *path, const char *size_str)
{
	off_t size = (off_t) strtoll(size_str, NULL, 10);

	if (truncate(path, size) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_utimens(
	const char *path, const char *sec_str, const char *nsec_str)
{
	struct timespec times[2];

	times[0].tv_sec = (time_t) strtoll(sec_str, NULL, 10);
	times[0].tv_nsec = (long) strtoll(nsec_str, NULL, 10);
	times[1] = times[0];

	if (utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_lchmod(const char *path, const char *mode_str)
{
	mode_t mode = (mode_t) strtol(mode_str, NULL, 8);

	if (fchmodat(AT_FDCWD, path, mode, AT_SYMLINK_NOFOLLOW) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_rename2(
	const char *oldpath, const char *newpath, const char *flags_str)
{
	unsigned int flags;

	if (strcmp(flags_str, "0") == 0) {
		flags = 0;
	} else if (strcmp(flags_str, "noreplace") == 0) {
		flags = RENAME_NOREPLACE;
	} else if (strcmp(flags_str, "exchange") == 0) {
		flags = RENAME_EXCHANGE;
	} else {
		fprintf(stderr, "testutil rename2: bad flags '%s'\n", flags_str);
		return 2;
	}
	if (syscall(SYS_renameat2, AT_FDCWD, oldpath, AT_FDCWD, newpath,
		    flags) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_setxattr(const char *path, const char *name, const char *value)
{
	if (lsetxattr(path, name, value, strlen(value), 0) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_getxattr(const char *path, const char *name)
{
	char buf[4096];
	ssize_t n;

	n = lgetxattr(path, name, buf, sizeof(buf) - 1);
	if (n == -1) {
		print_err(errno);
		return 1;
	}
	buf[n] = '\0';
	printf("%s", buf);
	return 0;
}

static int cmd_listxattr(const char *path)
{
	char buf[4096];
	ssize_t n, i;

	n = llistxattr(path, buf, sizeof(buf));
	if (n == -1) {
		print_err(errno);
		return 1;
	}
	for (i = 0; i < n; i += (ssize_t) strlen(buf + i) + 1)
		printf("%s\n", buf + i);
	return 0;
}

static int cmd_removexattr(const char *path, const char *name)
{
	if (lremovexattr(path, name) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_fallocate(
	const char *path, const char *mode_str, const char *offset_str,
	const char *len_str)
{
	int fd;
	int mode;
	off_t offset = (off_t) strtoll(offset_str, NULL, 10);
	off_t len = (off_t) strtoll(len_str, NULL, 10);

	if (strcmp(mode_str, "0") == 0) {
		mode = 0;
	} else if (strcmp(mode_str, "keep_size") == 0) {
		mode = FALLOC_FL_KEEP_SIZE;
	} else if (strcmp(mode_str, "punch_hole") == 0) {
		/* Punching a hole is only valid together with KEEP_SIZE. */
		mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
	} else {
		fprintf(stderr, "testutil fallocate: bad mode '%s'\n", mode_str);
		return 2;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	if (fallocate(fd, mode, offset, len) == -1) {
		print_err(errno);
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int cmd_writehold(
	const char *path, const char *how, const char *nbytes_str)
{
	int flags;
	long long remaining = strtoll(nbytes_str, NULL, 10);
	char buf[4096];
	int fd;

	if (strcmp(how, "append") == 0) {
		flags = O_WRONLY | O_APPEND;
	} else if (strcmp(how, "create") == 0) {
		flags = O_WRONLY | O_CREAT | O_TRUNC;
	} else {
		fprintf(stderr, "testutil writehold: bad mode '%s'\n", how);
		return 2;
	}
	fd = open(path, flags, 0644);
	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	memset(buf, 'x', sizeof(buf));
	while (remaining > 0) {
		size_t chunk = remaining < (long long) sizeof(buf)
			? (size_t) remaining : sizeof(buf);
		ssize_t n = write(fd, buf, chunk);

		if (n <= 0) {
			print_err(n == 0 ? EIO : errno);
			return 1;
		}
		remaining -= n;
	}
	printf("READY\n");
	fflush(stdout);
	for (;;)
		pause();
}

int main(int argc, char *argv[])
{
	if (argc == 4 && strcmp(argv[1], "truncate") == 0)
		return cmd_truncate(argv[2], argv[3]);
	if (argc == 5 && strcmp(argv[1], "utimens") == 0)
		return cmd_utimens(argv[2], argv[3], argv[4]);
	if (argc == 4 && strcmp(argv[1], "lchmod") == 0)
		return cmd_lchmod(argv[2], argv[3]);
	if (argc == 5 && strcmp(argv[1], "rename2") == 0)
		return cmd_rename2(argv[2], argv[3], argv[4]);
	if (argc == 5 && strcmp(argv[1], "setxattr") == 0)
		return cmd_setxattr(argv[2], argv[3], argv[4]);
	if (argc == 4 && strcmp(argv[1], "getxattr") == 0)
		return cmd_getxattr(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "listxattr") == 0)
		return cmd_listxattr(argv[2]);
	if (argc == 4 && strcmp(argv[1], "removexattr") == 0)
		return cmd_removexattr(argv[2], argv[3]);
	if (argc == 6 && strcmp(argv[1], "fallocate") == 0)
		return cmd_fallocate(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 5 && strcmp(argv[1], "writehold") == 0)
		return cmd_writehold(argv[2], argv[3], argv[4]);

	fprintf(stderr,
		"usage: testutil truncate <path> <size>\n"
		"       testutil utimens <path> <sec> <nsec>\n"
		"       testutil lchmod <path> <octal-mode>\n"
		"       testutil rename2 <old> <new> <0|noreplace|exchange>\n"
		"       testutil setxattr <path> <name> <value>\n"
		"       testutil getxattr <path> <name>\n"
		"       testutil listxattr <path>\n"
		"       testutil removexattr <path> <name>\n"
		"       testutil fallocate <path> <0|keep_size|punch_hole> <offset> <len>\n"
		"       testutil writehold <path> <append|create> <nbytes>\n");
	return 2;
}
