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
 *
 * Every subcommand prints "ERR <errno-name>" (via glibc's strerrorname_np)
 * and exits 1 on failure; on success it prints nothing and exits 0. All
 * output is single-line so the guest test scripts can capture it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

	fprintf(stderr,
		"usage: testutil truncate <path> <size>\n"
		"       testutil utimens <path> <sec> <nsec>\n"
		"       testutil lchmod <path> <octal-mode>\n"
		"       testutil rename2 <old> <new> <0|noreplace|exchange>\n");
	return 2;
}
