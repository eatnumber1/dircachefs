#ifndef DCFS_SYSCALLS_H_
#define DCFS_SYSCALLS_H_

#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>

#include <span>
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

}  // namespace syscalls

}  // namespace dcfs

#endif  // DCFS_SYSCALLS_H_
