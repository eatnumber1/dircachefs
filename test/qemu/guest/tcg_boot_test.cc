// SPDX-License-Identifier: MIT
// The guest boots under TCG when the host asks for it: with DCFS_FORCE_TCG=1
// in the test's environment, run-qemu.sh logs "using tcg" and tells the guest
// so on the kernel command line (dcfs_accel=tcg, set in the same branch), so
// the fallback that hosts without a usable /dev/kvm depend on stays real. The
// test reads the command line; it passing is also the guest having booted and
// run under TCG.

#include <string>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::HasSubstr;

TEST(TcgBootTest, TheGuestIsToldItRunsUnderTcg) {
  ASSERT_OK_AND_ASSIGN(std::string cmdline,
                       testonly::ReadFileToString("/proc/cmdline"));
  EXPECT_THAT(cmdline, HasSubstr("dcfs_accel=tcg"));
}

}  // namespace
}  // namespace dcfs
