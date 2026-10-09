#ifndef DCFS_SYSCALLS_H_
#define DCFS_SYSCALLS_H_

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#include <span>
#include <utility>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"

namespace dcfs {
namespace syscalls {

absl::Status close(FileDescriptor fd);

// fcntl(fd, F_DUPFD_CLOEXEC, 0).
absl::StatusOr<FileDescriptor> dup(int fd);

// --- Per-thread filesystem credentials (see backing.cc's AsCaller) ---------
//
// setfsuid(2)/setfsgid(2): set the calling thread's filesystem uid/gid and
// return the PREVIOUS value. They report no error: an unacceptable id is
// silently ignored (the previous value is returned either way), so a caller
// must read the value back to know whether a switch took effect -- passing
// an invalid id such as -1 changes nothing and returns the current value
// (backing.cc reads the current ids that way).
uid_t setfsuid(uid_t uid);
gid_t setfsgid(gid_t gid);

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

// mkdtemp(3): creates the directory and returns its name; `pattern` ends in
// XXXXXX. mkstemp(3): creates and opens the file, and `pattern` (ending in
// XXXXXX) becomes its name. O_CLOEXEC is added to the file.
absl::StatusOr<std::string> mkdtemp(std::string_view pattern);
absl::StatusOr<FileDescriptor> mkstemp(std::string &pattern);

// sync(2).
void sync();

// nanosleep(2): sleeps for `duration` (EINTR is an error status, as for the
// others), and getpid(2) (never fails).
absl::Status nanosleep(const struct timespec &duration);
pid_t getpid();

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

// --- The mount.dcfs wrapper (phase 15): process, mount and socket calls ----

// getuid(2) (never fails), setsid(2), chdir(2) and unshare(2).
uid_t getuid();
absl::StatusOr<pid_t> setsid();
absl::Status chdir(std::string_view path);
absl::Status unshare(int flags);

// close_range(2): closes the descriptors first..last.
absl::Status close_range(unsigned int first, unsigned int last, int flags);

// open_tree(2): a descriptor for `path` (with OPEN_TREE_CLONE, a detached
// clone of the mount there). O_CLOEXEC is not added: pass OPEN_TREE_CLOEXEC.
absl::StatusOr<FileDescriptor> open_tree(int dirfd, std::string_view path,
                                         unsigned int flags);

// socketpair(2) with SOCK_CLOEXEC added to `type`.
absl::StatusOr<std::pair<FileDescriptor, FileDescriptor>> socketpair(
    int domain, int type, int protocol);
// sendmsg(2), recvmsg(2) (with MSG_CMSG_CLOEXEC added to `flags`) and send(2):
// the bytes transferred. An unconnected peer's EPIPE is a status, never
// SIGPIPE, if `flags` has MSG_NOSIGNAL.
absl::StatusOr<size_t> sendmsg(int fd, const struct msghdr &message,
                               int flags);
absl::StatusOr<size_t> recvmsg(int fd, struct msghdr &message, int flags);
absl::StatusOr<size_t> send(int fd, const void *buf, size_t count, int flags);

// openlog(3) and syslog(3): `message` is logged verbatim (a "%s" format).
// `ident` must outlive the process's logging (a literal).
void openlog(const char *ident, int option, int facility);
void syslog(int priority, std::string_view message);

}  // namespace syscalls

}  // namespace dcfs

#endif  // DCFS_SYSCALLS_H_
