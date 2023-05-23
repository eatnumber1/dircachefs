#ifndef DFS_SYSCALLS_H_
#define DFS_SYSCALLS_H_

#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dfs/fd.h"
#include "dfs/mount.h"

namespace dfs {
namespace syscalls {

absl::Status close(dfs::FileDescriptor fd);

absl::StatusOr<dfs::FileDescriptor> open(
    const char *pathname, int flags, mode_t mode = 0);

absl::StatusOr<size_t> read(int fd, void *buf, size_t count);

absl::StatusOr<dfs::Mount> mount(
    const char *source, std::string target, const char *filesystemtype,
    unsigned long mountflags = 0, const void *data = nullptr);

absl::Status umount(dfs::Mount mount, int flags = 0);

absl::StatusOr<struct stat> stat(const char *pathname);

absl::Status sigaction(
    int signum,
    const struct sigaction *act = nullptr,
    struct sigaction *oldact = nullptr);

absl::StatusOr<dfs::FileDescriptor> signalfd(
    const sigset_t &mask, int flags = 0);
absl::Status signalfd(int fd, const sigset_t &mask, int flags = 0);

absl::Status sigprocmask(
    int how, const sigset_t *set, sigset_t *oldset = nullptr);

absl::Status pthread_sigmask(
    int how, const sigset_t *set, sigset_t *oldset = nullptr);

absl::Status pthread_setschedparam(
    pthread_t thread, int policy, const struct sched_param &param);

}  // namespace syscalls

// Mask a set of signals (ala pthread_sigmask) and unmask them at destruction.
class ScopedSignalMask {
 public:
  ScopedSignalMask() = default;
  static absl::StatusOr<ScopedSignalMask> Create(int how, const sigset_t &set);

  ScopedSignalMask(ScopedSignalMask &&);
  ScopedSignalMask(const ScopedSignalMask &) = delete;
  ScopedSignalMask &operator=(ScopedSignalMask &&);
  ScopedSignalMask &operator=(const ScopedSignalMask &) = delete;

  ~ScopedSignalMask();

 private:
  ScopedSignalMask(sigset_t oldset);

  bool valid_ = false;
  sigset_t oldset_;
};
}  // namespace dfs

#endif  // DFS_SYSCALLS_H_
