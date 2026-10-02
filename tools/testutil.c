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
 *   testutil lchown <path> <uid> <gid>
 *       fchownat(AT_FDCWD, <path>, uid, gid, AT_SYMLINK_NOFOLLOW); -1 for
 *       either leaves it unchanged (busybox chown cannot ask for -1/-1,
 *       which on Linux still updates ctime).
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
 *   testutil setxattrhex <path> <name> <hex>
 *   testutil getxattrhex <path> <name>
 *       As setxattr/getxattr, but the value is lowercase hex: for binary
 *       values such as a system.posix_acl_access ACL or a
 *       security.capability blob, which contain NUL bytes.
 *   testutil mmapwrite <path> <delay-seconds>
 *       open(2)s <path> O_RDWR, maps its first page MAP_SHARED, prints
 *       "MAPPED", sleeps <delay-seconds>, stores one byte through the
 *       mapping, msync(2)s it, prints "STORED", and then sleeps forever
 *       WITHOUT closing the fd or unmapping, until killed: a store the
 *       kernel never tells dcfs about (no write(2), no FLUSH).
 *   testutil fsfreeze <path> <freeze|thaw>
 *       FIFREEZE/FITHAW on the filesystem <path> is on: while frozen, every
 *       write to it (a rename, say) blocks until it is thawed -- a way to
 *       hold a dcfs mutation inside its backing syscall (busybox has no
 *       fsfreeze applet).
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
 *   testutil sqlite-lock <db-path> <hold-seconds>
 *       Opens <db-path> (dcfs's own cache database, e.g. /cache/dcfs.db)
 *       directly via libsqlite3, runs "BEGIN IMMEDIATE" to take the single
 *       writer lock (dcfs always turns on WAL mode -- see sqlite.cc's
 *       ConnectionFactory -- where this blocks any other connection's
 *       write transaction, including dcfs's own, with no write statement
 *       of its own needed), prints "READY", sleeps <hold-seconds>, then
 *       COMMITs (a no-op transaction) and exits. Used by release_leak.sh
 *       to force a real SQLITE_BUSY out of dcfs's own attribute-refresh
 *       write transaction (busy_timeout=5000 in sqlite.cc, so holding the
 *       lock longer than that guarantees dcfs's own retries are
 *       exhausted) -- fault injection no shell builtin or busybox applet
 *       can do, exactly like writehold above.
 *   testutil runas <uid> <gid> <groups> -- <cmd> [args...]
 *       Drops to <uid>/<gid> with supplementary groups <groups> (a
 *       comma-separated list of numeric gids, or "-" for none) --
 *       setgroups(2), setgid(2), setuid(2), in that order -- and execvp(3)s
 *       <cmd>. The guest has no /etc/passwd users and busybox su needs one;
 *       this needs none, and sets the exact group list a test wants. Used
 *       by credentials.sh to act on the dcfs mount as an unprivileged user.
 *       On failure prints "ERR <errno-name>" and exits 127 (execvp) or 1.
 *
 * Every subcommand prints "ERR <errno-name>" (via glibc's strerrorname_np)
 * and exits 1 on failure; on success it prints nothing (except
 * getxattr/getxattrhex/listxattr, which print their result) and exits 0. All output is
 * single-line (or, for listxattr, one name per line) so the guest test
 * scripts can capture it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

static int cmd_lchown(const char *path, const char *uid_str,
		      const char *gid_str)
{
	uid_t uid = (uid_t) strtol(uid_str, NULL, 10);
	gid_t gid = (gid_t) strtol(gid_str, NULL, 10);

	if (fchownat(AT_FDCWD, path, uid, gid, AT_SYMLINK_NOFOLLOW) == -1) {
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

static int cmd_setxattrhex(const char *path, const char *name, const char *hex)
{
	unsigned char buf[4096];
	size_t len = strlen(hex) / 2, i;

	if (strlen(hex) % 2 != 0 || len > sizeof(buf)) {
		fprintf(stderr, "testutil setxattrhex: bad hex value\n");
		return 2;
	}
	for (i = 0; i < len; i++) {
		unsigned int byte;

		if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
			fprintf(stderr, "testutil setxattrhex: bad hex value\n");
			return 2;
		}
		buf[i] = (unsigned char) byte;
	}
	if (lsetxattr(path, name, buf, len, 0) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_getxattrhex(const char *path, const char *name)
{
	unsigned char buf[4096];
	ssize_t n, i;

	n = lgetxattr(path, name, buf, sizeof(buf));
	if (n == -1) {
		print_err(errno);
		return 1;
	}
	for (i = 0; i < n; i++)
		printf("%02x", buf[i]);
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

static int cmd_mmapwrite(const char *path, const char *delay_str)
{
	int fd = open(path, O_RDWR);
	char *p;

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		print_err(errno);
		return 1;
	}
	printf("MAPPED\n");
	fflush(stdout);
	sleep((unsigned int) atoi(delay_str));
	p[0] = (char) (p[0] + 1);
	if (msync(p, 4096, MS_SYNC) == -1) {
		print_err(errno);
		return 1;
	}
	printf("STORED\n");
	fflush(stdout);
	for (;;)
		pause();
}

static int cmd_fsfreeze(const char *path, const char *how)
{
	unsigned long request;
	int fd;

	if (strcmp(how, "freeze") == 0) {
		request = FIFREEZE;
	} else if (strcmp(how, "thaw") == 0) {
		request = FITHAW;
	} else {
		fprintf(stderr, "testutil fsfreeze: bad mode '%s'\n", how);
		return 2;
	}
	fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	if (ioctl(fd, request, 0) == -1) {
		print_err(errno);
		close(fd);
		return 1;
	}
	close(fd);
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

static int cmd_sqlite_lock(const char *db_path, const char *seconds_str)
{
	sqlite3 *db;
	char *errmsg = NULL;
	int hold_seconds = atoi(seconds_str);
	int rc = sqlite3_open(db_path, &db);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "testutil sqlite-lock: open %s: %s\n", db_path,
			sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}
	/* BEGIN IMMEDIATE takes the single WAL writer lock right away,
	 * without needing an actual write statement to provoke it. */
	rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "testutil sqlite-lock: BEGIN IMMEDIATE: %s\n",
			errmsg ? errmsg : sqlite3_errmsg(db));
		sqlite3_free(errmsg);
		sqlite3_close(db);
		return 1;
	}
	printf("READY\n");
	fflush(stdout);
	sleep((unsigned int) hold_seconds);
	sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
	sqlite3_close(db);
	return 0;
}

static int cmd_runas(char *argv[])
{
	uid_t uid = (uid_t) strtoul(argv[2], NULL, 10);
	gid_t gid = (gid_t) strtoul(argv[3], NULL, 10);
	gid_t groups[64];
	size_t ngroups = 0;

	if (strcmp(argv[4], "-") != 0) {
		const char *p = argv[4];

		while (*p != '\0') {
			char *end;

			if (ngroups == sizeof(groups) / sizeof(groups[0])) {
				fprintf(stderr, "testutil runas: too many groups\n");
				return 1;
			}
			groups[ngroups++] = (gid_t) strtoul(p, &end, 10);
			if (end == p || (*end != ',' && *end != '\0')) {
				fprintf(stderr, "testutil runas: bad group list %s\n",
					argv[4]);
				return 1;
			}
			p = *end == ',' ? end + 1 : end;
		}
	}
	if (strcmp(argv[5], "--") != 0) {
		fprintf(stderr, "testutil runas: expected -- before the command\n");
		return 1;
	}
	if (setgroups(ngroups, groups) == -1 || setgid(gid) == -1 ||
	    setuid(uid) == -1) {
		print_err(errno);
		return 1;
	}
	execvp(argv[6], &argv[6]);
	print_err(errno);
	return 127;
}

int main(int argc, char *argv[])
{
	if (argc == 4 && strcmp(argv[1], "truncate") == 0)
		return cmd_truncate(argv[2], argv[3]);
	if (argc == 5 && strcmp(argv[1], "utimens") == 0)
		return cmd_utimens(argv[2], argv[3], argv[4]);
	if (argc == 4 && strcmp(argv[1], "lchmod") == 0)
		return cmd_lchmod(argv[2], argv[3]);
	if (argc == 5 && strcmp(argv[1], "lchown") == 0)
		return cmd_lchown(argv[2], argv[3], argv[4]);
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
	if (argc == 5 && strcmp(argv[1], "setxattrhex") == 0)
		return cmd_setxattrhex(argv[2], argv[3], argv[4]);
	if (argc == 4 && strcmp(argv[1], "getxattrhex") == 0)
		return cmd_getxattrhex(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "mmapwrite") == 0)
		return cmd_mmapwrite(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "fsfreeze") == 0)
		return cmd_fsfreeze(argv[2], argv[3]);
	if (argc == 6 && strcmp(argv[1], "fallocate") == 0)
		return cmd_fallocate(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 5 && strcmp(argv[1], "writehold") == 0)
		return cmd_writehold(argv[2], argv[3], argv[4]);
	if (argc == 4 && strcmp(argv[1], "sqlite-lock") == 0)
		return cmd_sqlite_lock(argv[2], argv[3]);
	if (argc >= 7 && strcmp(argv[1], "runas") == 0)
		return cmd_runas(argv);

	fprintf(stderr,
		"usage: testutil truncate <path> <size>\n"
		"       testutil utimens <path> <sec> <nsec>\n"
		"       testutil lchmod <path> <octal-mode>\n"
		"       testutil lchown <path> <uid> <gid>\n"
		"       testutil rename2 <old> <new> <0|noreplace|exchange>\n"
		"       testutil setxattr <path> <name> <value>\n"
		"       testutil getxattr <path> <name>\n"
		"       testutil listxattr <path>\n"
		"       testutil removexattr <path> <name>\n"
		"       testutil setxattrhex <path> <name> <hex>\n"
		"       testutil getxattrhex <path> <name>\n"
		"       testutil mmapwrite <path> <delay-seconds>\n"
		"       testutil fsfreeze <path> <freeze|thaw>\n"
		"       testutil fallocate <path> <0|keep_size|punch_hole> <offset> <len>\n"
		"       testutil writehold <path> <append|create> <nbytes>\n"
		"       testutil sqlite-lock <db-path> <hold-seconds>\n"
		"       testutil runas <uid> <gid> <gid,...|-> -- <cmd> [args...]\n");
	return 2;
}
