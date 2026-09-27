#include "dcfs/status.h"

#include <string.h>

#include <cerrno>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

TEST(ErrnoNameRoundTripTest, EveryTableEntryRoundTrips) {
  for (const auto &[name, value] : ErrnoNameTable()) {
    // name -> errno always round-trips to the table's own value, by
    // construction.
    EXPECT_THAT(ErrorNameToErrno(name), IsOkAndHolds(value)) << "name " << name;

    // errno -> name may produce a different (canonical, per
    // strerrorname_np) alias than `name` for numbers with more than one
    // valid name (e.g. ENOTSUP/EOPNOTSUPP), but that alias must itself be
    // in the table and round-trip back to the same errno value -- this is
    // the property ErrnoToStatus/GetErrnoFromStatus actually depend on.
    std::string canonical_name = ErrnoToErrorName(value);
    EXPECT_THAT(ErrorNameToErrno(canonical_name), IsOkAndHolds(value))
        << "errno " << value << " (table entry " << name
        << ", canonical name " << canonical_name << ")";
  }
}

TEST(ErrnoNameRoundTripTest, ExhaustiveOverAllErrnoValues) {
  // Every errno value the C library knows a canonical name for (via
  // strerrorname_np) must round-trip both ways through our own table:
  // name -> errno must recover the original number, and the canonical name
  // ErrnoToErrorName() produces for that number must itself map back to it.
  // Numeric aliases (e.g. ENOTSUP/EOPNOTSUPP, EAGAIN/EWOULDBLOCK,
  // EDEADLK/EDEADLOCK share one value on Linux) satisfy this trivially,
  // since both names are table entries for the very same numeric constant.
  for (int e = 1; e <= 134; ++e) {
    const char *name = strerrorname_np(e);
    if (name == nullptr) continue;

    EXPECT_THAT(ErrorNameToErrno(name), IsOkAndHolds(e))
        << "errno " << e << " (" << name << ")";

    std::string canonical_name = ErrnoToErrorName(e);
    EXPECT_THAT(ErrorNameToErrno(canonical_name), IsOkAndHolds(e))
        << "errno " << e << " (canonical name " << canonical_name << ")";
  }
}

TEST(ErrnoNameRoundTripTest, UnknownNumberRoundTrips) {
  // Not (plausibly) a real errno value on this platform.
  constexpr int kBogus = 999999;
  std::string name = ErrnoToErrorName(kBogus);
  EXPECT_THAT(ErrorNameToErrno(name), IsOkAndHolds(kBogus));
}

TEST(ErrnoNameRoundTripTest, UnknownNameFails) {
  EXPECT_THAT(ErrorNameToErrno("NOT_A_REAL_ERRNO_NAME"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(ErrnoPayloadTest, ErrnoToStatusSetsPayload) {
  absl::Status st = ErrnoToStatus(ENOENT, "open");
  EXPECT_THAT(GetErrnoFromStatus(st), IsOkAndHolds(ENOENT));
}

TEST(ErrnoPayloadTest, OkStatusHasNoPayload) {
  EXPECT_THAT(GetErrnoFromStatus(absl::OkStatus()), IsOkAndHolds(0));
}

TEST(ErrnoPayloadTest, MissingPayloadIsNotFound) {
  EXPECT_THAT(GetErrnoFromStatus(absl::InternalError("no payload here")),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(ErrnoPayloadTest, SurvivesStatusBuilderAnnotation) {
  absl::Status original = ErrnoToStatus(ENOENT, "open");
  absl::Status annotated =
      absl::StatusBuilder(original) << " while doing something";
  EXPECT_THAT(GetErrnoFromStatus(annotated), IsOkAndHolds(ENOENT));
}

TEST(ErrnoPayloadTest, SurvivesAbslReturnIfErrorAnnotation) {
  absl::Status original = ErrnoToStatus(ENOENT, "open");
  auto wrapper = [&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(original) << "while doing something";
    return absl::OkStatus();
  };
  absl::Status wrapped = wrapper();
  ASSERT_FALSE(wrapped.ok());
  EXPECT_THAT(GetErrnoFromStatus(wrapped), IsOkAndHolds(ENOENT));
}

TEST(StatusToErrnoTest, OkStatusIsZero) {
  EXPECT_EQ(StatusToErrno(absl::OkStatus()), 0);
}

TEST(StatusToErrnoTest, PrefersPayloadOverCodeTable) {
  // EACCES maps to absl::StatusCode::kPermissionDenied, whose code-table
  // fallback is EPERM (see StatusToErrno's table in status.cc); the payload
  // should win, giving back the original EACCES.
  absl::Status with_payload = ErrnoToStatus(EACCES, "open");
  ASSERT_EQ(with_payload.code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(StatusToErrno(with_payload), EACCES);

  // With the payload stripped, the same code falls back to the code table.
  absl::Status code_only(with_payload.code(), std::string(with_payload.message()));
  EXPECT_EQ(StatusToErrno(code_only), EPERM);
}

TEST(StatusToErrnoTest, CodeTableMapsPlainCodes) {
  EXPECT_EQ(StatusToErrno(absl::NotFoundError("x")), ENOENT);
  EXPECT_EQ(StatusToErrno(absl::InvalidArgumentError("x")), EINVAL);
  EXPECT_EQ(StatusToErrno(absl::UnimplementedError("x")), ENOSYS);
  EXPECT_EQ(StatusToErrno(absl::PermissionDeniedError("x")), EPERM);
}

}  // namespace
}  // namespace dcfs
