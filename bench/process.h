// Helpers for running dcfs from the benchmarks and reading its memory use.
#ifndef DCFS_BENCH_PROCESS_H_
#define DCFS_BENCH_PROCESS_H_

#include <sys/types.h>

#include <cstdint>

#include <string>
#include <vector>

namespace dcfs_bench {

class DcfsProcess {
 public:
  // Forks and execs `mount.dcfs -o dcfs.fstype=bind,dcfs.cache_db=db,... src
  // mnt` (each flag a dcfs.<flag> option). Does not
  // wait for the mount: see WaitMounted.
  bool Start(
      const std::string &dcfs, const std::string &src, const std::string &db,
      const std::string &mnt, const std::vector<std::string> &flags);
  // Waits up to `seconds` for `mnt` to appear in /proc/self/mountinfo (and
  // the daemon to still be alive).
  bool WaitMounted(int seconds);
  // SIGTERM and wait: dcfs unmounts itself and shuts down cleanly.
  void Stop();
  // SIGKILL, wait, and lazily unmount the dead mount: an unclean shutdown.
  void Crash();

  pid_t pid() const { return pid_; }
  const std::string &mnt() const { return mnt_; }

 private:
  pid_t pid_ = -1;
  std::string mnt_;
};

bool IsMounted(const std::string &mnt);

// Sleeps for `micros` microseconds.
void SleepMicros(long micros);

// VmRSS of `pid` in bytes, or 0.
uint64_t RssBytes(pid_t pid);

// sync(); echo `what` > /proc/sys/vm/drop_caches.
void DropCaches(int what);

}  // namespace dcfs_bench

#endif  // DCFS_BENCH_PROCESS_H_
