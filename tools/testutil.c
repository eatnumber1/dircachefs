/*
 * testutil - small helpers for the dcfs QEMU guest tests, for syscalls
 * busybox's own applets can't drive precisely enough:
 *
 *   - `testutil truncate` calls truncate(2) directly, with no open() in
 *     between -- the same syscall a real NFS client or `truncate(1)` on
 *     most systems issues, and distinct from busybox's own
 *     `truncate -s N FILE`, which goes through open(O_WRONLY)+ftruncate(2)
 *     instead.
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
 *   testutil utimes2 <path> <atime-sec> <mtime-sec>
 *       utimensat(2) with different access and modification times
 *       (whole seconds), following a symlink.
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
 *   testutil mmapwrite <path> <delay-seconds|usr1>
 *       open(2)s <path> O_RDWR, maps its first page MAP_SHARED, prints
 *       "MAPPED", sleeps <delay-seconds> (or, for "usr1", waits for a
 *       SIGUSR1: the caller does what it has to do between the mapping and
 *       the store, then signals, instead of guessing how long that takes),
 *       stores one byte through the mapping, msync(2)s it, prints "STORED",
 *       and then sleeps forever WITHOUT closing the fd or unmapping, until
 *       killed: a store the kernel never tells dcfs about (no write(2), no
 *       FLUSH).
 *   testutil mmapwrite-closed <path> <delay-seconds|usr1>
 *       As mmapwrite, but close(2)s the descriptor right after mmap(2),
 *       before printing "MAPPED": the store then comes after the last close
 *       (on dcfs, after the last RELEASE), through a mapping that holds only
 *       the backing file (step 23.1).
 *   testutil readnoatime <path>
 *       open(2)s <path> O_RDONLY | O_NOATIME, reads it to the end and closes
 *       it (busybox cannot ask for O_NOATIME).
 *   testutil mmapread <path>
 *       open(2)s <path> O_RDONLY, maps its first page MAP_PRIVATE, close(2)s
 *       the descriptor, reads one byte through the mapping and unmaps it: a
 *       read that is no read(2) (step 23.8).
 *   testutil removed-link <path>
 *       Holds <path> (a file or a directory) with an O_PATH descriptor,
 *       removes it (unlink(2) or rmdir(2)), and links it back as
 *       <path>.back through /proc/self/fd/<n> (linkat AT_SYMLINK_FOLLOW);
 *       prints "link=ok" or "link=ERR <errno-name>" (step 23.9).
 *   testutil tmpfile-link <dir> <name>
 *       open(2)s an O_TMPFILE file in <dir>, holds it with an O_PATH
 *       descriptor through /proc/self/fd, closes the file, and links it as
 *       <dir>/<name> through the O_PATH descriptor's magic link (linkat
 *       AT_SYMLINK_FOLLOW); prints "link=ok nlink=<n>" (the linked name's)
 *       or "link=ERR <errno-name>" (step 23.9).
 *   testutil heldstat <path>
 *       open(2)s <path> O_RDONLY, reads one byte, stat(2)s <path> while it
 *       is open, then closes it: one OPEN, one GETATTR, one FLUSH and one
 *       RELEASE through dcfs (a shell's redirections dup and close the
 *       descriptor several times, each close a FLUSH).
 *   testutil fsync <path>
 *       Opens a file or a directory read-only and fsync(2)s it (through dcfs:
 *       FUSE FSYNC or FSYNCDIR, after which dcfs runs a sync point), then
 *       closes it; prints "ERR <errno-name>" on failure.
 *   testutil syncfs <path>
 *       syncfs(2) on the filesystem <path> is on (what a dcfs sync point does
 *       to the backing filesystem); prints "ERR <errno-name>" on failure.
 *   testutil fsfreeze <path> <freeze|thaw>
 *       FIFREEZE/FITHAW on the filesystem <path> is on: while frozen, every
 *       write to it (a rename, say) blocks until it is thawed -- a way to
 *       hold a dcfs mutation inside its backing syscall (busybox has no
 *       fsfreeze applet).
 *   testutil shutdown <path> <default|logflush|nologflush>
 *       FS_IOC_SHUTDOWN (ext4's EXT4_IOC_SHUTDOWN, xfs's XFS_IOC_GOINGDOWN,
 *       btrfs's BTRFS_IOC_SHUTDOWN: one number) on the filesystem <path> is
 *       on, like xfstests' godown: from then on it fails every operation
 *       that reaches it, until it is unmounted and mounted again. "default"
 *       freezes it first (everything written so far is durable),
 *       "logflush" commits the journal but not data, "nologflush" commits
 *       nothing (what was not yet durable is lost, as in a power cut).
 *       btrfs has it from Linux 6.19, behind CONFIG_BTRFS_EXPERIMENTAL;
 *       elsewhere it fails with ENOTTY.
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
 *   testutil sql <db-path> <query>
 *       Opens dcfs's cache database read-only (a WAL reader: the daemon may
 *       be running, even blocked in a syscall) and prints the rows of one
 *       SELECT, columns separated by a tab, NULL as "NULL" (step 11.6: the
 *       state of a dentry and the dirty set while a mutation is held).
 *       Refuses a statement that is not read-only. Not opened `immutable` or
 *       with `nolock`: a reader must take part in the WAL's locking to see
 *       what phase 1 committed, which the daemon has not checkpointed. A
 *       reader that overlaps the daemon's sync point or its shutdown can make
 *       the TRUNCATE checkpoint of FinishRun see SQLITE_BUSY, so ask only
 *       while the daemon is held or idle; the busy timeout (2 s) covers a
 *       writer's brief lock.
 *   testutil sqlite-lock <db-path> <hold-seconds>
 *       Opens <db-path> (dcfs's own cache database, e.g. /cache/dcfs.db)
 *       directly via libsqlite3, runs "BEGIN IMMEDIATE" to take the single
 *       writer lock (dcfs always turns on WAL mode -- see sqlite.cc's
 *       ConnectionFactory -- where this blocks any other connection's
 *       write transaction, including dcfs's own, with no write statement
 *       of its own needed), prints "READY", sleeps <hold-seconds>, then
 *       COMMITs (a no-op transaction) and exits. Used by release_leak.sh
 *       to force a real SQLITE_BUSY out of dcfs's own attribute-refresh
 *       write transaction (dcfs sets no busy timeout, step 15.6b, so a write
 *       transaction that finds the lock held fails at once with SQLITE_BUSY)
 *       -- fault injection no shell builtin or busybox applet
 *       can do, exactly like writehold above.
 *   testutil runas <uid> <gid> <groups> -- <cmd> [args...]
 *       Drops to <uid>/<gid> with supplementary groups <groups> (a
 *       comma-separated list of numeric gids, or "-" for none) --
 *       setgroups(2), setgid(2), setuid(2), in that order -- and execvp(3)s
 *       <cmd>. The guest has no /etc/passwd users and busybox su needs one;
 *       this needs none, and sets the exact group list a test wants. Used
 *       by credentials.sh to act on the dcfs mount as an unprivileged user.
 *       On failure prints "ERR <errno-name>" and exits 127 (execvp) or 1.
 *   testutil waitmount <mountpoint> <present|absent> [pid]
 *       Waits, with no timeout and no polling interval, until /proc/self/mounts
 *       has (present) or no longer has (absent) an entry for <mountpoint>:
 *       the mount table is pollable (poll(2) reports POLLPRI|POLLERR when it
 *       changes), so the wait is the kernel's own event. With [pid], also
 *       watches that process through a pidfd and returns 1 when it has exited
 *       and the mount table is still not what was asked for (a daemon that
 *       refused to start). Exits 0 when the table is as asked; the caller
 *       cancels it. busybox has no such wait (guest/lib.sh start_daemon).
 *   testutil waitline <file> <text> [pid]
 *       Waits, with no timeout and no polling interval, until <file> contains
 *       <text> (a substring of what is written to it, e.g. a holder's
 *       "READY" or a line of the daemon's log): an inotify watch on the file's
 *       directory wakes it at each write, and the file is read again. With
 *       [pid] it also watches that process through a pidfd and returns 1 when
 *       it has exited without the text having appeared. The file need not
 *       exist yet. Exits 0 when the text is there.
 *   testutil opath-hold <path>
 *       open(2)s <path> O_PATH, prints "READY", and sleeps forever with it
 *       open, until killed: on a dcfs mount the kernel then keeps the
 *       file's inode (no FORGET) without any FUSE open (idle.sh).
 *   testutil opath-hold-tree <dir>
 *       open(2)s every regular file below <dir> O_PATH (raising its own
 *       RLIMIT_NOFILE to fs.nr_open first), prints "READY <n>", and sleeps
 *       forever with them open, until killed: the kernel keeps every one of
 *       those dcfs inodes (no FORGET) across drop_caches (destroy.sh).
 *   testutil opath-unlink-stat <path>
 *       open(2)s <path> O_PATH|O_NOFOLLOW, unlink(2)s <path>, then
 *       fstat(2)s the descriptor and prints "nlink=<n> size=<bytes>": an
 *       object that is removed but still referenced, without any open
 *       file the filesystem would see (busybox cannot open O_PATH).
 *   testutil rmcwd <dir>
 *       chdir(2)s into <dir>, rmdir(2)s <dir>, then prints the outcome of
 *       stat(".") ("stat=nlink:<n>"), open(".") and one getdents64 on it,
 *       one field each, "ERR <errno-name>" for a failure: a process whose
 *       working directory was removed.
 *   testutil unlinked-mutate <path>
 *   testutil opath-unlinked-mutate <path>
 *   testutil rmcwd-mutate <dir>
 *       Changes an object that is removed but still referenced, and prints
 *       one line: each step as "<step>=ok" or "<step>=<errno-name>", then
 *       the object's attributes. unlinked-mutate creates <path> (writing
 *       "hello"), keeps it open O_RDWR and unlinks it, then works through
 *       the descriptor (ftruncate, fchmod, fchown, futimens, f*xattr,
 *       fsync) and reopens it through /proc/self/fd. opath-unlinked-mutate
 *       does the same through an O_PATH descriptor, whose
 *       /proc/self/fd/<n> magic link the path-based calls follow (truncate,
 *       chmod, chown, utimensat, *xattr, and an open that writes and reads
 *       back). rmcwd-mutate chdir(2)s into <dir>, rmdir(2)s it, and changes
 *       "." (chmod, chown, utimensat, *xattr). Step 23.2: the removed-
 *       object records answer the kernel's changes too.
 *   testutil copyrange <src> <dst>
 *       copy_file_range(2) of all of <src> to the start of <dst> (created
 *       or truncated, mode 0644), looping until EOF; prints "copied=<n>".
 *   testutil clone <src> <dst>
 *       FICLONE: <dst> (created or truncated) becomes a reflink of <src>.
 *   testutil shared-extents <path>
 *       FS_IOC_FIEMAP: prints "shared=<n> extents=<m>", how many of the
 *       file's extents are shared (FIEMAP_EXTENT_SHARED: reflinked).
 *   testutil getflags <path>
 *   testutil setflags <path> <hex>
 *       FS_IOC_GETFLAGS (prints the flags in hex) / FS_IOC_SETFLAGS on an
 *       O_RDONLY descriptor (chattr/lsattr without the applets).
 *   testutil fsxattr <path>
 *       FS_IOC_FSGETXATTR: prints "xflags=<hex> extsize=<n> projid=<n>".
 *   testutil getversion <path>
 *       FS_IOC_GETVERSION: prints the inode generation.
 *   testutil ioctl-unknown <path>
 *       An ioctl dcfs does not forward (FS_IOC_GETFSLABEL on a file):
 *       prints its outcome.
 *   testutil tmpfile <dir> <name> <empty|proc|excl|none>
 *       open(<dir>, O_TMPFILE|O_RDWR, 0640), writes "tmp", and prints its
 *       nlink, then links it as <dir>/<name>: "empty" with linkat(fd, "",
 *       AT_EMPTY_PATH), "proc" with linkat of /proc/self/fd/<n>
 *       (AT_SYMLINK_FOLLOW), "excl" opens with O_EXCL (never linkable) and
 *       tries "empty", "none" does not link; prints each step's outcome and
 *       the linked file's nlink and size.
 *   testutil ext4-tune-casefold <path>
 *       Switches the casefold feature on under the mounted ext4 filesystem
 *       <path> is on (EXT4_IOC_SET_TUNE_SB_PARAM, Linux 6.18+). The kernel
 *       does not load the encoding then, so a directory made case-insensitive
 *       afterwards (chattr +F) oopses the next readdir: the reproducer of
 *       guest/casefold_tune_oops.sh (a kernel bug; no other test uses it).
 *   testutil readdir-ino <dir> [small-first]
 *       Lists <dir> with getdents64, printing "<name> <d_ino>" per entry,
 *       in whatever order the directory itself returns them -- which,
 *       off a plain (non-FUSE) directory, is not necessarily "." and ".."
 *       first: a backing filesystem is free to return its entries (dot
 *       entries included) in any order, e.g. hashed, and a caller
 *       comparing two listings for the same *set* of entries must sort
 *       before comparing (see guest/readdir_boundary.sh). With
 *       "small-first", the first call's buffer fits exactly one entry, so
 *       on a FUSE mount with readdirplus "auto" the rest comes from plain
 *       READDIR requests (the kernel uses READDIRPLUS only at offset 0),
 *       whose inode numbers busybox ls -i never shows separately -- this
 *       is for exercising dcfs's own READDIR path specifically, and is
 *       meaningless (and, since the first entry a plain filesystem
 *       returns can be any entry in the directory, actively unsafe --
 *       EINVAL if that entry's record does not fit the request) off a
 *       plain directory.
 *   testutil mkfiles <dir> <prefix> <suffix> <count>
 *       creates <count> empty files in <dir>, named <prefix>, a five-digit
 *       zero-padded index from 00000, then <suffix> (open(2) O_CREAT, mode
 *       0666 less the umask, like `: >file`). One process instead of a
 *       shell fork per file: guest/readdir_boundary.sh makes 7,500. Prints
 *       "ERR <errno name>" and stops at the first failure.
 *   testutil handle-save <path> <file>
 *       name_to_handle_at of <path>, written to <file> ("<type> <hex>").
 *   testutil handle-stat <dir> <file>
 *       open_by_handle_at (O_PATH) of the handle in <file>, against the
 *       mount <dir> is on, then fstat: prints "OK <size> <inode> <mode in
 *       octal>" or "ERR <errno name>" of whichever failed. guest/fault_lib.sh's
 *       identity oracle (step 26.14e) and guest/fault_power.sh's "born"
 *       scenario (step 23.11) reopen saved handles with it.
 *   testutil btrfs-subvol-create <path>
 *       BTRFS_IOC_SUBVOL_CREATE: creates a btrfs subvolume at <path> (whose
 *       parent directory must already exist on a btrfs filesystem). Step
 *       5.2's btrfs boundary-refusal check needs a subvolume boundary, and
 *       this minimal busybox initramfs has neither a `btrfs`(8) binary nor
 *       a busybox applet for it. The ioctl constants used are copied from
 *       the kernel's <linux/btrfs.h> UAPI header rather than included from
 *       it, since this toolchain's sysroot is not guaranteed to carry
 *       btrfs-specific UAPI headers.
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
#include <ftw.h>
#include <libgen.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

/* BTRFS_IOC_SUBVOL_CREATE, copied from the kernel's <linux/btrfs.h> UAPI
 * header (not included from it -- see the btrfs-subvol-create doc comment
 * above). BTRFS_PATH_NAME_MAX and the struct layout are stable UAPI, part
 * of the on-disk-adjacent ioctl ABI every btrfs-progs release also depends
 * on verbatim. */
#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_PATH_NAME_MAX 4087
struct btrfs_ioctl_vol_args {
	int64_t fd;
	char name[BTRFS_PATH_NAME_MAX + 1];
};
#define BTRFS_IOC_SUBVOL_CREATE \
	_IOW(BTRFS_IOCTL_MAGIC, 14, struct btrfs_ioctl_vol_args)

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

static int cmd_utimes2(const char *path, const char *atime_str,
		       const char *mtime_str)
{
	struct timespec times[2] = {
		{.tv_sec = (time_t) strtoll(atime_str, NULL, 10)},
		{.tv_sec = (time_t) strtoll(mtime_str, NULL, 10)},
	};

	if (utimensat(AT_FDCWD, path, times, 0) == -1) {
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

static int cmd_mmapwrite(const char *path, const char *delay_str,
			 int close_first)
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
	if (close_first && close(fd) == -1) {
		print_err(errno);
		return 1;
	}
	if (strcmp(delay_str, "usr1") == 0) {
		/* Blocked before MAPPED is printed, so a SIGUSR1 sent as soon
		 * as the caller has seen it cannot be lost. */
		sigset_t set;
		int sig;

		sigemptyset(&set);
		sigaddset(&set, SIGUSR1);
		sigprocmask(SIG_BLOCK, &set, NULL);
		printf("MAPPED\n");
		fflush(stdout);
		sigwait(&set, &sig);
	} else {
		printf("MAPPED\n");
		fflush(stdout);
		sleep((unsigned int) atoi(delay_str));
	}
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

static int cmd_readnoatime(const char *path)
{
	char buf[4096];
	ssize_t n;
	int fd = open(path, O_RDONLY | O_NOATIME);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		;
	if (n == -1) {
		print_err(errno);
		return 1;
	}
	close(fd);
	return 0;
}

static int cmd_mmapread(const char *path)
{
	volatile char *p;
	char c;
	int fd = open(path, O_RDONLY);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		print_err(errno);
		return 1;
	}
	if (close(fd) == -1) {
		print_err(errno);
		return 1;
	}
	c = p[0];
	if (munmap((void *) p, 4096) == -1) {
		print_err(errno);
		return 1;
	}
	printf("%d\n", (int) c);
	return 0;
}

static int cmd_removed_link(const char *path)
{
	char proc[64], back[4096];
	struct stat st;
	const char *name;
	int fd = open(path, O_PATH | O_NOFOLLOW);

	if (fd == -1 || fstat(fd, &st) == -1 ||
	    (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) == -1) {
		print_err(errno);
		return 1;
	}
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	snprintf(back, sizeof(back), "%s.back", path);
	if (linkat(AT_FDCWD, proc, AT_FDCWD, back, AT_SYMLINK_FOLLOW) == -1) {
		name = strerrorname_np(errno);
		printf("link=ERR %s\n", name ? name : "UNKNOWN");
	} else {
		printf("link=ok\n");
	}
	close(fd);
	return 0;
}

static int cmd_tmpfile_link(const char *dir, const char *name)
{
	char proc[64], path[4096];
	struct stat st;
	const char *err;
	int fd = open(dir, O_TMPFILE | O_RDWR, 0644), opath;

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	opath = open(proc, O_PATH);
	if (opath == -1 || close(fd) == -1) {
		print_err(errno);
		return 1;
	}
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", opath);
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (linkat(AT_FDCWD, proc, AT_FDCWD, path, AT_SYMLINK_FOLLOW) == -1) {
		err = strerrorname_np(errno);
		printf("link=ERR %s\n", err ? err : "UNKNOWN");
	} else if (stat(path, &st) == -1) {
		print_err(errno);
		return 1;
	} else {
		printf("link=ok nlink=%lu\n", (unsigned long) st.st_nlink);
	}
	close(opath);
	return 0;
}

static int cmd_heldstat(const char *path)
{
	struct stat st;
	char c;
	int fd = open(path, O_RDONLY);

	if (fd == -1 || read(fd, &c, 1) == -1 || stat(path, &st) == -1) {
		print_err(errno);
		return 1;
	}
	close(fd);
	return 0;
}

/* The event source for a process: a pidfd, readable once it has exited.
 * Returns -1 with *gone set when there is none to watch because it is already
 * gone (or no pid was given: *gone stays 0). */
static int watch_pid(const char *pid_str, int *gone)
{
	long pid = pid_str == NULL ? 0 : atol(pid_str);
	int fd;

	*gone = 0;
	if (pid <= 0)
		return -1;
	fd = (int) syscall(SYS_pidfd_open, (pid_t) pid, 0);
	if (fd == -1)
		*gone = 1;
	return fd;
}

/* Reads the whole of fd from its start into buf (NUL-terminated, truncated at
 * size - 1) and returns the length. */
static size_t slurp(int fd, char *buf, size_t size)
{
	size_t len = 0;

	if (lseek(fd, 0, SEEK_SET) == -1)
		return 0;
	while (len < size - 1) {
		ssize_t n = read(fd, buf + len, size - 1 - len);

		if (n <= 0)
			break;
		len += (size_t) n;
	}
	buf[len] = '\0';
	return len;
}

static int cmd_waitmount(const char *mountpoint, const char *mode,
			 const char *pid_str)
{
	static char table[1 << 20];
	char needle[PATH_MAX + 3];
	int want, gone, mfd, pfd;

	if (strcmp(mode, "present") == 0)
		want = 1;
	else if (strcmp(mode, "absent") == 0)
		want = 0;
	else {
		fprintf(stderr, "testutil waitmount: mode is present or absent\n");
		return 2;
	}
	if (snprintf(needle, sizeof(needle), " %s ", mountpoint) >=
	    (int) sizeof(needle)) {
		print_err(ENAMETOOLONG);
		return 2;
	}
	mfd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
	if (mfd == -1) {
		print_err(errno);
		return 2;
	}
	pfd = watch_pid(pid_str, &gone);
	for (;;) {
		struct pollfd fds[2];
		nfds_t n = 1;

		/* Read before polling: the read is what the kernel compares the
		 * table's next change with, so nothing is missed in between. */
		slurp(mfd, table, sizeof(table));
		if ((strstr(table, needle) != NULL) == want)
			return 0;
		if (gone)
			return 1;
		fds[0] = (struct pollfd){ .fd = mfd, .events = POLLPRI | POLLERR };
		if (pfd != -1)
			fds[n++] = (struct pollfd){ .fd = pfd, .events = POLLIN };
		if (poll(fds, n, -1) == -1 && errno != EINTR) {
			print_err(errno);
			return 2;
		}
		if (n == 2 && (fds[1].revents & POLLIN))
			gone = 1;
	}
}

static int cmd_waitline(const char *file, const char *text, const char *pid_str)
{
	static char contents[1 << 20];
	char *dir_copy = strdup(file);
	int ifd, gone, pfd;

	if (dir_copy == NULL)
		return 2;
	ifd = inotify_init1(IN_CLOEXEC);
	if (ifd == -1 ||
	    inotify_add_watch(ifd, dirname(dir_copy),
			      IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE |
				      IN_MOVED_TO) == -1) {
		print_err(errno);
		return 2;
	}
	pfd = watch_pid(pid_str, &gone);
	for (;;) {
		struct pollfd fds[2];
		nfds_t n = 1;
		int fd = open(file, O_RDONLY | O_CLOEXEC);

		/* The watch is in place before the first read, so a write after
		 * this read wakes the poll below. */
		if (fd != -1) {
			slurp(fd, contents, sizeof(contents));
			close(fd);
			if (strstr(contents, text) != NULL)
				return 0;
		}
		if (gone)
			return 1;
		fds[0] = (struct pollfd){ .fd = ifd, .events = POLLIN };
		if (pfd != -1)
			fds[n++] = (struct pollfd){ .fd = pfd, .events = POLLIN };
		if (poll(fds, n, -1) == -1 && errno != EINTR) {
			print_err(errno);
			return 2;
		}
		if (fds[0].revents & POLLIN) {
			char ev[4096];

			if (read(ifd, ev, sizeof(ev)) < 0 && errno != EINTR)
				return 2;
		}
		if (n == 2 && (fds[1].revents & POLLIN))
			gone = 1;
	}
}

static int cmd_fsync(const char *path)
{
	int fd = open(path, O_RDONLY);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	if (fsync(fd) == -1) {
		print_err(errno);
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int cmd_syncfs(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	if (syncfs(fd) == -1) {
		print_err(errno);
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
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

/* FS_IOC_SHUTDOWN and its flags, from the kernel's <linux/fs.h> UAPI
 * header (Linux 7.1), defined here when the sysroot's header predates
 * them; ext4 and xfs had the same number and flags under their own names
 * since 4.x. */
#ifndef FS_IOC_SHUTDOWN
#define FS_IOC_SHUTDOWN _IOR('X', 125, uint32_t)
#define FS_SHUTDOWN_FLAGS_DEFAULT 0x0
#define FS_SHUTDOWN_FLAGS_LOGFLUSH 0x1
#define FS_SHUTDOWN_FLAGS_NOLOGFLUSH 0x2
#endif

static int cmd_shutdown(const char *path, const char *how)
{
	uint32_t flags;
	int fd;

	if (strcmp(how, "default") == 0) {
		flags = FS_SHUTDOWN_FLAGS_DEFAULT;
	} else if (strcmp(how, "logflush") == 0) {
		flags = FS_SHUTDOWN_FLAGS_LOGFLUSH;
	} else if (strcmp(how, "nologflush") == 0) {
		flags = FS_SHUTDOWN_FLAGS_NOLOGFLUSH;
	} else {
		fprintf(stderr, "testutil shutdown: bad mode '%s'\n", how);
		return 2;
	}
	fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	if (ioctl(fd, FS_IOC_SHUTDOWN, &flags) == -1) {
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

static int cmd_sql(const char *db_path, const char *query)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
	int cols, i, step;

	if (rc != SQLITE_OK) {
		fprintf(stderr, "testutil sql: open %s: %s\n", db_path,
			sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}
	sqlite3_busy_timeout(db, 2000);
	rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "testutil sql: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}
	if (!sqlite3_stmt_readonly(stmt)) {
		fprintf(stderr, "testutil sql: refusing a statement that writes\n");
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return 1;
	}
	cols = sqlite3_column_count(stmt);
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		for (i = 0; i < cols; i++) {
			const unsigned char *text = sqlite3_column_text(stmt, i);

			printf("%s%s", i ? "\t" : "", text ? (const char *) text : "NULL");
		}
		printf("\n");
	}
	if (step != SQLITE_DONE)
		fprintf(stderr, "testutil sql: %s\n", sqlite3_errmsg(db));
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return step == SQLITE_DONE ? 0 : 1;
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

static int cmd_opath_hold(const char *path)
{
	int fd = open(path, O_PATH);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	printf("READY\n");
	fflush(stdout);
	for (;;)
		pause();
}

static long opath_held;

static int opath_hold_one(const char *path, const struct stat *st, int type,
			  struct FTW *ftw)
{
	(void) st;
	(void) ftw;
	if (type != FTW_F)
		return 0;
	if (open(path, O_PATH) == -1) {
		fprintf(stderr, "%s: ", path);
		print_err(errno);
		return 1;
	}
	opath_held++;
	return 0;
}

static int cmd_opath_hold_tree(const char *dir)
{
	struct rlimit limit;
	FILE *f = fopen("/proc/sys/fs/nr_open", "re");
	unsigned long long nr_open = 0;

	if (f != NULL) {
		if (fscanf(f, "%llu", &nr_open) != 1)
			nr_open = 0;
		fclose(f);
	}
	if (nr_open > 0) {
		limit.rlim_cur = limit.rlim_max = (rlim_t) nr_open;
		if (setrlimit(RLIMIT_NOFILE, &limit) == -1) {
			print_err(errno);
			return 1;
		}
	}
	/* FTW_PHYS: no symlinks followed; 64 directory descriptors at most. */
	if (nftw(dir, opath_hold_one, 64, FTW_PHYS) != 0)
		return 1;
	printf("READY %ld\n", opath_held);
	fflush(stdout);
	for (;;)
		pause();
}

static int cmd_opath_unlink_stat(const char *path)
{
	struct stat st;
	int fd = open(path, O_PATH | O_NOFOLLOW);

	if (fd == -1 || unlink(path) == -1 || fstat(fd, &st) == -1) {
		print_err(errno);
		return 1;
	}
	printf("nlink=%lu size=%lld\n", (unsigned long) st.st_nlink,
	       (long long) st.st_size);
	close(fd);
	return 0;
}

static int cmd_rmcwd(const char *dir)
{
	char buf[4096];
	struct stat st;
	const char *name;
	int fd;

	if (chdir(dir) == -1 || rmdir(dir) == -1) {
		print_err(errno);
		return 1;
	}
	if (stat(".", &st) == -1) {
		name = strerrorname_np(errno);
		printf("stat=ERR %s", name ? name : "UNKNOWN");
	} else {
		printf("stat=nlink:%lu", (unsigned long) st.st_nlink);
	}
	fd = open(".", O_RDONLY | O_DIRECTORY);
	if (fd == -1) {
		name = strerrorname_np(errno);
		printf(" open=ERR %s getdents=-\n", name ? name : "UNKNOWN");
		return 0;
	}
	if (syscall(SYS_getdents64, fd, buf, sizeof(buf)) == -1) {
		name = strerrorname_np(errno);
		printf(" open=ok getdents=ERR %s\n", name ? name : "UNKNOWN");
	} else {
		printf(" open=ok getdents=ok\n");
	}
	close(fd);
	return 0;
}

/* Appends " <step>=ok" or " <step>=<errno-name>" for a call that returned
 * `ret` (-1 on failure, errno set) to the line being printed. */
static void step(const char *name, int ret)
{
	const char *err;

	if (ret != -1) {
		printf(" %s=ok", name);
		return;
	}
	err = strerrorname_np(errno);
	printf(" %s=%s", name, err ? err : "UNKNOWN");
}

/* The attributes the mutate subcommands compare: what a local filesystem
 * and dcfs must agree on (no ctime: it is the moment of the change). */
static void print_stat(int ret, const struct stat *st, int with_times)
{
	if (ret == -1) {
		step("stat", ret);
		return;
	}
	printf(" stat=size:%lld,mode:%o,uid:%u,gid:%u,nlink:%lu",
	       (long long) st->st_size, (unsigned) st->st_mode,
	       (unsigned) st->st_uid, (unsigned) st->st_gid,
	       (unsigned long) st->st_nlink);
	if (with_times)
		printf(",atime:%lld.%09ld,mtime:%lld.%09ld",
		       (long long) st->st_atim.tv_sec, st->st_atim.tv_nsec,
		       (long long) st->st_mtim.tv_sec, st->st_mtim.tv_nsec);
}

/* The getxattr step: "getxattr=<value>" or the error. */
static void step_getxattr(ssize_t n, const char *value)
{
	if (n < 0) {
		step("getxattr", -1);
		return;
	}
	printf(" getxattr=%.*s", (int) n, value);
}

static const struct timespec kMutateTimes[2] = {
	{.tv_sec = 1000000000, .tv_nsec = 5},
	{.tv_sec = 1000000000, .tv_nsec = 7},
};

static int cmd_unlinked_mutate(const char *path)
{
	char proc[64], buf[64];
	struct stat st;
	ssize_t n;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644), fd2;

	if (fd == -1 || write(fd, "hello", 5) != 5 || unlink(path) == -1) {
		print_err(errno);
		return 1;
	}
	printf("fd");
	step("truncate", ftruncate(fd, 3));
	step("chmod", fchmod(fd, 0600));
	step("chown", fchown(fd, 1000, 1000));
	step("utimens", futimens(fd, kMutateTimes));
	step("setxattr", fsetxattr(fd, "user.dcfs", "v1", 2, 0));
	n = fgetxattr(fd, "user.dcfs", buf, sizeof(buf));
	step_getxattr(n, buf);
	step("removexattr", fremovexattr(fd, "user.dcfs"));
	n = fgetxattr(fd, "user.dcfs", buf, sizeof(buf));
	step_getxattr(n, buf);
	step("fsync", fsync(fd));
	print_stat(fstat(fd, &st), &st, 1);
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	fd2 = open(proc, O_RDONLY);
	step("reopen", fd2);
	if (fd2 != -1) {
		n = pread(fd2, buf, sizeof(buf), 0);
		printf(" read=%.*s", n < 0 ? 0 : (int) n, buf);
		close(fd2);
	}
	printf("\n");
	close(fd);
	return 0;
}

static int cmd_opath_unlinked_mutate(const char *path)
{
	char proc[64], buf[64];
	struct stat st;
	ssize_t n;
	int fd, fd2;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd == -1 || write(fd, "hello", 5) != 5 || close(fd) == -1) {
		print_err(errno);
		return 1;
	}
	fd = open(path, O_PATH | O_NOFOLLOW);
	if (fd == -1 || unlink(path) == -1) {
		print_err(errno);
		return 1;
	}
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	printf("opath");
	step("truncate", truncate(proc, 4));
	step("chmod", chmod(proc, 0640));
	step("chown", chown(proc, 1000, 1000));
	step("utimens", utimensat(AT_FDCWD, proc, kMutateTimes, 0));
	step("setxattr", setxattr(proc, "user.dcfs", "v2", 2, 0));
	n = getxattr(proc, "user.dcfs", buf, sizeof(buf));
	step_getxattr(n, buf);
	step("removexattr", removexattr(proc, "user.dcfs"));
	fd2 = open(proc, O_RDWR);
	step("reopen", fd2);
	if (fd2 != -1) {
		step("write", (int) pwrite(fd2, "J", 1, 0));
		n = pread(fd2, buf, sizeof(buf), 0);
		printf(" read=%.*s", n < 0 ? 0 : (int) n, buf);
		step("fsync", fsync(fd2));
		close(fd2);
	}
	/* The write after utimensat set mtime to now: no times. */
	print_stat(fstat(fd, &st), &st, 0);
	printf("\n");
	close(fd);
	return 0;
}

static int cmd_rmcwd_mutate(const char *dir)
{
	char buf[64];
	struct stat st;
	ssize_t n;

	if (chdir(dir) == -1 || rmdir(dir) == -1) {
		print_err(errno);
		return 1;
	}
	printf("cwd");
	step("chmod", chmod(".", 0700));
	step("chown", chown(".", 1000, 1000));
	step("utimens", utimensat(AT_FDCWD, ".", kMutateTimes, 0));
	step("setxattr", setxattr(".", "user.dcfs", "v3", 2, 0));
	n = getxattr(".", "user.dcfs", buf, sizeof(buf));
	step_getxattr(n, buf);
	step("removexattr", removexattr(".", "user.dcfs"));
	print_stat(stat(".", &st), &st, 1);
	printf("\n");
	return 0;
}

static int cmd_copyrange(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY), out;
	long long total = 0;

	if (in == -1) {
		print_err(errno);
		return 1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out == -1) {
		print_err(errno);
		return 1;
	}
	for (;;) {
		ssize_t n = copy_file_range(in, NULL, out, NULL, 1 << 30, 0);

		if (n == -1) {
			print_err(errno);
			return 1;
		}
		if (n == 0)
			break;
		total += n;
	}
	printf("copied=%lld\n", total);
	close(in);
	close(out);
	return 0;
}

static int cmd_clone(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY), out;

	if (in == -1) {
		print_err(errno);
		return 1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out == -1 || ioctl(out, FICLONE, in) == -1) {
		print_err(errno);
		return 1;
	}
	printf("cloned\n");
	return 0;
}

static int cmd_shared_extents(const char *path)
{
	union {
		struct fiemap map;
		char buf[sizeof(struct fiemap) +
			 64 * sizeof(struct fiemap_extent)];
	} u;
	unsigned int i, shared = 0;
	int fd = open(path, O_RDONLY);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	memset(&u, 0, sizeof(u));
	u.map.fm_length = ~0ULL;
	u.map.fm_flags = FIEMAP_FLAG_SYNC;
	u.map.fm_extent_count = 64;
	if (ioctl(fd, FS_IOC_FIEMAP, &u.map) == -1) {
		print_err(errno);
		return 1;
	}
	for (i = 0; i < u.map.fm_mapped_extents; i++)
		if (u.map.fm_extents[i].fe_flags & FIEMAP_EXTENT_SHARED)
			shared++;
	printf("shared=%u extents=%u\n", shared, u.map.fm_mapped_extents);
	return 0;
}

static int cmd_getflags(const char *path)
{
	int flags = 0, fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd == -1 || ioctl(fd, FS_IOC_GETFLAGS, &flags) == -1) {
		print_err(errno);
		return 1;
	}
	printf("%x\n", flags);
	return 0;
}

static int cmd_setflags(const char *path, const char *hex)
{
	int flags = (int) strtol(hex, NULL, 16);
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd == -1 || ioctl(fd, FS_IOC_SETFLAGS, &flags) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

static int cmd_fsxattr(const char *path)
{
	struct fsxattr fsx;
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	memset(&fsx, 0, sizeof(fsx));
	if (fd == -1 || ioctl(fd, FS_IOC_FSGETXATTR, &fsx) == -1) {
		print_err(errno);
		return 1;
	}
	printf("xflags=%x extsize=%u projid=%u\n", fsx.fsx_xflags,
	       fsx.fsx_extsize, fsx.fsx_projid);
	return 0;
}

static int cmd_getversion(const char *path)
{
	long gen = 0;
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd == -1 || ioctl(fd, FS_IOC_GETVERSION, &gen) == -1) {
		print_err(errno);
		return 1;
	}
	printf("%lu\n", (unsigned long) (uint32_t) gen);
	return 0;
}

static int cmd_ioctl_unknown(const char *path)
{
	char label[FSLABEL_MAX];
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	printf("ioctl");
	step("getfslabel", ioctl(fd, FS_IOC_GETFSLABEL, label));
	printf("\n");
	return 0;
}

static int cmd_tmpfile(const char *dir, const char *name, const char *how)
{
	char path[4096], proc[64];
	struct stat st;
	int excl = strcmp(how, "excl") == 0;
	int fd = open(dir, O_TMPFILE | O_RDWR | (excl ? O_EXCL : 0), 0640);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	printf("tmpfile");
	step("write", (int) write(fd, "tmp", 3));
	if (fstat(fd, &st) == 0)
		printf(" nlink=%lu mode=%o", (unsigned long) st.st_nlink,
		       (unsigned) (st.st_mode & 07777));
	if (strcmp(how, "proc") == 0)
		step("link", linkat(AT_FDCWD, proc, AT_FDCWD, path,
				    AT_SYMLINK_FOLLOW));
	else if (strcmp(how, "none") != 0)
		step("link", linkat(fd, "", AT_FDCWD, path, AT_EMPTY_PATH));
	if (fstat(fd, &st) == 0)
		printf(" nlink=%lu", (unsigned long) st.st_nlink);
	step("relink", linkat(fd, "", AT_FDCWD, path, AT_EMPTY_PATH));
	close(fd);
	if (stat(path, &st) == 0)
		printf(" linked=size:%lld,nlink:%lu,mode:%o",
		       (long long) st.st_size, (unsigned long) st.st_nlink,
		       (unsigned) (st.st_mode & 07777));
	else
		step("linked", -1);
	printf("\n");
	return 0;
}

/* EXT4_IOC_{GET,SET}_TUNE_SB_PARAM, copied from the kernel's
 * <linux/ext4.h> UAPI header (as the btrfs ioctl above): the sysroot's
 * headers predate it. */
struct ext4_tune_sb_params {
	uint32_t set_flags;
	uint32_t checkinterval;
	uint16_t errors_behavior;
	uint16_t mnt_count;
	uint16_t max_mnt_count;
	uint16_t raid_stride;
	uint64_t last_check_time;
	uint64_t reserved_blocks;
	uint64_t blocks_count;
	uint32_t default_mnt_opts;
	uint32_t reserved_uid;
	uint32_t reserved_gid;
	uint32_t raid_stripe_width;
	uint16_t encoding;
	uint16_t encoding_flags;
	uint8_t def_hash_alg;
	uint8_t pad_1;
	uint16_t pad_2;
	uint32_t feature_compat;
	uint32_t feature_incompat;
	uint32_t feature_ro_compat;
	uint32_t set_feature_compat_mask;
	uint32_t set_feature_incompat_mask;
	uint32_t set_feature_ro_compat_mask;
	uint32_t clear_feature_compat_mask;
	uint32_t clear_feature_incompat_mask;
	uint32_t clear_feature_ro_compat_mask;
	uint8_t mount_opts[64];
	uint8_t pad[68];
};
#define EXT4_IOC_GET_TUNE_SB_PARAM _IOR('f', 45, struct ext4_tune_sb_params)
#define EXT4_IOC_SET_TUNE_SB_PARAM _IOW('f', 46, struct ext4_tune_sb_params)
#define EXT4_TUNE_FL_EDIT_FEATURES 0x00004000
#define EXT4_FEATURE_INCOMPAT_CASEFOLD 0x20000

static int cmd_ext4_tune_casefold(const char *path)
{
	struct ext4_tune_sb_params params;
	int fd = open(path, O_RDONLY);

	memset(&params, 0, sizeof(params));
	if (fd == -1 || ioctl(fd, EXT4_IOC_GET_TUNE_SB_PARAM, &params) == -1) {
		print_err(errno);
		return 1;
	}
	params.set_flags = EXT4_TUNE_FL_EDIT_FEATURES;
	params.set_feature_compat_mask = 0;
	params.set_feature_incompat_mask = EXT4_FEATURE_INCOMPAT_CASEFOLD;
	params.set_feature_ro_compat_mask = 0;
	params.clear_feature_compat_mask = 0;
	params.clear_feature_incompat_mask = 0;
	params.clear_feature_ro_compat_mask = 0;
	if (ioctl(fd, EXT4_IOC_SET_TUNE_SB_PARAM, &params) == -1) {
		print_err(errno);
		return 1;
	}
	return 0;
}

struct linux_dirent64 {
	ino64_t d_ino;
	off64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

static int cmd_readdir_ino(const char *dir, int small_first)
{
	char buf[4096];
	size_t want = small_first ? 24 /* exactly one record */ : sizeof(buf);
	int fd = open(dir, O_RDONLY | O_DIRECTORY);

	if (fd == -1) {
		print_err(errno);
		return 1;
	}
	for (;;) {
		long n = syscall(SYS_getdents64, fd, buf, want);
		long pos;

		if (n == -1) {
			print_err(errno);
			return 1;
		}
		if (n == 0)
			break;
		for (pos = 0; pos < n;) {
			struct linux_dirent64 *d = (void *) (buf + pos);

			printf("%s %llu\n", d->d_name,
			       (unsigned long long) d->d_ino);
			pos += d->d_reclen;
		}
		want = sizeof(buf);
	}
	close(fd);
	return 0;
}

static int cmd_mkfiles(const char *dir, const char *prefix, const char *suffix,
		       const char *count_str)
{
	long long count = strtoll(count_str, NULL, 10);
	char name[PATH_MAX];

	for (long long i = 0; i < count; i++) {
		int fd;

		if (snprintf(name, sizeof(name), "%s/%s%05lld%s", dir, prefix,
			     i, suffix) >= (int) sizeof(name)) {
			print_err(ENAMETOOLONG);
			return 1;
		}
		fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd == -1) {
			print_err(errno);
			return 1;
		}
		close(fd);
	}
	return 0;
}

static int cmd_btrfs_subvol_create(const char *path)
{
	struct btrfs_ioctl_vol_args args;
	char *path_copy_dir, *path_copy_base, *dir, *base;
	int parent_fd;
	int ret = 0;

	path_copy_dir = strdup(path);
	path_copy_base = strdup(path);
	if (path_copy_dir == NULL || path_copy_base == NULL) {
		print_err(ENOMEM);
		free(path_copy_dir);
		free(path_copy_base);
		return 1;
	}
	dir = dirname(path_copy_dir);
	base = basename(path_copy_base);

	if (strlen(base) > BTRFS_PATH_NAME_MAX) {
		fprintf(stderr,
			"testutil btrfs-subvol-create: name too long\n");
		free(path_copy_dir);
		free(path_copy_base);
		return 2;
	}

	parent_fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (parent_fd == -1) {
		print_err(errno);
		free(path_copy_dir);
		free(path_copy_base);
		return 1;
	}

	memset(&args, 0, sizeof(args));
	strncpy(args.name, base, BTRFS_PATH_NAME_MAX);
	if (ioctl(parent_fd, BTRFS_IOC_SUBVOL_CREATE, &args) == -1) {
		print_err(errno);
		ret = 1;
	}
	close(parent_fd);
	free(path_copy_dir);
	free(path_copy_base);
	return ret;
}

/* ------------------------------------------------------------------------
 * names-*: the Phase 9 "file names are bytes" corpus helpers (see
 * guest/names.sh, guest/names_random.sh and docs/plan/phases/09-*.md).
 *
 *   testutil names-create <root>          build the corpus tree under <root>
 *                                         (an existing empty directory)
 *   testutil names-verify <root> [open]   check every object against the
 *                                         corpus; with "open" also checks an
 *                                         NFS-style handle round trip
 *   testutil names-handles-save <root> <file>
 *   testutil names-handles-open <root> <file>
 *   testutil names-remove <root>          unlink/rmdir the whole corpus tree
 *   testutil names-dump <root> [meta]     canonical hex dump of a tree: two
 *                                         trees with the same bytes dump the
 *                                         same; "meta" skips file contents
 *   testutil names-errs <root>            the errno of operations on the
 *                                         special names (".", "..", 256 bytes)
 *   testutil names-chain-create <root>    a directory chain deeper than
 *   testutil names-chain-check <root>     PATH_MAX in total, walked by fd
 *   testutil names-random <root> <seed> <count>
 *                                         <count> names of random bytes
 *
 * The corpus is described by the hazard classes of the phase file; the
 * generic/453 and generic/454 sets were written independently for dcfs, in
 * the spirit of those xfstests tests (Copyright (c) 2014 Oracle, GPL-2.0:
 * names and xattr names that render alike but are different bytes), without
 * copying any of their code.
 *
 * Every failure prints one "FAIL ..." line (names are printed in hex, never
 * raw) and the command exits 1 after checking everything.
 * ------------------------------------------------------------------------ */
#include <dirent.h>
#include <stdarg.h>

struct bytes {
	const char *p;
	size_t n;
};
#define B(s) { s, sizeof(s) - 1 }

static const struct bytes fixed_names[] = {
	/* Delimiters of our formats. */
	B("a\nb"), B("a\rb"), B("a\tb"), B("a b"), B(" lead"), B("trail "),
	B(" both "), B("#hash"), B("a,b"), B("a=b"), B("a:b"), B("a\\b"),
	B("a\"b"), B("it's"), B("\\040"), B("\\n"), B("%2F"), B("trail\\"),
	B("\n"), B("line\n"),
	/* Byte classes at their edges. */
	B("\x01"), B("\x1f"), B("\x7f"), B("\x80"), B("\xff"), B("a\x01" "b"),
	B("a\x1f" "b"), B("a\x7f" "b"), B("a\x80" "b"), B("a\xff" "b"),
	/* Text-decoding hazards. */
	B("\xbf"), B("trunc\xe2\x82"), B("\xc0\xaf"), B("a\xc0\xaf" "b"),
	B("\xed\xa0\x80"), B("caf\xc3\xa9"), B("Case"), B("case"), B("CASE"),
	/* Path-walk specials. */
	B("..."), B(".x"), B("x."), B(". "), B(" ."),
	/* Ordering and prefixes. */
	B("a"), B("a\x80"), B("a\xff"), B("aa"),
	/* Lengths: one byte (255 and 255-mid-character are added below). */
	B("x"),
	/* generic/453 style sets: NFC, NFD (and the compatibility forms) of one
	 * character; full-width and half-width; a ligature and its expansion;
	 * the fake slashes; emoji; box characters; bidi and invisible
	 * characters. */
	B("\xc3\x85"), B("A\xcc\x8a"), B("\xe2\x84\xab"), B("\xef\xbc\xa1"),
	B("A"), B("\xef\xbd\xb6"), B("\xe3\x82\xab"), B("\xef\xac\x81"),
	B("fi"), B("a\xe2\x88\x95" "b"), B("a\xe2\x81\x84" "b"),
	B("\xf0\x9f\x98\x80"),
	B("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"),
	B("\xe2\x94\x8c\xe2\x94\x80\n\xe2\x94\x82x\n\xe2\x94\x94\xe2\x94\x80"),
	B("evil\xe2\x80\xae" "gpj.exe"), B("a\xe2\x80\x8b" "b"), B("ab"),
	B("a\xe2\x80\x8d" "b"), B("\xef\xbb\xbf" "bom"),
};
#define N_FIXED (sizeof(fixed_names) / sizeof(fixed_names[0]))

static struct bytes corpus[N_FIXED + 2];
static size_t ncorpus;
static char long255[256], long255mid[256];
#define LONGLINK "long-target" /* the 4095-byte symlink target */
#define LONGLINK_LEN 4095
#define LONGLINK_LEN_SHORT 1023 /* the most xfs allows */
#define SHORT_XATTR_NAME 40

static int nfail;

static void failf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void failf(const char *fmt, ...)
{
	va_list ap;

	if (++nfail > 60)
		return;
	printf("FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* Hex of a byte string, in one of four rotating buffers. */
static const char *hexs(const void *p, size_t n)
{
	static char bufs[4][2 * 8200 + 1];
	static int cur;
	char *out = bufs[cur++ % 4];
	size_t i;

	if (n > 8200)
		n = 8200;
	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", ((const unsigned char *) p)[i]);
	out[2 * n] = '\0';
	return out;
}

static void corpus_init(void)
{
	size_t i;

	memset(long255, 'n', 255);
	memset(long255mid, 'm', 253);
	long255mid[253] = '\xe2';
	long255mid[254] = '\x82';
	for (i = 0; i < N_FIXED; i++)
		corpus[ncorpus++] = fixed_names[i];
	corpus[ncorpus].p = long255;
	corpus[ncorpus++].n = 255;
	corpus[ncorpus].p = long255mid;
	corpus[ncorpus++].n = 255;
}

static void bpath(char *out, size_t cap, const char *root, const char *sub,
		  struct bytes name)
{
	size_t n = (size_t) snprintf(out, cap, "%s/%s", root, sub);

	if (name.p != NULL) {
		out[n++] = '/';
		memcpy(out + n, name.p, name.n);
		n += name.n;
	}
	out[n] = '\0';
}

static int cmp_bytes(const void *a, const void *b)
{
	const struct bytes *x = a, *y = b;
	size_t m = x->n < y->n ? x->n : y->n;
	int c = memcmp(x->p, y->p, m);

	if (c != 0)
		return c;
	return x->n < y->n ? -1 : x->n > y->n;
}

/* The entries of a directory (not "." / ".."), sorted by bytes. */
struct dlist {
	struct bytes *e;
	size_t n;
	int dot, dotdot;
	int err;
};

static void dlist_free(struct dlist *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free((void *) l->e[i].p);
	free(l->e);
}

static struct dlist dlist_read(const char *path)
{
	struct dlist l = { NULL, 0, 0, 0, 0 };
	DIR *d = opendir(path);
	struct dirent *de;
	size_t cap = 0;

	if (d == NULL) {
		l.err = errno;
		return l;
	}
	while ((de = readdir(d)) != NULL) {
		size_t n = strlen(de->d_name);

		if (strcmp(de->d_name, ".") == 0) {
			l.dot++;
			continue;
		}
		if (strcmp(de->d_name, "..") == 0) {
			l.dotdot++;
			continue;
		}
		if (l.n == cap) {
			cap = cap ? 2 * cap : 64;
			l.e = realloc(l.e, cap * sizeof(*l.e));
		}
		l.e[l.n].p = memcpy(malloc(n + 1), de->d_name, n + 1);
		l.e[l.n++].n = n;
	}
	closedir(d);
	qsort(l.e, l.n, sizeof(*l.e), cmp_bytes);
	return l;
}

/* The directory at path lists exactly want[0..nwant) (any order). */
static void check_listing(const char *path, const struct bytes *want,
			  size_t nwant)
{
	struct bytes *w = malloc((nwant + 1) * sizeof(*w));
	struct dlist l = dlist_read(path);
	size_t i;

	memcpy(w, want, nwant * sizeof(*w));
	qsort(w, nwant, sizeof(*w), cmp_bytes);
	if (l.err) {
		failf("opendir %s: errno %d", path, l.err);
	} else {
		if (l.dot != 1 || l.dotdot != 1)
			failf("listing of %s: . x%d .. x%d", path, l.dot,
			      l.dotdot);
		if (l.n != nwant)
			failf("listing of %s: %zu entries, want %zu", path,
			      l.n, nwant);
		for (i = 0; i < l.n && i < nwant; i++)
			if (cmp_bytes(&l.e[i], &w[i]) != 0) {
				failf("listing of %s: entry %zu is %s, want "
				      "%s",
				      path, i, hexs(l.e[i].p, l.e[i].n),
				      hexs(w[i].p, w[i].n));
				break;
			}
	}
	dlist_free(&l);
	free(w);
}

static size_t content_f(size_t i, char *out, size_t cap)
{
	return (size_t) snprintf(out, cap, "f:%zu\n", i);
}

static size_t target_for(size_t i, char *out)
{
	static const char *const suffix[] = { "", "/", "//x", "/./", "/.." };
	const char *s = suffix[i % 5];

	memcpy(out, corpus[i].p, corpus[i].n);
	memcpy(out + corpus[i].n, s, strlen(s));
	return corpus[i].n + strlen(s);
}

static void long_target(char *out)
{
	size_t k;

	for (k = 0; k < LONGLINK_LEN; k++)
		out[k] = (char) ((k * 7 + 1) % 255 + 1);
}

/* "user." plus the name; 255-byte names are cut so the xattr name fits in
 * XATTR_NAME_MAX (the two cut names still differ). */
static size_t xattr_name(size_t i, char *out)
{
	size_t n = corpus[i].n > 250 ? 250 : corpus[i].n;

	memcpy(out, "user.", 5);
	memcpy(out + 5, corpus[i].p, n);
	return 5 + n;
}

static const char *xattr_file(size_t i)
{
	return corpus[i].n > SHORT_XATTR_NAME ? "x2" : "x1";
}

static void xattr_value(size_t i, char *out)
{
	out[0] = '\0';
	out[1] = (char) i;
	out[2] = '\xff';
	out[3] = '\0';
	out[4] = 'v';
}

static int write_file(const char *path, const void *data, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);

	if (fd == -1)
		return -1;
	if (write(fd, data, n) != (ssize_t) n) {
		close(fd);
		return -1;
	}
	return close(fd);
}

static int read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd == -1)
		return -1;
	n = read(fd, buf, cap);
	close(fd);
	return (int) n;
}

static const char *const corpus_dirs[] = { "f", "d", "l", "h", "r", "rd" };

static int cmd_names_create(const char *root)
{
	char path[8192], path2[8192], buf[8192], val[5], xn[300];
	size_t i, n;

	corpus_init();
	for (i = 0; i < sizeof(corpus_dirs) / sizeof(corpus_dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, corpus_dirs[i]);
		if (mkdir(path, 0755) == -1)
			failf("mkdir %s: errno %d", path, errno);
	}
	for (i = 0; i < ncorpus; i++) {
		bpath(path, sizeof(path), root, "f", corpus[i]);
		n = content_f(i, buf, sizeof(buf));
		if (write_file(path, buf, n) == -1)
			failf("create file %s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);

		bpath(path, sizeof(path), root, "d", corpus[i]);
		if (mkdir(path, 0755) == -1) {
			failf("mkdir %s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
		} else {
			strcat(path, "/in");
			if (write_file(path, "in", 2) == -1)
				failf("create in/ of %s: errno %d",
				      hexs(corpus[i].p, corpus[i].n), errno);
		}

		bpath(path, sizeof(path), root, "l", corpus[i]);
		n = target_for(i, buf);
		buf[n] = '\0';
		if (symlink(buf, path) == -1)
			failf("symlink %s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
	}
	/* The long symlink target (4095 bytes). */
	long_target(buf);
	buf[LONGLINK_LEN] = '\0';
	snprintf(path, sizeof(path), "%s/l/" LONGLINK, root);
	{
		int r = symlink(buf, path);

		if (r == -1 && errno == ENAMETOOLONG) {
			/* xfs keeps symlink targets under 1024 bytes. */
			buf[LONGLINK_LEN_SHORT] = '\0';
			r = symlink(buf, path);
		}
		if (r == -1)
			failf("symlink %s: errno %d", LONGLINK, errno);
	}

	/* Hard links: h/c[i+1] is f/c[i]. */
	for (i = 0; i < ncorpus; i++) {
		bpath(path, sizeof(path), root, "f", corpus[i]);
		bpath(path2, sizeof(path2), root, "h",
		      corpus[(i + 1) % ncorpus]);
		if (link(path, path2) == -1)
			failf("link %s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
	}

	/* xattrs: x1 holds the short names, x2 the long ones. */
	snprintf(path, sizeof(path), "%s/x1", root);
	write_file(path, "x", 1);
	snprintf(path, sizeof(path), "%s/x2", root);
	write_file(path, "x", 1);
	for (i = 0; i < ncorpus; i++) {
		size_t xl = xattr_name(i, xn);

		xn[xl] = '\0';
		snprintf(path, sizeof(path), "%s/%s", root, xattr_file(i));
		xattr_value(i, val);
		if (lsetxattr(path, xn, val, sizeof(val), XATTR_CREATE) == -1)
			failf("setxattr %s: errno %d", hexs(xn, xl), errno);
	}

	/* Renames chain each name into the next: n renames, not n^2. */
	bpath(path, sizeof(path), root, "r", corpus[0]);
	n = content_f(0, buf, sizeof(buf));
	if (write_file(path, buf, n) == -1)
		failf("create r/%s: errno %d", hexs(corpus[0].p, corpus[0].n),
		      errno);
	bpath(path, sizeof(path), root, "rd", corpus[0]);
	if (mkdir(path, 0755) == -1)
		failf("mkdir rd/%s: errno %d", hexs(corpus[0].p, corpus[0].n),
		      errno);
	strcat(path, "/in");
	write_file(path, "in", 2);
	for (i = 0; i + 1 < ncorpus; i++) {
		bpath(path, sizeof(path), root, "r", corpus[i]);
		bpath(path2, sizeof(path2), root, "r", corpus[i + 1]);
		if (rename(path, path2) == -1)
			failf("rename r %s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
		bpath(path, sizeof(path), root, "rd", corpus[i]);
		bpath(path2, sizeof(path2), root, "rd", corpus[i + 1]);
		if (rename(path, path2) == -1)
			failf("rename rd %s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
	}
	if (nfail)
		printf("names-create: %d failures\n", nfail);
	return nfail != 0;
}

/* ---- handles ---- */

union fhbuf {
	struct file_handle h;
	unsigned char bytes[sizeof(struct file_handle) + 128];
};

static int get_handle(const char *path, union fhbuf *fh)
{
	int mount_id;

	memset(fh, 0, sizeof(*fh));
	fh->h.handle_bytes = 128;
	return name_to_handle_at(AT_FDCWD, path, &fh->h, &mount_id, 0);
}

/* kind: 'f' file, 'd' directory, 'l' symlink; i indexes the corpus. */
static void check_handle(int mfd, char kind, size_t i, union fhbuf *fh)
{
	char buf[8192], want[8192];
	int fd;
	ssize_t n;

	if (kind == 'l') {
		fd = open_by_handle_at(mfd, &fh->h, O_PATH | O_NOFOLLOW);
		if (fd == -1) {
			failf("open_by_handle_at l %zu: errno %d", i, errno);
			return;
		}
		n = readlinkat(fd, "", buf, sizeof(buf));
		close(fd);
		if (n != (ssize_t) target_for(i, want) ||
		    memcmp(buf, want, (size_t) n) != 0)
			failf("handle readlink of %s differs",
			      hexs(corpus[i].p, corpus[i].n));
	} else if (kind == 'd') {
		fd = open_by_handle_at(mfd, &fh->h, O_RDONLY | O_DIRECTORY);
		if (fd == -1) {
			failf("open_by_handle_at d %zu: errno %d", i, errno);
			return;
		}
		n = openat(fd, "in", O_RDONLY);
		close(fd);
		if (n == -1)
			failf("handle of dir %s: no in/ (errno %d)",
			      hexs(corpus[i].p, corpus[i].n), errno);
		else
			close((int) n);
	} else {
		fd = open_by_handle_at(mfd, &fh->h, O_RDONLY);
		if (fd == -1) {
			failf("open_by_handle_at f %zu: errno %d", i, errno);
			return;
		}
		n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n != (ssize_t) content_f(i, want, sizeof(want)) ||
		    memcmp(buf, want, (size_t) n) != 0)
			failf("handle content of %s differs",
			      hexs(corpus[i].p, corpus[i].n));
	}
}

static const char *kind_dir(char kind)
{
	return kind == 'f' ? "f" : kind == 'd' ? "d" : "l";
}

static int cmd_handle_save(const char *path, const char *file)
{
	union fhbuf fh;
	FILE *out;

	if (get_handle(path, &fh) == -1) {
		printf("ERR %s\n", strerrorname_np(errno));
		return 1;
	}
	out = fopen(file, "w");
	if (out == NULL) {
		printf("ERR %s\n", strerrorname_np(errno));
		return 1;
	}
	fprintf(out, "%d %s\n", fh.h.handle_type,
		hexs(fh.h.f_handle, fh.h.handle_bytes));
	fclose(out);
	return 0;
}

static int cmd_handle_stat(const char *dir, const char *file)
{
	char hex[512];
	union fhbuf fh;
	struct stat st;
	FILE *in = fopen(file, "r");
	int mfd, fd, type;
	size_t j;

	if (in == NULL || fscanf(in, "%d %511s", &type, hex) != 2) {
		printf("ERR bad handle file\n");
		return 1;
	}
	fclose(in);
	/* fhbuf holds 128 handle bytes. */
	if (strlen(hex) > 2 * 128 || strlen(hex) % 2 != 0) {
		printf("ERR bad handle file\n");
		return 1;
	}
	memset(&fh, 0, sizeof(fh));
	fh.h.handle_type = type;
	fh.h.handle_bytes = (unsigned) strlen(hex) / 2;
	for (j = 0; j < fh.h.handle_bytes; j++) {
		unsigned int v;

		sscanf(hex + 2 * j, "%2x", &v);
		fh.h.f_handle[j] = (unsigned char) v;
	}
	mfd = open(dir, O_RDONLY | O_DIRECTORY);
	if (mfd == -1) {
		printf("ERR %s\n", strerrorname_np(errno));
		return 1;
	}
	fd = open_by_handle_at(mfd, &fh.h, O_PATH | O_NOFOLLOW);
	if (fd == -1 || fstat(fd, &st) == -1) {
		printf("ERR %s\n", strerrorname_np(errno));
	} else {
		printf("OK %lld %llu %o\n", (long long) st.st_size,
		       (unsigned long long) st.st_ino, (unsigned) st.st_mode);
	}
	if (fd != -1)
		close(fd);
	close(mfd);
	return 0;
}

static int cmd_names_handles_save(const char *root, const char *file)
{
	static const char kinds[] = "fdl";
	char path[8192];
	union fhbuf fh;
	FILE *out = fopen(file, "w");
	size_t i, k;

	corpus_init();
	if (out == NULL) {
		printf("ERR %s\n", strerrorname_np(errno));
		return 1;
	}
	for (k = 0; k < 3; k++)
		for (i = 0; i < ncorpus; i++) {
			bpath(path, sizeof(path), root, kind_dir(kinds[k]),
			      corpus[i]);
			if (get_handle(path, &fh) == -1) {
				failf("name_to_handle_at %c %zu: errno %d",
				      kinds[k], i, errno);
				continue;
			}
			fprintf(out, "%c %zu %d %s\n", kinds[k], i,
				fh.h.handle_type,
				hexs(fh.h.f_handle, fh.h.handle_bytes));
		}
	fclose(out);
	return nfail != 0;
}

static int cmd_names_handles_open(const char *root, const char *file)
{
	char line[1024], hex[512];
	FILE *in = fopen(file, "r");
	int mfd = open(root, O_RDONLY | O_DIRECTORY);
	char kind;
	size_t i;
	int type;

	corpus_init();
	if (in == NULL || mfd == -1) {
		printf("ERR %s\n", strerrorname_np(errno));
		return 1;
	}
	while (fgets(line, sizeof(line), in) != NULL) {
		union fhbuf fh;
		size_t j;

		if (sscanf(line, "%c %zu %d %511s", &kind, &i, &type, hex) !=
		    4) {
			failf("bad handle line");
			continue;
		}
		memset(&fh, 0, sizeof(fh));
		fh.h.handle_type = type;
		fh.h.handle_bytes = (unsigned) strlen(hex) / 2;
		for (j = 0; j < fh.h.handle_bytes; j++) {
			unsigned int v;

			sscanf(hex + 2 * j, "%2x", &v);
			fh.h.f_handle[j] = (unsigned char) v;
		}
		check_handle(mfd, kind, i, &fh);
	}
	fclose(in);
	close(mfd);
	return nfail != 0;
}

/* ---- verify ---- */

static void sorted_xattr_names(const char *path, struct bytes **out,
			       size_t *nout, char **mem)
{
	char *list = malloc(70000);
	ssize_t n = llistxattr(path, list, 70000);
	size_t cnt = 0, off;

	*out = NULL;
	*nout = 0;
	*mem = list;
	if (n < 0) {
		failf("llistxattr %s: errno %d", path, errno);
		return;
	}
	for (off = 0; off < (size_t) n; off += strlen(list + off) + 1)
		cnt++;
	*out = malloc((cnt + 1) * sizeof(**out));
	for (off = 0; off < (size_t) n; off += strlen(list + off) + 1) {
		(*out)[*nout].p = list + off;
		(*out)[(*nout)++].n = strlen(list + off);
	}
	qsort(*out, *nout, sizeof(**out), cmp_bytes);
}

static void verify_xattrs(const char *root, const char *file, int longs)
{
	char path[8192], xn[300], val[5], got[16], *mem;
	struct bytes *want = malloc(ncorpus * sizeof(*want)), *have;
	char (*names)[300] = malloc(ncorpus * 300);
	size_t i, nwant = 0, nhave;

	snprintf(path, sizeof(path), "%s/%s", root, file);
	for (i = 0; i < ncorpus; i++) {
		size_t xl;
		ssize_t n;

		if ((corpus[i].n > SHORT_XATTR_NAME) != longs)
			continue;
		xl = xattr_name(i, xn);
		xn[xl] = '\0';
		memcpy(names[nwant], xn, xl + 1);
		want[nwant].p = names[nwant];
		want[nwant++].n = xl;
		xattr_value(i, val);
		n = lgetxattr(path, xn, got, sizeof(got));
		if (n != (ssize_t) sizeof(val) || memcmp(got, val, 5) != 0)
			failf("getxattr %s: %zd (errno %d)", hexs(xn, xl), n,
			      errno);
	}
	qsort(want, nwant, sizeof(*want), cmp_bytes);
	sorted_xattr_names(path, &have, &nhave, &mem);
	if (nhave != nwant)
		failf("listxattr %s: %zu names, want %zu", file, nhave, nwant);
	for (i = 0; i < nhave && i < nwant; i++)
		if (cmp_bytes(&have[i], &want[i]) != 0) {
			failf("listxattr %s: name %zu is %s, want %s", file, i,
			      hexs(have[i].p, have[i].n),
			      hexs(want[i].p, want[i].n));
			break;
		}
	free(have);
	free(mem);
	free(want);
	free(names);
}

static int cmd_names_verify(const char *root, int with_handles)
{
	char path[8192], path2[8192], buf[8192], want[8192];
	struct bytes *all = malloc((ncorpus + 2) * sizeof(*all));
	struct stat st, st2;
	size_t i, n;
	int mfd;
	union fhbuf fh;

	corpus_init();
	all = realloc(all, (ncorpus + 2) * sizeof(*all));
	memcpy(all, corpus, ncorpus * sizeof(*all));
	mfd = open(root, O_RDONLY | O_DIRECTORY);

	snprintf(path, sizeof(path), "%s/f", root);
	check_listing(path, all, ncorpus);
	snprintf(path, sizeof(path), "%s/d", root);
	check_listing(path, all, ncorpus);
	snprintf(path, sizeof(path), "%s/h", root);
	check_listing(path, all, ncorpus);
	snprintf(path, sizeof(path), "%s/r", root);
	check_listing(path, &corpus[ncorpus - 1], 1);
	snprintf(path, sizeof(path), "%s/rd", root);
	check_listing(path, &corpus[ncorpus - 1], 1);
	all[ncorpus].p = LONGLINK;
	all[ncorpus].n = strlen(LONGLINK);
	snprintf(path, sizeof(path), "%s/l", root);
	check_listing(path, all, ncorpus + 1);

	for (i = 0; i < ncorpus; i++) {
		const char *h = hexs(corpus[i].p, corpus[i].n);

		/* regular file */
		bpath(path, sizeof(path), root, "f", corpus[i]);
		n = content_f(i, want, sizeof(want));
		if (lstat(path, &st) == -1 || !S_ISREG(st.st_mode) ||
		    st.st_size != (off_t) n || st.st_nlink != 2)
			failf("lstat f/%s: errno %d mode %o size %lld nlink "
			      "%lu",
			      h, errno, st.st_mode, (long long) st.st_size,
			      (unsigned long) st.st_nlink);
		if (read_file(path, buf, sizeof(buf)) != (int) n ||
		    memcmp(buf, want, n) != 0)
			failf("content of f/%s differs", h);
		if (with_handles) {
			if (get_handle(path, &fh) == -1)
				failf("name_to_handle_at f/%s: errno %d", h,
				      errno);
			else
				check_handle(mfd, 'f', i, &fh);
		}

		/* directory */
		bpath(path, sizeof(path), root, "d", corpus[i]);
		if (lstat(path, &st) == -1 || !S_ISDIR(st.st_mode))
			failf("lstat d/%s: errno %d", h, errno);
		{
			struct bytes in = B("in");

			check_listing(path, &in, 1);
		}
		if (with_handles) {
			if (get_handle(path, &fh) == -1)
				failf("name_to_handle_at d/%s: errno %d", h,
				      errno);
			else
				check_handle(mfd, 'd', i, &fh);
		}

		/* symlink */
		bpath(path, sizeof(path), root, "l", corpus[i]);
		n = target_for(i, want);
		{
			ssize_t r = readlink(path, buf, sizeof(buf));

			if (r != (ssize_t) n || memcmp(buf, want, n) != 0)
				failf("readlink l/%s: %zd (errno %d)", h, r,
				      errno);
		}
		if (lstat(path, &st) == -1 || !S_ISLNK(st.st_mode) ||
		    st.st_size != (off_t) n)
			failf("lstat l/%s: errno %d size %lld", h, errno,
			      (long long) st.st_size);
		if (with_handles) {
			if (get_handle(path, &fh) == -1)
				failf("name_to_handle_at l/%s: errno %d", h,
				      errno);
			else
				check_handle(mfd, 'l', i, &fh);
		}

		/* hard link: h/c[i+1] is f/c[i] */
		bpath(path, sizeof(path), root, "f", corpus[i]);
		bpath(path2, sizeof(path2), root, "h",
		      corpus[(i + 1) % ncorpus]);
		if (lstat(path, &st) == -1 || lstat(path2, &st2) == -1 ||
		    st.st_ino != st2.st_ino || st.st_dev != st2.st_dev)
			failf("hard link of f/%s is not the same object", h);
	}
	{
		ssize_t r;

		long_target(want);
		snprintf(path, sizeof(path), "%s/l/" LONGLINK, root);
		r = readlink(path, buf, sizeof(buf));
		if ((r != LONGLINK_LEN && r != LONGLINK_LEN_SHORT) ||
		    memcmp(buf, want, (size_t) r) != 0)
			failf("readlink of the long target: %zd", r);
	}
	verify_xattrs(root, "x1", 0);
	verify_xattrs(root, "x2", 1);

	/* The rename chain ended on the last name, with its content. */
	bpath(path, sizeof(path), root, "r", corpus[ncorpus - 1]);
	n = content_f(0, want, sizeof(want));
	if (read_file(path, buf, sizeof(buf)) != (int) n ||
	    memcmp(buf, want, n) != 0)
		failf("content after the rename chain differs");
	bpath(path, sizeof(path), root, "rd", corpus[ncorpus - 1]);
	strcat(path, "/in");
	if (read_file(path, buf, sizeof(buf)) != 2)
		failf("rd/<last>/in missing after the rename chain");

	free(all);
	close(mfd);
	if (nfail)
		printf("names-verify: %d failures\n", nfail);
	return nfail != 0;
}

static int cmd_names_remove(const char *root)
{
	char path[8192];
	size_t i, k;
	static const char *const files[] = { "f", "h", "l" };

	corpus_init();
	for (k = 0; k < 3; k++)
		for (i = 0; i < ncorpus; i++) {
			bpath(path, sizeof(path), root, files[k], corpus[i]);
			if (unlink(path) == -1)
				failf("unlink %s/%s: errno %d", files[k],
				      hexs(corpus[i].p, corpus[i].n), errno);
		}
	snprintf(path, sizeof(path), "%s/l/" LONGLINK, root);
	if (unlink(path) == -1)
		failf("unlink long link: errno %d", errno);
	for (i = 0; i < ncorpus; i++) {
		bpath(path, sizeof(path), root, "d", corpus[i]);
		strcat(path, "/in");
		if (unlink(path) == -1)
			failf("unlink d/%s/in: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
		path[strlen(path) - 3] = '\0';
		if (rmdir(path) == -1)
			failf("rmdir d/%s: errno %d",
			      hexs(corpus[i].p, corpus[i].n), errno);
	}
	bpath(path, sizeof(path), root, "r", corpus[ncorpus - 1]);
	if (unlink(path) == -1)
		failf("unlink r/last: errno %d", errno);
	bpath(path, sizeof(path), root, "rd", corpus[ncorpus - 1]);
	strcat(path, "/in");
	if (unlink(path) == -1)
		failf("unlink rd/last/in: errno %d", errno);
	path[strlen(path) - 3] = '\0';
	if (rmdir(path) == -1)
		failf("rmdir rd/last: errno %d", errno);
	snprintf(path, sizeof(path), "%s/x1", root);
	unlink(path);
	snprintf(path, sizeof(path), "%s/x2", root);
	unlink(path);
	for (i = 0; i < sizeof(corpus_dirs) / sizeof(corpus_dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, corpus_dirs[i]);
		if (rmdir(path) == -1)
			failf("rmdir %s: errno %d", corpus_dirs[i], errno);
	}
	if (nfail)
		printf("names-remove: %d failures\n", nfail);
	return nfail != 0;
}

/* ---- dump ---- */

static void dump_tree(const char *path, const char *hexpath, int meta)
{
	struct dlist l = dlist_read(path);
	size_t i;

	if (l.err) {
		printf("%s ERR %d\n", hexpath, l.err);
		return;
	}
	for (i = 0; i < l.n; i++) {
		char child[8192], hexchild[16400], buf[70000];
		struct stat st;
		const char *type = "?";
		size_t pl = strlen(path);

		memcpy(child, path, pl);
		child[pl] = '/';
		memcpy(child + pl + 1, l.e[i].p, l.e[i].n + 1);
		snprintf(hexchild, sizeof(hexchild), "%s/%s", hexpath,
			 hexs(l.e[i].p, l.e[i].n));
		if (lstat(child, &st) == -1) {
			printf("%s LSTAT-ERR %d\n", hexchild, errno);
			continue;
		}
		type = S_ISDIR(st.st_mode)    ? "dir"
		       : S_ISREG(st.st_mode)  ? "reg"
		       : S_ISLNK(st.st_mode)  ? "lnk"
		       : S_ISCHR(st.st_mode)  ? "chr"
		       : S_ISBLK(st.st_mode)  ? "blk"
		       : S_ISFIFO(st.st_mode) ? "fifo"
		       : S_ISSOCK(st.st_mode) ? "sock"
					      : "other";
		printf("%s %s %o %lu %u %u %lld", hexchild, type,
		       st.st_mode & 07777, (unsigned long) st.st_nlink,
		       st.st_uid, st.st_gid,
		       S_ISDIR(st.st_mode) ? 0LL : (long long) st.st_size);
		if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
			printf(" rdev=%u:%u", major(st.st_rdev),
			       minor(st.st_rdev));
		if (S_ISLNK(st.st_mode)) {
			ssize_t r = readlink(child, buf, sizeof(buf));

			printf(" target=%s", r < 0 ? "ERR" : hexs(buf, (size_t) r));
		}
		{
			struct bytes *xs;
			char *mem;
			size_t nx, k;

			sorted_xattr_names(child, &xs, &nx, &mem);
			for (k = 0; k < nx; k++) {
				char name[300], val[70000];
				ssize_t vn;

				memcpy(name, xs[k].p, xs[k].n);
				name[xs[k].n] = '\0';
				vn = lgetxattr(child, name, val, sizeof(val));
				printf(" x:%s=%s", hexs(name, xs[k].n),
				       vn < 0 ? "ERR" : hexs(val, (size_t) vn));
			}
			free(xs);
			free(mem);
		}
		if (S_ISREG(st.st_mode) && !meta) {
			int n = read_file(child, buf, 1024);

			printf(" data=%s", n < 0 ? "ERR" : hexs(buf, (size_t) n));
		}
		printf("\n");
		if (S_ISDIR(st.st_mode))
			dump_tree(child, hexchild, meta);
	}
	dlist_free(&l);
}

static int cmd_names_dump(const char *root, int meta)
{
	dump_tree(root, "", meta);
	return 0;
}

/* ---- errors on special names ---- */

static void errline(const char *op, int r)
{
	printf("%s %s\n", op, r == -1 ? strerrorname_np(errno) : "OK");
}

static int cmd_names_errs(const char *root)
{
	char p[8192], q[8192], n256[257], x256[300];
	int fd, r;

	if (chdir(root) == -1) {
		print_err(errno);
		return 1;
	}
	memset(n256, 'z', 256);
	n256[256] = '\0';
	errline("mkdir .", mkdir(".", 0755));
	errline("mkdir ..", mkdir("..", 0755));
	errline("mkdir ...", mkdir("...", 0755));
	errline("rmdir ...", rmdir("..."));
	fd = open(".", O_WRONLY | O_CREAT, 0644);
	errline("creat .", fd);
	fd = open("..", O_WRONLY | O_CREAT, 0644);
	errline("creat ..", fd);
	errline("symlink .", symlink("t", "."));
	errline("symlink ..", symlink("t", ".."));
	errline("rmdir .", rmdir("."));
	errline("rmdir ..", rmdir(".."));
	errline("unlink .", unlink("."));
	errline("unlink ..", unlink(".."));
	write_file("s", "s", 1);
	errline("rename s .", rename("s", "."));
	errline("rename s ..", rename("s", ".."));
	errline("link s .", link("s", "."));
	errline("link s ..", link("s", ".."));
	errline("mkdir 256", mkdir(n256, 0755));
	fd = open(n256, O_WRONLY | O_CREAT, 0644);
	errline("creat 256", fd);
	{
		struct stat st;

		errline("lstat 256", lstat(n256, &st));
	}
	errline("symlink 256", symlink("t", n256));
	errline("rename s 256", rename("s", n256));
	errline("link s 256", link("s", n256));
	memcpy(x256, "user.", 5);
	memset(x256 + 5, 'y', 251);
	x256[256] = '\0';
	errline("setxattr 256", lsetxattr("s", x256, "v", 1, 0));
	errline("setxattr empty", lsetxattr("s", "", "v", 1, 0));
	errline("setxattr user.", lsetxattr("s", "user.", "v", 1, 0));
	snprintf(p, sizeof(p), "s/x");
	snprintf(q, sizeof(q), "s");
	{
		struct stat st;

		errline("lstat s/x", lstat(p, &st));
	}
	r = unlink("s");
	errline("unlink s", r);
	return 0;
}

/* ---- directory chain deeper than PATH_MAX ---- */

#define CHAIN_DEPTH 24
#define CHAIN_NAME 200

static void chain_name(int k, char *out)
{
	memset(out, 'c', CHAIN_NAME);
	out[0] = (char) ('A' + k);
	out[CHAIN_NAME] = '\0';
}

static int cmd_names_chain_create(const char *root)
{
	char name[CHAIN_NAME + 1];
	int fd = open(root, O_RDONLY | O_DIRECTORY), k;

	for (k = 0; k < CHAIN_DEPTH; k++) {
		int nfd;

		chain_name(k, name);
		if (mkdirat(fd, name, 0755) == -1) {
			failf("mkdirat depth %d: errno %d", k, errno);
			return 1;
		}
		nfd = openat(fd, name, O_RDONLY | O_DIRECTORY);
		close(fd);
		if (nfd == -1) {
			failf("openat depth %d: errno %d", k, errno);
			return 1;
		}
		fd = nfd;
	}
	{
		int lf = openat(fd, "leaf", O_WRONLY | O_CREAT | O_EXCL, 0644);

		if (lf == -1 || write(lf, "deep\n", 5) != 5)
			failf("create leaf: errno %d", errno);
		if (lf != -1)
			close(lf);
	}
	close(fd);
	return nfail != 0;
}

static int cmd_names_chain_check(const char *root)
{
	char name[CHAIN_NAME + 1], buf[16];
	int mfd = open(root, O_RDONLY | O_DIRECTORY);
	int fd = open(root, O_RDONLY | O_DIRECTORY), k;
	union fhbuf fh;
	int mount_id, lf;
	ssize_t n;

	for (k = 0; k < CHAIN_DEPTH; k++) {
		int nfd;

		chain_name(k, name);
		nfd = openat(fd, name, O_RDONLY | O_DIRECTORY);
		close(fd);
		if (nfd == -1) {
			failf("lookup depth %d: errno %d", k, errno);
			return 1;
		}
		fd = nfd;
	}
	lf = openat(fd, "leaf", O_RDONLY);
	n = lf == -1 ? -1 : read(lf, buf, sizeof(buf));
	if (n != 5 || memcmp(buf, "deep\n", 5) != 0)
		failf("leaf read: %zd (errno %d)", n, errno);
	if (lf != -1)
		close(lf);
	memset(&fh, 0, sizeof(fh));
	fh.h.handle_bytes = 128;
	if (name_to_handle_at(fd, "leaf", &fh.h, &mount_id, 0) == -1) {
		failf("name_to_handle_at leaf: errno %d", errno);
	} else {
		lf = open_by_handle_at(mfd, &fh.h, O_RDONLY);
		n = lf == -1 ? -1 : read(lf, buf, sizeof(buf));
		if (n != 5 || memcmp(buf, "deep\n", 5) != 0)
			failf("leaf by handle: %zd (errno %d)", n, errno);
		if (lf != -1)
			close(lf);
	}
	close(fd);
	close(mfd);
	return nfail != 0;
}

/* ---- seeded random names ---- */

static uint64_t rng_state;

static uint64_t rng(void)
{
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static int cmd_names_random(const char *root, const char *seed_str,
			    const char *count_str)
{
	uint64_t count = strtoull(count_str, NULL, 10), idx;

	rng_state = strtoull(seed_str, NULL, 10);
	if (chdir(root) == -1) {
		print_err(errno);
		return 1;
	}
	for (idx = 0; idx < count; idx++) {
		char name[256], xn[300];
		size_t len, k;
		int fd;

		len = (rng() & 1) ? 1 + rng() % 255 : 1 + rng() % 12;
		for (k = 0; k < len; k++) {
			do
				name[k] = (char) (rng() & 0xff);
			while (name[k] == '\0' || name[k] == '/');
		}
		name[len] = '\0';
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		switch (idx % 8) {
		case 0:
			if (mkdir(name, 0755) == -1 && errno != EEXIST)
				failf("mkdir %s: errno %d", hexs(name, len),
				      errno);
			break;
		case 1:
			if (symlink(name, name) == -1 && errno != EEXIST)
				failf("symlink %s: errno %d", hexs(name, len),
				      errno);
			break;
		default:
			fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
			if (fd == -1) {
				if (errno != EEXIST)
					failf("create %s: errno %d",
					      hexs(name, len), errno);
				break;
			}
			if (write(fd, name, len) != (ssize_t) len)
				failf("write %s", hexs(name, len));
			close(fd);
			if (idx % 5 == 0) {
				size_t xl = len > 200 ? 200 : len;

				memcpy(xn, "user.", 5);
				memcpy(xn + 5, name, xl);
				xn[5 + xl] = '\0';
				if (lsetxattr(name, xn, name, len, 0) == -1)
					failf("setxattr %s: errno %d",
					      hexs(xn, 5 + xl), errno);
			}
		}
	}
	return nfail != 0;
}

int main(int argc, char *argv[])
{
	if (argc == 4 && strcmp(argv[1], "truncate") == 0)
		return cmd_truncate(argv[2], argv[3]);
	if (argc == 5 && strcmp(argv[1], "utimens") == 0)
		return cmd_utimens(argv[2], argv[3], argv[4]);
	if (argc == 5 && strcmp(argv[1], "utimes2") == 0)
		return cmd_utimes2(argv[2], argv[3], argv[4]);
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
		return cmd_mmapwrite(argv[2], argv[3], 0);
	if (argc == 4 && strcmp(argv[1], "mmapwrite-closed") == 0)
		return cmd_mmapwrite(argv[2], argv[3], 1);
	if (argc == 3 && strcmp(argv[1], "readnoatime") == 0)
		return cmd_readnoatime(argv[2]);
	if (argc == 3 && strcmp(argv[1], "mmapread") == 0)
		return cmd_mmapread(argv[2]);
	if (argc == 3 && strcmp(argv[1], "heldstat") == 0)
		return cmd_heldstat(argv[2]);
	if (argc == 3 && strcmp(argv[1], "removed-link") == 0)
		return cmd_removed_link(argv[2]);
	if (argc == 4 && strcmp(argv[1], "tmpfile-link") == 0)
		return cmd_tmpfile_link(argv[2], argv[3]);
	if ((argc == 4 || argc == 5) && strcmp(argv[1], "waitmount") == 0)
		return cmd_waitmount(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
	if ((argc == 4 || argc == 5) && strcmp(argv[1], "waitline") == 0)
		return cmd_waitline(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
	if (argc == 3 && strcmp(argv[1], "fsync") == 0)
		return cmd_fsync(argv[2]);
	if (argc == 3 && strcmp(argv[1], "syncfs") == 0)
		return cmd_syncfs(argv[2]);
	if (argc == 4 && strcmp(argv[1], "fsfreeze") == 0)
		return cmd_fsfreeze(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "shutdown") == 0)
		return cmd_shutdown(argv[2], argv[3]);
	if (argc == 6 && strcmp(argv[1], "fallocate") == 0)
		return cmd_fallocate(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 5 && strcmp(argv[1], "writehold") == 0)
		return cmd_writehold(argv[2], argv[3], argv[4]);
	if (argc == 4 && strcmp(argv[1], "sql") == 0)
		return cmd_sql(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "sqlite-lock") == 0)
		return cmd_sqlite_lock(argv[2], argv[3]);
	if (argc >= 7 && strcmp(argv[1], "runas") == 0)
		return cmd_runas(argv);
	if (argc == 3 && strcmp(argv[1], "opath-hold") == 0)
		return cmd_opath_hold(argv[2]);
	if (argc == 3 && strcmp(argv[1], "opath-hold-tree") == 0)
		return cmd_opath_hold_tree(argv[2]);
	if (argc == 3 && strcmp(argv[1], "opath-unlink-stat") == 0)
		return cmd_opath_unlink_stat(argv[2]);
	if (argc == 3 && strcmp(argv[1], "rmcwd") == 0)
		return cmd_rmcwd(argv[2]);
	if (argc == 4 && strcmp(argv[1], "copyrange") == 0)
		return cmd_copyrange(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "clone") == 0)
		return cmd_clone(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "shared-extents") == 0)
		return cmd_shared_extents(argv[2]);
	if (argc == 3 && strcmp(argv[1], "getflags") == 0)
		return cmd_getflags(argv[2]);
	if (argc == 4 && strcmp(argv[1], "setflags") == 0)
		return cmd_setflags(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "fsxattr") == 0)
		return cmd_fsxattr(argv[2]);
	if (argc == 3 && strcmp(argv[1], "getversion") == 0)
		return cmd_getversion(argv[2]);
	if (argc == 3 && strcmp(argv[1], "ioctl-unknown") == 0)
		return cmd_ioctl_unknown(argv[2]);
	if (argc == 5 && strcmp(argv[1], "tmpfile") == 0)
		return cmd_tmpfile(argv[2], argv[3], argv[4]);
	if (argc == 3 && strcmp(argv[1], "unlinked-mutate") == 0)
		return cmd_unlinked_mutate(argv[2]);
	if (argc == 3 && strcmp(argv[1], "opath-unlinked-mutate") == 0)
		return cmd_opath_unlinked_mutate(argv[2]);
	if (argc == 3 && strcmp(argv[1], "rmcwd-mutate") == 0)
		return cmd_rmcwd_mutate(argv[2]);
	if (argc == 3 && strcmp(argv[1], "ext4-tune-casefold") == 0)
		return cmd_ext4_tune_casefold(argv[2]);
	if (argc == 3 && strcmp(argv[1], "readdir-ino") == 0)
		return cmd_readdir_ino(argv[2], 0);
	if (argc == 4 && strcmp(argv[1], "readdir-ino") == 0 &&
	    strcmp(argv[3], "small-first") == 0)
		return cmd_readdir_ino(argv[2], 1);
	if (argc == 6 && strcmp(argv[1], "mkfiles") == 0)
		return cmd_mkfiles(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 3 && strcmp(argv[1], "btrfs-subvol-create") == 0)
		return cmd_btrfs_subvol_create(argv[2]);

	if (argc == 3 && strcmp(argv[1], "names-create") == 0)
		return cmd_names_create(argv[2]);
	if (argc >= 3 && argc <= 4 && strcmp(argv[1], "names-verify") == 0)
		return cmd_names_verify(argv[2], argc == 4);
	if (argc == 4 && strcmp(argv[1], "handle-save") == 0)
		return cmd_handle_save(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "handle-stat") == 0)
		return cmd_handle_stat(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "names-handles-save") == 0)
		return cmd_names_handles_save(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "names-handles-open") == 0)
		return cmd_names_handles_open(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "names-remove") == 0)
		return cmd_names_remove(argv[2]);
	if (argc >= 3 && argc <= 4 && strcmp(argv[1], "names-dump") == 0)
		return cmd_names_dump(argv[2], argc == 4);
	if (argc == 3 && strcmp(argv[1], "names-errs") == 0)
		return cmd_names_errs(argv[2]);
	if (argc == 3 && strcmp(argv[1], "names-chain-create") == 0)
		return cmd_names_chain_create(argv[2]);
	if (argc == 3 && strcmp(argv[1], "names-chain-check") == 0)
		return cmd_names_chain_check(argv[2]);
	if (argc == 5 && strcmp(argv[1], "names-random") == 0)
		return cmd_names_random(argv[2], argv[3], argv[4]);

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
		"       testutil mmapwrite <path> <delay-seconds|usr1>\n"
		"       testutil waitmount <mountpoint> <present|absent> [pid]\n"
		"       testutil waitline <file> <text> [pid]\n"
		"       testutil fsync <path>\n"
		"       testutil syncfs <path>\n"
		"       testutil fsfreeze <path> <freeze|thaw>\n"
		"       testutil shutdown <path> <default|logflush|nologflush>\n"
		"       testutil fallocate <path> <0|keep_size|punch_hole> <offset> <len>\n"
		"       testutil writehold <path> <append|create> <nbytes>\n"
		"       testutil sql <db-path> <query>\n"
		"       testutil sqlite-lock <db-path> <hold-seconds>\n"
		"       testutil runas <uid> <gid> <gid,...|-> -- <cmd> [args...]\n"
		"       testutil opath-unlink-stat <path>\n"
		"       testutil rmcwd <dir>\n"
		"       testutil readdir-ino <dir> [small-first]\n"
		"       testutil mkfiles <dir> <prefix> <suffix> <count>\n"
		"       testutil btrfs-subvol-create <path>\n"
		"       testutil names-{create,verify,remove,dump,errs,chain-create,chain-check} <root>\n"
		"       testutil names-handles-{save,open} <root> <file>\n"
		"       testutil names-random <root> <seed> <count>\n");
	return 2;
}
