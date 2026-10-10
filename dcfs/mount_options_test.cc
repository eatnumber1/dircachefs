#include "dcfs/mount_options.h"

#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Optional;

// Step 15.8: both are always there, in this order, whatever else is named.
TEST(MountOptionsTest, DefaultPermissionsAndAllowOtherComeFirst) {
  EXPECT_THAT(BuildMountOptions({}),
              IsOkAndHolds(Field(&MountOptions::options,
                                 ElementsAre("default_permissions",
                                             "allow_other"))));
}

TEST(MountOptionsTest, FuseOptFollows) {
  std::vector<std::string> fuse_opt = {"suid", "dev"};
  EXPECT_THAT(BuildMountOptions(fuse_opt),
              IsOkAndHolds(Field(&MountOptions::options,
                                 ElementsAre("default_permissions",
                                             "allow_other", "suid", "dev"))));
}

TEST(MountOptionsTest, MaxReadIsKept) {
  std::vector<std::string> fuse_opt = {"max_read=65536"};
  EXPECT_THAT(BuildMountOptions(fuse_opt),
              IsOkAndHolds(Field(&MountOptions::max_read, Optional(65536u))));
  std::vector<std::string> bad = {"max_read=lots"};
  EXPECT_THAT(BuildMountOptions(bad),
              IsOkAndHolds(Field(&MountOptions::max_read, std::nullopt)));
}

// dcfs adds default_permissions itself: naming it is a mistake about what
// dcfs does, reported rather than passed through twice.
TEST(MountOptionsTest, FuseOptNamingDefaultPermissionsIsRedundant) {
  std::vector<std::string> fuse_opt = {"suid", "default_permissions"};
  EXPECT_THAT(BuildMountOptions(fuse_opt),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("default_permissions")));
}

TEST(MountOptionsTest, FuseOptNamingAllowOtherIsRedundant) {
  for (const char *named : {"allow_other", "suid,allow_other"}) {
    std::vector<std::string> fuse_opt = {named};
    EXPECT_THAT(BuildMountOptions(fuse_opt),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("allow_other is redundant")));
  }
}

TEST(MountOptionsTest, HasDefaultPermissions) {
  EXPECT_TRUE(HasDefaultPermissions(std::vector<std::string>{
      "default_permissions"}));
  EXPECT_TRUE(HasDefaultPermissions(std::vector<std::string>{
      "allow_other,default_permissions"}));
  EXPECT_FALSE(HasDefaultPermissions(std::vector<std::string>{}));
  EXPECT_FALSE(HasDefaultPermissions(std::vector<std::string>{
      "allow_other", "default_permissions_not", "xdefault_permissions"}));
}

}  // namespace
}  // namespace dcfs
