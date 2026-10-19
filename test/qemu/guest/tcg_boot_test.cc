// SPDX-License-Identifier: MIT
// The guest boots under TCG when the host asks for it: with DCFS_FORCE_TCG=1
// in the test's environment, run-qemu.sh logs "using tcg" and tells the guest
// so on the kernel command line (dcfs_accel=tcg, set in the same branch), so
// the fallback that hosts without a usable /dev/kvm depend on stays real. The
// command line is only what run-qemu.sh says, so the test also asks QEMU: the
// hypervisor signature in CPUID leaf 0x40000000 is "TCGTCGTCGTCG" under TCG
// and "KVMKVMKVM" under KVM.

#include <string>

#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "dcfs/testonly/hypervisor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::Eq;
using ::testing::HasSubstr;

TEST(TcgBootTest, TheGuestIsToldItRunsUnderTcg) {
  ASSERT_OK_AND_ASSIGN(std::string cmdline,
                       testonly::ReadFileToString("/proc/cmdline"));
  EXPECT_THAT(cmdline, HasSubstr("dcfs_accel=tcg"));
}

TEST(TcgBootTest, QemuReportsTcgAsTheHypervisor) {
  EXPECT_THAT(testonly::HypervisorSignature(), Eq("TCGTCGTCGTCG"));
}

}  // namespace
}  // namespace dcfs
