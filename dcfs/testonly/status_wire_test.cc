// SPDX-License-Identifier: MIT
#include "dcfs/testonly/status_wire.h"

#include <cerrno>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "dcfs/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

absl::Status RoundTrip(const absl::Status &status) {
  absl::StatusOr<absl::Status> parsed =
      testonly::ParseStatus(testonly::SerializeStatus(status));
  EXPECT_TRUE(parsed.ok()) << parsed.status();
  return parsed.ok() ? *parsed : absl::InternalError("did not parse");
}

TEST(StatusWireTest, OkStaysOk) {
  EXPECT_TRUE(RoundTrip(absl::OkStatus()).ok());
}

TEST(StatusWireTest, TheCodeAndTheMessageSurvive) {
  EXPECT_THAT(RoundTrip(absl::PermissionDeniedError("no entry")),
              StatusIs(absl::StatusCode::kPermissionDenied, "no entry"));
}

TEST(StatusWireTest, AMessageThatIsNotTextSurvives) {
  const std::string bytes("a\xff\xfe\0b\n:", 7);
  EXPECT_THAT(RoundTrip(absl::UnknownError(bytes)),
              StatusIs(absl::StatusCode::kUnknown, bytes));
}

TEST(StatusWireTest, EveryPayloadSurvives) {
  absl::Status status = absl::AbortedError("x");
  status.SetPayload("type/one", absl::Cord("first"));
  status.SetPayload("type/two", absl::Cord(std::string("\0\xff", 2)));
  absl::Status back = RoundTrip(status);
  EXPECT_EQ(back.GetPayload("type/one"), absl::Cord("first"));
  EXPECT_EQ(back.GetPayload("type/two"), absl::Cord(std::string("\0\xff", 2)));
  EXPECT_FALSE(back.GetPayload("type/three").has_value());
}

TEST(StatusWireTest, AnErrnoStatusKeepsItsExactMessageAndItsErrno) {
  absl::Status back = RoundTrip(ErrnoToStatus(ENOENT, "open x"));
  EXPECT_THAT(back, StatusIs(absl::StatusCode::kNotFound,
                             "open x: No such file or directory"));
  EXPECT_EQ(StatusToErrno(back), ENOENT);
}

TEST(StatusWireTest, BytesThatAreNotAStatusAreRejected) {
  EXPECT_THAT(testonly::ParseStatus("\xff\xff\xff"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("not a serialized Status")));
}

}  // namespace
}  // namespace dcfs
