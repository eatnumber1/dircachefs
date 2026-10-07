#include "dcfs/ret_check.h"

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

absl::Status CheckTrue() {
  RET_CHECK(1 == 1);
  return absl::OkStatus();
}

absl::Status CheckFalse() {
  RET_CHECK(1 == 2);
  return absl::OkStatus();
}

absl::Status CheckFalseWithContext() {
  RET_CHECK(1 == 2) << "extra context";
  return absl::OkStatus();
}

absl::StatusOr<int> CheckFalseInStatusOrFunction() {
  RET_CHECK(false);
  return 42;
}

enum class Color { kRed = 1, kBlue = 2 };

absl::Status CheckScopedEnumEqFails() {
  RET_CHECK_EQ(Color::kRed, Color::kBlue);
  return absl::OkStatus();
}

absl::Status CheckEqFails() {
  RET_CHECK_EQ(1 + 1, 3);
  return absl::OkStatus();
}

absl::Status CheckNeFails() {
  RET_CHECK_NE(5, 5);
  return absl::OkStatus();
}

absl::Status CheckOkPropagatesFailure(const absl::Status &status) {
  RET_CHECK_OK(status);
  return absl::OkStatus();
}

absl::Status CheckOkPasses() {
  RET_CHECK_OK(absl::OkStatus());
  return absl::OkStatus();
}

int g_eval_count = 0;
int EvaluatedOnce() {
  ++g_eval_count;
  return 5;
}

absl::Status CheckEqEvaluatesOperandsOnce() {
  RET_CHECK_EQ(EvaluatedOnce(), 5);
  return absl::OkStatus();
}

TEST(RetCheckTest, PassingCheckFallsThrough) {
  EXPECT_THAT(CheckTrue(), IsOk());
}

TEST(RetCheckTest, FailingCheckIsInternal) {
  absl::Status st = CheckFalse();
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(st.message(), HasSubstr("1 == 2"));
}

TEST(RetCheckTest, ContextIsAppended) {
  absl::Status st = CheckFalseWithContext();
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(st.message(), HasSubstr("extra context"));
}

TEST(RetCheckTest, UsableInStatusOrReturningFunction) {
  absl::StatusOr<int> result = CheckFalseInStatusOrFunction();
  EXPECT_THAT(result, StatusIs(absl::StatusCode::kInternal));
}

TEST(RetCheckTest, EqFailureMessageHasBothOperands) {
  absl::Status st = CheckEqFails();
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(st.message(), HasSubstr("2"));
  EXPECT_THAT(st.message(), HasSubstr("3"));
}

TEST(RetCheckTest, NeFailureMessageHasOperand) {
  absl::Status st = CheckNeFails();
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(st.message(), HasSubstr("5"));
}

TEST(RetCheckTest, OkPropagatesNonOkStatusAsInternal) {
  absl::Status original = absl::NotFoundError("the thing was not found");
  absl::Status st = CheckOkPropagatesFailure(original);
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(st.message(), HasSubstr("the thing was not found"));
}

TEST(RetCheckTest, OkPassesThroughOnOkStatus) {
  EXPECT_THAT(CheckOkPasses(), IsOk());
}

TEST(RetCheckTest, OperandsEvaluatedExactlyOnce) {
  g_eval_count = 0;
  EXPECT_THAT(CheckEqEvaluatesOperandsOnce(), IsOk());
  EXPECT_EQ(g_eval_count, 1);
}

// A scoped enum has no operator<<, so RET_CHECK_EQ prints its underlying
// value.
TEST(RetCheckTest, ScopedEnumOperandsPrintTheirValues) {
  EXPECT_THAT(CheckScopedEnumEqFails(),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("(1 vs 2)")));
}

}  // namespace
}  // namespace dcfs
