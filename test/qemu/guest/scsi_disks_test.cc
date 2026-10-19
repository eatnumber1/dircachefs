// SPDX-License-Identifier: MIT
// Disks behind the guest's one virtio-scsi controller (run-qemu.sh, a disk-spec
// named sd<letter>) appear in the guest as /dev/sd<letter> in the order of the
// letters, with the sizes the spec gave, and the first is usable as a
// filesystem (guest/init mounted it as the test's directory).

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::testing::Eq;
using ::testing::HasSubstr;

// The size in bytes the kernel reports for the disk `name` (sysfs counts
// 512-byte sectors whatever the disk's block size).
absl::StatusOr<uint64_t> SizeOfDisk(std::string_view name) {
  ASSIGN_OR_RETURN(std::string sectors, testonly::ReadFileToString(absl::StrCat(
                                            "/sys/block/", name, "/size")));
  uint64_t n = 0;
  if (!absl::SimpleAtoi(absl::StripAsciiWhitespace(sectors), &n)) {
    return absl::InvalidArgumentError(
        absl::StrCat("the size of ", name, " is '", sectors, "'"));
  }
  return n * 512;
}

TEST(ScsiDisksTest, EachDiskHasItsSpecsSize) {
  EXPECT_THAT(SizeOfDisk("sda"), IsOkAndHolds(Eq(64u << 20)));
  EXPECT_THAT(SizeOfDisk("sdb"), IsOkAndHolds(Eq(96u << 20)));
}

TEST(ScsiDisksTest, TheFirstDiskIsTheTestsDirectory) {
  ASSERT_OK_AND_ASSIGN(std::string mounts,
                       testonly::ReadFileToString("/proc/mounts"));
  EXPECT_THAT(mounts, HasSubstr("/dev/sda /tmp/test_tmpdir ext4 "));
}

}  // namespace
}  // namespace dcfs
