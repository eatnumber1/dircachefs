#include "dcfs/backing_capture.h"

#include <sys/mount.h>
#include <sys/statvfs.h>

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::ElementsAre;

TEST(NativeMountCommandTest, AutodetectedTypeHasNoTypeFlag) {
  EXPECT_THAT(NativeMountCommand({.source = "/dev/sdb1"}, "/tmp/s"),
              ElementsAre("mount", "-n", "/dev/sdb1", "/tmp/s"));
}

TEST(NativeMountCommandTest, TypeAndOptions) {
  EXPECT_THAT(NativeMountCommand({.source = "UUID=aaaa",
                                  .native_type = "xfs",
                                  .options = {"noatime", "ro"}},
                                 "/tmp/s"),
              ElementsAre("mount", "-n", "-t", "xfs", "-o", "noatime,ro",
                          "UUID=aaaa", "/tmp/s"));
}

TEST(NativeMountCommandTest, BindIgnoresTheTypeAndAddsBindFirst) {
  EXPECT_THAT(NativeMountCommand({.source = "/srv/raw",
                                  .native_type = "ext4",
                                  .bind = true,
                                  .options = {"ro"}},
                                 "/tmp/s"),
              ElementsAre("mount", "-n", "-o", "bind,ro", "/srv/raw",
                          "/tmp/s"));
}

TEST(NativeMountCommandTest, SloppyAndVerboseArePassedThrough) {
  EXPECT_THAT(NativeMountCommand({.source = "h:/e",
                                  .native_type = "nfs",
                                  .sloppy = true,
                                  .verbose = true},
                                 "/tmp/s"),
              ElementsAre("mount", "-n", "-s", "-v", "-t", "nfs", "h:/e",
                          "/tmp/s"));
}

// The bind's read-only remount keeps the flags the mount already has.
TEST(MountFlagsFromStatvfsTest, KeepsTheMountsOwnFlags) {
  EXPECT_EQ(MountFlagsFromStatvfs(0), 0u);
  EXPECT_EQ(MountFlagsFromStatvfs(ST_NOSUID | ST_NODEV | ST_NOEXEC),
            static_cast<unsigned long>(MS_NOSUID | MS_NODEV | MS_NOEXEC));
  EXPECT_EQ(MountFlagsFromStatvfs(ST_NOATIME | ST_NODIRATIME | ST_RDONLY),
            static_cast<unsigned long>(MS_NOATIME | MS_NODIRATIME |
                                       MS_RDONLY));
  EXPECT_EQ(MountFlagsFromStatvfs(ST_RELATIME), 0u);
}

}  // namespace
}  // namespace dcfs
