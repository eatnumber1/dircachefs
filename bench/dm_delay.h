// A device-mapper "delay" target over a block device, made through the
// /dev/mapper/control ioctls (the guest has no dmsetup).
#ifndef DCFS_BENCH_DM_DELAY_H_
#define DCFS_BENCH_DM_DELAY_H_

#include <string>

namespace dcfs_bench {

// Creates and resumes a dm device `name` that passes everything through to
// `device` with every read and write delayed by `delay_ms`. Returns the
// device node (/dev/dm-N) or "" after printing an error to stderr.
std::string CreateDelayDevice(const std::string &name,
                              const std::string &device, int delay_ms);

}  // namespace dcfs_bench

#endif  // DCFS_BENCH_DM_DELAY_H_
