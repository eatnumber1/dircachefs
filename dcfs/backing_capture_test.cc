#include "dcfs/backing_capture.h"


#include <string>

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

TEST(NativeMountCommandTest, SloppyAndVerboseArePassedThrough) {
  EXPECT_THAT(NativeMountCommand({.source = "h:/e",
                                  .native_type = "nfs",
                                  .sloppy = true,
                                  .verbose = true},
                                 "/tmp/s"),
              ElementsAre("mount", "-n", "-s", "-v", "-t", "nfs", "h:/e",
                          "/tmp/s"));
}

}  // namespace
}  // namespace dcfs
