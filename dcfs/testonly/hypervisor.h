// SPDX-License-Identifier: MIT
#ifndef DCFS_TESTONLY_HYPERVISOR_H_
#define DCFS_TESTONLY_HYPERVISOR_H_

#include <cpuid.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace dcfs::testonly {

// The hypervisor signature x86 CPUID leaf 0x40000000 returns in EBX, ECX and
// EDX: "KVMKVMKVM" under KVM, "TCGTCGTCGTCG" under QEMU's TCG (QEMU's
// cpu_x86_cpuid in target/i386/cpu.c fills it), 12 bytes either way, and
// without trailing NULs in the returned string. Empty on a CPU without the
// hypervisor leaves.
inline std::string HypervisorSignature() {
  unsigned int eax = 0;
  unsigned int regs[3] = {0, 0, 0};
  __cpuid(0x40000000, eax, regs[0], regs[1], regs[2]);
  char raw[sizeof(regs)];
  std::memcpy(raw, regs, sizeof(raw));
  return {raw, std::find(raw, raw + sizeof(raw), '\0')};
}

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_HYPERVISOR_H_
