// SPDX-License-Identifier: MIT
// Disks behind the guest's one virtio-scsi controller (run-qemu.sh, a disk-spec
// named sd<letter>) are found by their LUN, not by the kernel's name: the
// kernel names SCSI disks in the order their asynchronous probes finish, so
// /dev/sdb may be LUN 2. guest/init links /dev/dcfs-scsi/sd<letter> to the
// disk of LUN <letter> - 'a'. Each such disk has the size its spec gave, and
// the first is usable as a filesystem (guest/init mounted it as the test's
// directory).

#include <fcntl.h>
// include-cleaner looks for SEEK_END in <stdio.h>, not <cstdio>.
// NOLINTNEXTLINE(modernize-deprecated-headers): the macro's header
#include <stdio.h>

#include <cstdint>
#include <string_view>

#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::Eq;

// The size in bytes of the block device at `path`.
absl::StatusOr<uint64_t> SizeOfDevice(std::string_view path) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::openat(AT_FDCWD, path, O_RDONLY));
  ASSIGN_OR_RETURN(off_t size, syscalls::lseek(*fd, 0, SEEK_END));
  return static_cast<uint64_t>(size);
}

TEST(ScsiDisksTest, EachDiskHasItsSpecsSize) {
  EXPECT_THAT(SizeOfDevice("/dev/dcfs-scsi/sda"), IsOkAndHolds(Eq(64u << 20)));
  EXPECT_THAT(SizeOfDevice("/dev/dcfs-scsi/sdb"), IsOkAndHolds(Eq(96u << 20)));
}

TEST(ScsiDisksTest, TheFirstDiskIsTheTestsDirectory) {
  absl::StatusOr<struct stat> directory = syscalls::stat("/tmp/test_tmpdir");
  absl::StatusOr<struct stat> disk = syscalls::stat("/dev/dcfs-scsi/sda");
  ASSERT_THAT(directory, IsOk());
  ASSERT_THAT(disk, IsOk());
  EXPECT_THAT(directory->st_dev, Eq(disk->st_rdev));
}

}  // namespace
}  // namespace dcfs
