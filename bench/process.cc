#include "bench/process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_process.h"

namespace dcfs_bench {

void SleepMicros(long micros) {
  const struct timespec duration = {.tv_sec = micros / 1'000'000,
                                    .tv_nsec = (micros % 1'000'000) * 1000};
  dcfs::syscalls::nanosleep(duration).IgnoreError();
}

bool IsMounted(const std::string &mnt) {
  std::ifstream f("/proc/self/mountinfo");
  std::string line;
  const std::string needle = " " + mnt + " ";
  while (std::getline(f, line)) {
    if (line.find(needle) != std::string::npos &&
        line.find("fuse") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DcfsProcess::Start(
    const std::string &dcfs, const std::string &src, const std::string &db,
    const std::string &mnt, const std::vector<std::string> &flags) {
  mnt_ = mnt;
  std::vector<std::string> args = {
      dcfs, "--source=" + src, "--cache_db=" + db};
  args.insert(args.end(), flags.begin(), flags.end());
  args.push_back(mnt);
  absl::StatusOr<pid_t> forked = dcfs::syscalls::fork();
  if (!forked.ok()) return false;
  pid_ = *forked;
  if (pid_ == 0) {
    absl::StatusOr<dcfs::FileDescriptor> log = dcfs::syscalls::openat(
        AT_FDCWD, "/tmp/dcfs-bench.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log.ok()) {
      dcfs::syscalls::dup2(**log, 1).IgnoreError();
      dcfs::syscalls::dup2(**log, 2).IgnoreError();
    }
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    dcfs::syscalls::execv(dcfs.c_str(), argv.data()).IgnoreError();
    dcfs::syscalls::_exit(127);
  }
  return true;
}

bool DcfsProcess::WaitMounted(int seconds) {
  for (int i = 0; i < seconds * 100; ++i) {
    if (IsMounted(mnt_)) return true;
    int status;
    absl::StatusOr<pid_t> reaped =
        dcfs::syscalls::waitpid(pid_, &status, WNOHANG);
    if (reaped.ok() && *reaped == pid_) {
      pid_ = -1;
      return false;
    }
    SleepMicros(10000);
  }
  return false;
}

void DcfsProcess::Stop() {
  if (pid_ <= 0) return;
  dcfs::syscalls::kill(pid_, SIGTERM).IgnoreError();
  int status;
  dcfs::syscalls::waitpid(pid_, &status, 0).IgnoreError();
  pid_ = -1;
  if (IsMounted(mnt_)) {
    dcfs::syscalls::umount2(mnt_, MNT_DETACH).IgnoreError();
  }
}

void DcfsProcess::Crash() {
  if (pid_ <= 0) return;
  dcfs::syscalls::kill(pid_, SIGKILL).IgnoreError();
  int status;
  dcfs::syscalls::waitpid(pid_, &status, 0).IgnoreError();
  pid_ = -1;
  dcfs::syscalls::umount2(mnt_, MNT_DETACH).IgnoreError();
}

uint64_t RssBytes(pid_t pid) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      return strtoull(line.c_str() + 6, nullptr, 10) * 1024;
    }
  }
  return 0;
}

void DropCaches(int what) {
  dcfs::syscalls::sync();
  absl::StatusOr<dcfs::FileDescriptor> fd =
      dcfs::syscalls::openat(AT_FDCWD, "/proc/sys/vm/drop_caches", O_WRONLY);
  if (!fd.ok()) return;
  char c = static_cast<char>('0' + what);
  if (absl::StatusOr<size_t> n = dcfs::syscalls::write(**fd, &c, 1);
      !n.ok() || *n != 1) {
    fprintf(stderr, "drop_caches: %s\n", n.status().ToString().c_str());
  }
}

}  // namespace dcfs_bench
