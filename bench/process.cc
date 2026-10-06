#include "bench/process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dcfs_bench {

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
  pid_ = fork();
  if (pid_ < 0) return false;
  if (pid_ == 0) {
    int fd = open("/tmp/dcfs-bench.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
    }
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(dcfs.c_str(), argv.data());
    _exit(127);
  }
  return true;
}

bool DcfsProcess::WaitMounted(int seconds) {
  for (int i = 0; i < seconds * 100; ++i) {
    if (IsMounted(mnt_)) return true;
    int status;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      return false;
    }
    usleep(10000);
  }
  return false;
}

void DcfsProcess::Stop() {
  if (pid_ <= 0) return;
  kill(pid_, SIGTERM);
  int status;
  waitpid(pid_, &status, 0);
  pid_ = -1;
  if (IsMounted(mnt_)) umount2(mnt_.c_str(), MNT_DETACH);
}

void DcfsProcess::Crash() {
  if (pid_ <= 0) return;
  kill(pid_, SIGKILL);
  int status;
  waitpid(pid_, &status, 0);
  pid_ = -1;
  umount2(mnt_.c_str(), MNT_DETACH);
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
  sync();
  int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if (fd < 0) return;
  char c = static_cast<char>('0' + what);
  if (write(fd, &c, 1) != 1) perror("drop_caches");
  close(fd);
}

}  // namespace dcfs_bench
