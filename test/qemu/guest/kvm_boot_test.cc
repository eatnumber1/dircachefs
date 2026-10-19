// SPDX-License-Identifier: MIT
// The counterpart of tcg_boot_test: under the default accelerator the guest's
// CPUID hypervisor signature is KVM's, so the signature that test reads tells
// the two accelerators apart in both directions. A host without a usable
// /dev/kvm boots the guest under TCG (run-qemu.sh says so on the kernel
// command line) and the test is skipped there.

#include <string>

#include "absl/strings/match.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "dcfs/testonly/hypervisor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::Eq;
using ::testing::HasSubstr;

TEST(KvmBootTest, QemuReportsKvmAsTheHypervisor) {
  ASSERT_OK_AND_ASSIGN(std::string cmdline,
                       testonly::ReadFileToString("/proc/cmdline"));
  if (absl::StrContains(cmdline, "dcfs_accel=tcg")) {
    GTEST_SKIP() << "run-qemu.sh fell back to TCG: no usable /dev/kvm";
  }
  EXPECT_THAT(cmdline, HasSubstr("dcfs_accel=kvm"));
  EXPECT_THAT(testonly::HypervisorSignature(), Eq("KVMKVMKVM"));
}

}  // namespace
}  // namespace dcfs
