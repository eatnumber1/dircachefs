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

[[nodiscard]] absl::Status close(FileDescriptor fd);

// fcntl(fd, F_DUPFD_CLOEXEC, 0).
[[nodiscard]] absl::StatusOr<FileDescriptor> dup(int fd);

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
[[nodiscard]] absl::Status setgroups(std::span<const gid_t> groups);

// getgroups(2): the calling thread's supplementary groups; with size 0 only
// the count.
[[nodiscard]] absl::StatusOr<int> getgroups(int size, gid_t *list);

// mkdtemp(3): creates the directory and returns its name; `pattern` ends in
// XXXXXX. mkstemp(3): creates and opens the file, and `pattern` (ending in
// XXXXXX) becomes its name. O_CLOEXEC is added to the file.
[[nodiscard]] absl::StatusOr<std::string> mkdtemp(std::string_view pattern);
[[nodiscard]] absl::StatusOr<FileDescriptor> mkstemp(std::string &pattern);

// sync(2).
void sync();

// nanosleep(2): sleeps for `duration` (EINTR is an error status, as for the
// others), and getpid(2) (never fails).
[[nodiscard]] absl::Status nanosleep(const struct timespec &duration);
pid_t getpid();

// fcntl(2) with an integer argument (or none). Commands that return a new
// descriptor (F_DUPFD) are for `dup` below, not this.
[[nodiscard]] absl::StatusOr<int> fcntl(int fd, int cmd, int arg = 0);

// realpath(3): the canonical absolute path.
[[nodiscard]] absl::StatusOr<std::string> realpath(std::string_view path);

// clock_gettime(2).
[[nodiscard]] absl::StatusOr<struct timespec> clock_gettime(clockid_t clock);

// getrlimit(2), setrlimit(2), flock(2).
[[nodiscard]] absl::StatusOr<struct rlimit> getrlimit(int resource);
[[nodiscard]] absl::Status setrlimit(int resource, const struct rlimit &limit);
[[nodiscard]] absl::Status flock(int fd, int operation);
// poll(2) of one descriptor: its revents (0 if `timeout_ms` passed).
[[nodiscard]] absl::StatusOr<short> poll(int fd, short events, int timeout_ms);

// --- The mount.dcfs wrapper (phase 15): process, mount and socket calls ----

// getuid(2) (never fails), setsid(2), chdir(2) and unshare(2).
uid_t getuid();
[[nodiscard]] absl::StatusOr<pid_t> setsid();
[[nodiscard]] absl::Status chdir(std::string_view path);
[[nodiscard]] absl::Status unshare(int flags);

// close_range(2): closes the descriptors first..last.
[[nodiscard]] absl::Status close_range(unsigned int first, unsigned int last, int flags);

// open_tree(2): a descriptor for `path` (with OPEN_TREE_CLONE, a detached
// clone of the mount there). O_CLOEXEC is not added: pass OPEN_TREE_CLOEXEC.
[[nodiscard]] absl::StatusOr<FileDescriptor> open_tree(int dirfd, std::string_view path,
                                         unsigned int flags);

// socketpair(2) with SOCK_CLOEXEC added to `type`.
[[nodiscard]] absl::StatusOr<std::pair<FileDescriptor, FileDescriptor>> socketpair(
    int domain, int type, int protocol);
// sendmsg(2), recvmsg(2) (with MSG_CMSG_CLOEXEC added to `flags`) and send(2):
// the bytes transferred. An unconnected peer's EPIPE is a status, never
// SIGPIPE, if `flags` has MSG_NOSIGNAL.
[[nodiscard]] absl::StatusOr<size_t> sendmsg(int fd, const struct msghdr &message,
                               int flags);
[[nodiscard]] absl::StatusOr<size_t> recvmsg(int fd, struct msghdr &message, int flags);
[[nodiscard]] absl::StatusOr<size_t> send(int fd, const void *buf, size_t count, int flags);

// openlog(3) and syslog(3): `message` is logged verbatim (a "%s" format).
// `ident` must outlive the process's logging (a literal).
void openlog(const char *ident, int option, int facility);
void syslog(int priority, std::string_view message);

}  // namespace syscalls

}  // namespace dcfs

#endif  // DCFS_SYSCALLS_H_
