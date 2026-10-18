// SPDX-License-Identifier: MIT
#ifndef DCFS_SYSCALLS_H_
#define DCFS_SYSCALLS_H_

#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "dcfs/fd.h"

namespace dcfs::syscalls {

absl::Status close(FileDescriptor fd);

// fcntl(fd, F_DUPFD_CLOEXEC, 0).
absl::StatusOr<FileDescriptor> dup(int fd);

// --- Per-thread filesystem credentials (see backing.cc's AsCaller) ---------
//
// setfsuid(2)/setfsgid(2): set the calling thread's filesystem uid/gid.
// The system calls report no error: an id the kernel does not accept is
// silently ignored, and the manpage's only check is to read the value back.
// So each wrapper makes two calls (a failure comes back inside the result,
// decoded here: 1.5): EINVAL for (uid_t)-1, which is the query form and not
// an id; EPERM "did not take" when the id read back differs.
absl::Status setfsuid(uid_t uid);
absl::Status setfsgid(gid_t gid);

// The calling thread's filesystem uid/gid: the manpage idiom, setfsuid(-1)
// changes nothing and returns the current value. Cannot fail.
uid_t fsuid();
gid_t fsgid();

// umask(2): sets the process's file mode creation mask and returns the
// previous one. Cannot fail. Process-wide (the fs_struct is shared by every
// thread not created with CLONE_FS unshared), unlike the credentials above.
mode_t umask(mode_t mask);

// setgroups(2) as the raw system call, which on Linux changes only the
// CALLING THREAD's supplementary groups. Not glibc's setgroups(), which
// broadcasts the change to every thread of the process (the POSIX
// per-process semantics, implemented with a signal to each thread).
absl::Status setgroups(std::span<const gid_t> groups);

// getgroups(2): the calling thread's supplementary groups; with size 0 only
// the count.
absl::StatusOr<int> getgroups(int size, gid_t *list);

// A thread's capability sets, a bit per capability (uint64_t{1} <<
// CAP_FSETID, ...).
struct CapabilitySets {
  uint64_t effective = 0;
  uint64_t permitted = 0;
  uint64_t inheritable = 0;
};

// capget(2) and capset(2) of the CALLING THREAD alone (pid 0, version 3).
// capset fails with EPERM for an effective capability outside the
// permitted set, or a permitted one the thread does not have.
absl::StatusOr<CapabilitySets> capget();
absl::Status capset(const CapabilitySets &sets);

// mkdtemp(3): creates the directory and returns its name; `pattern` ends in
// XXXXXX. mkstemp(3): creates and opens the file, and `pattern` (ending in
// XXXXXX) becomes its name. O_CLOEXEC is added to the file.
absl::StatusOr<std::string> mkdtemp(std::string_view pattern);
absl::StatusOr<FileDescriptor> mkstemp(std::string &pattern);

// sync(2).
void sync();

// nanosleep(2): sleeps for `duration` (EINTR is an error status, as for the
// others), and getpid(2) and gettid(2) (never fail).
absl::Status nanosleep(const struct timespec &duration);
pid_t getpid();
pid_t gettid();

// fcntl(2) with an integer argument (or none). Commands that return a new
// descriptor (F_DUPFD) are for `dup` below, not this.
absl::StatusOr<int> fcntl(int fd, int cmd, int arg = 0);

// realpath(3): the canonical absolute path.
absl::StatusOr<std::string> realpath(std::string_view path);

// clock_gettime(2).
absl::StatusOr<struct timespec> clock_gettime(clockid_t clock);

// getrlimit(2), setrlimit(2), flock(2).
absl::StatusOr<struct rlimit> getrlimit(int resource);
absl::Status setrlimit(int resource, const struct rlimit &limit);
absl::Status flock(int fd, int operation);
// poll(2) of one descriptor: its revents (0 if `timeout_ms` passed).
absl::StatusOr<short> poll(int fd, short events, int timeout_ms);

// poll(2) of several descriptors: how many have events; each `fds[i].revents`
// says which (0 for all of them if `timeout` passed). `timeout` is not
// negative and at most 2^31-1 ms (about 24 days), or InfiniteDuration() to
// wait for an event.
absl::StatusOr<int> poll(std::span<struct pollfd> fds, absl::Duration timeout);

// pidfd_open(2) of `pid` (no flags): a descriptor that polls readable once the
// process has exited.
absl::StatusOr<FileDescriptor> pidfd_open(pid_t pid);

// setresuid(2)/setresgid(2) for the whole process, as glibc does. A process
// that has given up root cannot take it back (EPERM).
absl::Status setresuid(uid_t real, uid_t effective, uid_t saved);
absl::Status setresgid(gid_t real, gid_t effective, gid_t saved);

// --- The mount.dcfs wrapper (phase 15): process, mount and socket calls ----

// getuid(2), geteuid(2), getgid(2), getegid(2) (never fail), setsid(2),
// chdir(2) and getcwd(3).
uid_t getuid();
uid_t geteuid();
gid_t getgid();
gid_t getegid();
absl::StatusOr<pid_t> setsid();
absl::Status chdir(std::string_view path);
absl::StatusOr<std::string> getcwd();

// socketpair(2) with SOCK_CLOEXEC added to `type`.
absl::StatusOr<std::pair<FileDescriptor, FileDescriptor>> socketpair(
    int domain, int type, int protocol);
// send(2): the bytes transferred. An unconnected peer's EPIPE is a status,
// never SIGPIPE, if `flags` has MSG_NOSIGNAL.
absl::StatusOr<size_t> send(int fd, const void *buf, size_t count, int flags);

// openlog(3) and syslog(3): `message` is logged verbatim (a "%s" format).
// `ident` must outlive the process's logging (a literal).
void openlog(const char *ident, int option, int facility);
void syslog(int priority, std::string_view message);

// --- dcfs's own files off the backing filesystem ----------------------------
//
// The daemon's lock file in /run/dcfs (umount_helper.h), by path. They do not
// pass the backing door (backing_door.h), so a daemon whose door is poisoned
// still releases its lock. Never for a name on the backing filesystem: those
// are syscalls_backing.h's *at calls, at the door.

// open(2), O_CLOEXEC added; mkdir(2); unlink(2); stat(2).
absl::StatusOr<FileDescriptor> open(std::string_view path, int flags,
                                    mode_t mode = 0);
absl::Status mkdir(std::string_view path, mode_t mode);
absl::Status unlink(std::string_view path);
absl::StatusOr<struct stat> stat(std::string_view path);

// chmod(2), rename(2), rmdir(2) and statfs(2), by path.
absl::Status chmod(std::string_view path, mode_t mode);
absl::Status rename(std::string_view from, std::string_view to);
absl::Status rmdir(std::string_view path);
absl::StatusOr<struct statfs> statfs(std::string_view path);

// mmap(2), munmap(2) and mincore(2): the mapping's address, and for mincore
// one byte per page of [addr, addr + length) in `vec`, whose low bit says the
// page is in the page cache.
absl::StatusOr<void *> mmap(void *addr, size_t length, int prot, int flags,
                            int fd, off_t offset);
absl::Status munmap(void *addr, size_t length);
absl::Status mincore(void *addr, size_t length, unsigned char *vec);

// inotify_init1(2) (IN_CLOEXEC is added to `flags`) and inotify_add_watch(2):
// the watch descriptor.
absl::StatusOr<FileDescriptor> inotify_init1(int flags);
absl::StatusOr<int> inotify_add_watch(int fd, std::string_view path,
                                      uint32_t mask);

}  // namespace dcfs::syscalls

#endif  // DCFS_SYSCALLS_H_
