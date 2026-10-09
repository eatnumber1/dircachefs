#include "dcfs/status.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/types/source_location.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

TEST(ErrnoNameRoundTripTest, AliasNamesAreKnown) {
  // Names that share a value with the canonical one strerrorname_np gives.
  EXPECT_THAT(ErrorNameToErrno("EOPNOTSUPP"), IsOkAndHolds(ENOTSUP));
  EXPECT_THAT(ErrorNameToErrno("ENOTSUP"), IsOkAndHolds(ENOTSUP));
}

TEST(ErrnoNameRoundTripTest, ExhaustiveOverAllErrnoValues) {
  // Every errno value the C library knows a canonical name for (via
  // strerrorname_np) must round-trip both ways through our own table:
  // name -> errno must recover the original number, and the canonical name
  // ErrnoToErrorName() produces for that number must itself map back to it.
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

// docs/style.md 1.7: the handler that replies logs an error at ERROR only if
// dcfs produced it. An errno from a syscall is the backing filesystem's
// answer; a status with no errno, or built by DcfsErrnoToStatus, is dcfs's.
TEST(ProducedByDcfsTest, OnlyAForwardedErrnoIsNotDcfsOwn) {
  EXPECT_FALSE(ProducedByDcfs(absl::OkStatus()));
  EXPECT_FALSE(ProducedByDcfs(ErrnoToStatus(ENOENT, "openat")));
  EXPECT_TRUE(ProducedByDcfs(InternalErrorBuilder() << "broken"));
  EXPECT_TRUE(ProducedByDcfs(absl::AbortedError("sqlite busy")));
  EXPECT_TRUE(ProducedByDcfs(DcfsErrnoToStatus(EAGAIN, "kept changing")));
}

// An errno that describes dcfs's own process (it ran out of descriptors or
// memory, or has a bug), not the backing filesystem's answer, is dcfs's.
TEST(ProducedByDcfsTest, ErrnosOfDcfsOwnProcessAreDcfsOwn) {
  for (int err : {EMFILE, ENFILE, ENOMEM, EBADF, EFAULT}) {
    EXPECT_TRUE(ProducedByDcfs(ErrnoToStatus(err, "openat"))) << err;
  }
  EXPECT_FALSE(ProducedByDcfs(ErrnoToStatus(EEXIST, "mkdirat")));
}

TEST(ProducedByDcfsTest, MarkProducedByDcfsKeepsTheErrno) {
  absl::Status marked = MarkProducedByDcfs(ErrnoToStatus(ENOENT, "openat"));
  EXPECT_TRUE(ProducedByDcfs(marked));
  EXPECT_EQ(StatusToErrno(marked), ENOENT);
  EXPECT_TRUE(MarkProducedByDcfs(absl::OkStatus()).ok());
}

TEST(ProducedByDcfsTest, DcfsErrnoToStatusKeepsTheErrno) {
  absl::Status status = DcfsErrnoToStatus(ENOTSUP, "refused");
  EXPECT_THAT(GetErrnoFromStatus(status), IsOkAndHolds(ENOTSUP));
  EXPECT_EQ(StatusToErrno(status), ENOTSUP);
  absl::Status with_context = absl::StatusBuilder(status) << "while testing";
  EXPECT_TRUE(ProducedByDcfs(with_context));
}

// The builder is made on the line of the macro call, so its recorded source
// location must be that line.
#define EXPECT_HELPER(helper, expected_code)                              \
  do {                                                                    \
    absl::StatusBuilder builder = helper();                               \
    EXPECT_EQ(builder.source_location().line(), __LINE__);                \
    EXPECT_NE(std::string(builder.source_location().file_name())          \
                  .find("status_test.cc"),                                \
              std::string::npos);                                         \
    absl::Status status = std::move(builder) << "went wrong: " << 42;     \
    EXPECT_EQ(status.code(), expected_code);                              \
    EXPECT_EQ(status.message(), "went wrong: 42");                        \
    /* The Status itself records the call site, not only the builder. */  \
    ASSERT_FALSE(status.GetSourceLocations().empty());                    \
    EXPECT_EQ(status.GetSourceLocations().front().line(), __LINE__);      \
    EXPECT_FALSE(status.GetPayload(kErrnoTypeUrl).has_value());           \
  } while (0)

TEST(ErrorBuilderTest, EachHelperHasItsCodeMessageAndCallerLine) {
  EXPECT_HELPER(InternalErrorBuilder, absl::StatusCode::kInternal);
  EXPECT_HELPER(FailedPreconditionErrorBuilder,
                absl::StatusCode::kFailedPrecondition);
  EXPECT_HELPER(NotFoundErrorBuilder, absl::StatusCode::kNotFound);
  EXPECT_HELPER(InvalidArgumentErrorBuilder,
                absl::StatusCode::kInvalidArgument);
  EXPECT_HELPER(AbortedErrorBuilder, absl::StatusCode::kAborted);
  EXPECT_HELPER(UnimplementedErrorBuilder, absl::StatusCode::kUnimplemented);
  EXPECT_HELPER(AlreadyExistsErrorBuilder, absl::StatusCode::kAlreadyExists);
  EXPECT_HELPER(ResourceExhaustedErrorBuilder,
                absl::StatusCode::kResourceExhausted);
}

TEST(ErrorBuilderTest, ConvertsToStatusOrAndKeepsAnExplicitLocation) {
  auto f = []() -> absl::StatusOr<int> {
    return NotFoundErrorBuilder() << "no row " << 7;
  };
  EXPECT_THAT(f(), StatusIs(absl::StatusCode::kNotFound, "no row 7"));
  absl::StatusBuilder b = InternalErrorBuilder(absl::SourceLocation::current());
  EXPECT_EQ(b.source_location().line(), __LINE__ - 1);
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

// Every code of the fallback table (status.cc's StatusCodeToErrno), which
// serves a status that carries no errno payload: the code of a failure that
// did not come from a syscall. kOk is not here: StatusToErrno answers 0 for
// an ok status before the table is consulted.
TEST(StatusToErrnoTest, CodeTableHasAnErrnoForEveryCode) {
  const struct {
    absl::StatusCode code;
    int want;
  } kCases[] = {
      {absl::StatusCode::kCancelled, ECANCELED},
      {absl::StatusCode::kUnknown, EPROTO},
      {absl::StatusCode::kInvalidArgument, EINVAL},
      {absl::StatusCode::kDeadlineExceeded, ETIMEDOUT},
      {absl::StatusCode::kNotFound, ENOENT},
      {absl::StatusCode::kAlreadyExists, EEXIST},
      {absl::StatusCode::kPermissionDenied, EPERM},
      {absl::StatusCode::kResourceExhausted, ENOSPC},
      {absl::StatusCode::kFailedPrecondition, EBUSY},
      {absl::StatusCode::kAborted, EDEADLK},
      {absl::StatusCode::kOutOfRange, ERANGE},
      {absl::StatusCode::kUnimplemented, ENOSYS},
      {absl::StatusCode::kInternal, ELIBBAD},
      {absl::StatusCode::kUnavailable, EAGAIN},
      {absl::StatusCode::kDataLoss, ENOTRECOVERABLE},
      {absl::StatusCode::kUnauthenticated, EPERM},
  };
  for (const auto &[code, want] : kCases) {
    EXPECT_EQ(StatusToErrno(absl::Status(code, "x")), want)
        << "code " << absl::StatusCodeToString(code);
  }
}

TEST(StatusToErrnoTest, ACodeOutsideTheEnumIsAProtocolError) {
  // absl keeps a code it does not know as it came (a status from a newer
  // library, a corrupted one): the table's default.
  EXPECT_EQ(
      StatusToErrno(absl::Status(static_cast<absl::StatusCode>(4242), "x")),
      EPROTO);
}

TEST(StatusToErrnoTest, AnUnreadablePayloadFallsBackToTheCode) {
  // A payload whose name is not an errno name (written by something else, or
  // by a build whose table differs) must not be trusted: the code decides.
  absl::Status status = absl::NotFoundError("x");
  status.SetPayload(kErrnoTypeUrl, absl::Cord("NOT_AN_ERRNO_NAME"));
  EXPECT_THAT(GetErrnoFromStatus(status),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_EQ(StatusToErrno(status), ENOENT);
}

}  // namespace
}  // namespace dcfs
