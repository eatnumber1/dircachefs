// Step 15.6b: the lock the daemon holds for umount.fuse to wait on. Waiting for a real daemon is
// test/qemu/guest/mount_dcfs.sh (and the systemd guest): it takes a mount.
#include "dcfs/umount_helper.h"

#include <fcntl.h>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

TEST(HoldDaemonLockTest, OneDaemonPerFuseDevice) {
  constexpr char kInfo[] = "40 26 0:91 / /m rw - fuse.dcfs /dev/vdb rw\n";
  absl::StatusOr<DaemonLock> lock = HoldDaemonLock(kInfo, "/m");
  ASSERT_THAT(lock, IsOk());
  EXPECT_EQ(lock->path, "/run/dcfs/0_91.lock");
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, lock->path), IsOk());
  EXPECT_THAT(HoldDaemonLock(kInfo, "/m"),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("still shutting down")));
}

// A helper that opened the file before the unmount waits for the daemon it
// belongs to, never for the next daemon that gets the same device number.
TEST(HoldDaemonLockTest, ARemovedFileIsAFreshLockForTheNextDaemon) {
  constexpr char kInfo[] = "40 26 0:92 / /m rw - fuse.dcfs /dev/vdb rw\n";
  absl::StatusOr<DaemonLock> first = HoldDaemonLock(kInfo, "/m");
  ASSERT_THAT(first, IsOk());
  RemoveDaemonLockFile(*first);
  EXPECT_EQ(StatusToErrno(syscalls::fstatat(AT_FDCWD, first->path).status()),
            ENOENT);
  absl::StatusOr<DaemonLock> second = HoldDaemonLock(kInfo, "/m");
  EXPECT_THAT(second, IsOk());
  // Removing it twice is not an error worth a log line.
  RemoveDaemonLockFile(*first);
  RemoveDaemonLockFile(*second);
}

TEST(HoldDaemonLockTest, AMountThatIsNotThereIsAnInternalError) {
  EXPECT_THAT(HoldDaemonLock("26 1 8:1 / / rw - ext4 /dev/vda rw\n", "/m"),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("/m")));
}

}  // namespace
}  // namespace dcfs
