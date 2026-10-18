// SPDX-License-Identifier: MIT
#ifndef DCFS_CAPABILITY_SET_H_
#define DCFS_CAPABILITY_SET_H_

#include <cstdint>

namespace dcfs {

// A set of capabilities (CAP_FSETID, ...; capabilities(7)), as a thread's
// effective, permitted or inheritable set. The kernel keeps one as a
// kernel_cap_t, bit n for capability n (include/linux/capability.h:24 (Linux
// v7.3-rc7)), and gives it out in that layout: in hex in /proc/<pid>/status,
// in two 32-bit words through capget(2).
class CapabilitySet {
 public:
  constexpr CapabilitySet() = default;

  // The set whose kernel_cap_t is `bits`, and back.
  static constexpr CapabilitySet FromKernel(uint64_t bits) {
    CapabilitySet set;
    set.bits_ = bits;
    return set;
  }
  constexpr uint64_t ToKernel() const { return bits_; }

  // `cap` is a CAP_* constant of <linux/capability.h>: at most CAP_LAST_CAP,
  // which is below 64.
  constexpr bool Has(int cap) const { return ((bits_ >> cap) & 1) != 0; }
  constexpr void Add(int cap) { bits_ |= uint64_t{1} << cap; }

 private:
  uint64_t bits_ = 0;
};

}  // namespace dcfs

#endif  // DCFS_CAPABILITY_SET_H_
