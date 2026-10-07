#include "dcfs/mount_fds.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls_backing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

DeviceId MakeId(uint8_t seed, uint64_t subvol_id = 0) {
  DeviceId id;
  id.uuid.fill(seed);
  id.subvol_id = subvol_id;
  return id;
}

// Any valid, harmless fd works: MountFds never dereferences it.
FileDescriptor OpenPlaceholderFd() {
  absl::StatusOr<FileDescriptor> opened =
      syscalls::openat(AT_FDCWD, "/", O_PATH);
  CHECK_OK(opened);
  return *std::move(opened);
}

TEST(MountFdsTest, InsertGetErase) {
  MountFds fds;
  DeviceId id = MakeId(1);
  FileDescriptor fd = OpenPlaceholderFd();
  ASSERT_GE(*fd, 0);
  int raw_fd = *fd;

  ASSERT_THAT(fds.Insert(id, std::move(fd)), IsOk());
  EXPECT_EQ(fds.size(), 1u);

  absl::StatusOr<int> got = fds.Get(id);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, raw_fd);

  fds.Erase(id);
  EXPECT_EQ(fds.size(), 0u);
  EXPECT_THAT(fds.Get(id), StatusIs(absl::StatusCode::kNotFound));
}

TEST(MountFdsTest, InsertDuplicateFails) {
  MountFds fds;
  DeviceId id = MakeId(2);

  ASSERT_THAT(fds.Insert(id, OpenPlaceholderFd()), IsOk());
  EXPECT_THAT(fds.Insert(id, OpenPlaceholderFd()),
              StatusIs(absl::StatusCode::kAlreadyExists));
  EXPECT_EQ(fds.size(), 1u);
}

TEST(MountFdsTest, GetNotFoundIncludesDeviceId) {
  MountFds fds;
  DeviceId id = MakeId(3, 42);
  EXPECT_THAT(fds.Get(id),
              StatusIs(absl::StatusCode::kNotFound, HasSubstr(id.ToString())));
}

TEST(MountFdsTest, EraseNotPresentIsNoop) {
  MountFds fds;
  DeviceId id = MakeId(4);
  fds.Erase(id);
  EXPECT_EQ(fds.size(), 0u);
}

}  // namespace
}  // namespace dcfs
