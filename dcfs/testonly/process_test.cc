// SPDX-License-Identifier: MIT
#include "dcfs/testonly/process.h"

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

TEST(RunCommandTest, ASucceedingCommandIsOk) {
  EXPECT_THAT(testonly::RunCommand({"/bin/true"}), IsOk());
}

TEST(RunCommandTest, ANonZeroExitNamesTheCommandAndItsStatus) {
  EXPECT_THAT(testonly::RunCommand({"/bin/sh", "-c", "exit 3"}),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("`/bin/sh -c exit 3` exited with status 3")));
}

TEST(RunCommandTest, ASignalNamesTheCommandAndTheSignal) {
  EXPECT_THAT(testonly::RunCommand({"/bin/sh", "-c", "kill -9 $$"}),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("was killed by signal 9")));
}

TEST(RunCommandTest, AMissingProgramExitsWith127) {
  EXPECT_THAT(testonly::RunCommand({"/no/such/program"}),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("exited with status 127")));
}

TEST(RunCommandTest, NoCommandIsAnError) {
  EXPECT_THAT(testonly::RunCommand({}),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("RunCommand of no command")));
}

TEST(DescribeWaitStatusTest, SaysHowAChildEnded) {
  EXPECT_EQ(testonly::DescribeWaitStatus(7 << 8), "exited with status 7");
  EXPECT_EQ(testonly::DescribeWaitStatus(9), "was killed by signal 9");
}

}  // namespace
}  // namespace dcfs
