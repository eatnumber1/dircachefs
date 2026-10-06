// The guest's minimal kernel has no POSIX timers (no CONFIG_POSIX_TIMERS,
// so clock_gettime of a CPU-time clock fails with EINVAL), and
// google/benchmark exits when its per-thread CPU clock fails. This
// link-time wrapper (-Wl,--wrap=clock_gettime, see BUILD.bazel) answers
// the CPU-time clocks with CLOCK_MONOTONIC when the kernel refuses them, so
// the "CPU" column equals wall time here; only the Time column is of
// interest in these benchmarks. Moot once the kernel has POSIX timers.
#include <time.h>

extern "C" int __real_clock_gettime(clockid_t clock, struct timespec *ts);

extern "C" int __wrap_clock_gettime(clockid_t clock, struct timespec *ts) {
  int rc = __real_clock_gettime(clock, ts);
  if (rc != 0 &&
      (clock == CLOCK_THREAD_CPUTIME_ID || clock == CLOCK_PROCESS_CPUTIME_ID)) {
    return __real_clock_gettime(CLOCK_MONOTONIC, ts);
  }
  return rc;
}
