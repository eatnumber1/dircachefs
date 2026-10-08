#include "dcfs/remount.h"

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

TEST(RemountDcfsTest, AMountpointThatIsNotThereFailsWithItsErrno) {
  absl::Status status = RemountDcfs("/no/such/mountpoint", true);
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kNotFound,
                               HasSubstr("/no/such/mountpoint")));
  EXPECT_EQ(StatusToErrno(status), ENOENT);
}

// A remount of anything else would change another filesystem's flags.
TEST(RemountDcfsTest, SomethingThatIsNotADcfsMountIsRefused) {
  EXPECT_THAT(RemountDcfs("/proc", true),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("not a dcfs mount")));
}

}  // namespace
}  // namespace dcfs
