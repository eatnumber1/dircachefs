#include "bench/process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/testonly/files.h"

namespace dcfs_bench {

void SleepMicros(long micros) {
  const struct timespec duration = {.tv_sec = micros / 1'000'000,
                                    .tv_nsec = (micros % 1'000'000) * 1000};
  dcfs::syscalls::nanosleep(duration).IgnoreError();
}

bool IsMounted(const std::string &mnt) {
  absl::StatusOr<std::string> info =
      dcfs::testonly::ReadFileToString("/proc/self/mountinfo");
  if (!info.ok()) return false;
  const std::string needle = " " + mnt + " ";
  for (std::string_view line : absl::StrSplit(*info, '\n')) {
    if (line.find(needle) != std::string_view::npos &&
        line.find("fuse") != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

bool DcfsProcess::Start(const std::string &dcfs, const std::string &src,
                        const std::string &db, const std::string &mnt,
                        const std::vector<std::string> &flags) {
  mnt_ = mnt;
  // dcfs dispatches on argv[0] (phase 15): it runs as mount.dcfs, in the
  // foreground, with each flag as a dcfs.<flag> mount option.
  std::string options = "dcfs.fstype=bind,dcfs.foreground,dcfs.cache_db=" + db;
  for (const std::string &flag : flags) {
    options += ",dcfs." + (flag.starts_with("--") ? flag.substr(2) : flag);
  }
  std::vector<std::string> args = {"mount.dcfs", "-o", options, src, mnt};
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
  absl::StatusOr<std::string> status = dcfs::testonly::ReadFileToString(
      "/proc/" + std::to_string(pid) + "/status");
  if (!status.ok()) return 0;
  for (std::string_view line : absl::StrSplit(*status, '\n')) {
    if (absl::ConsumePrefix(&line, "VmRSS:")) {
      return strtoull(std::string(line).c_str(), nullptr, 10) * 1024;
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
    fprintf(stderr, "drop_caches: wrote %zu of 1 byte: %s\n",
            n.ok() ? *n : size_t{0}, n.status().ToString().c_str());
  }
}

}  // namespace dcfs_bench
