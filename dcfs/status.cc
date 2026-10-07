#include "dcfs/status.h"

#include <cerrno>
#include <optional>

#include "absl/strings/cord.h"

namespace dcfs {

absl::Status ErrnoToStatus(int error_number, std::string_view message) {
  absl::Status status = absl::ErrnoToStatus(error_number, message);
  status.SetPayload(kErrnoTypeUrl, absl::Cord(ErrnoToErrorName(error_number)));
  return status;
}

absl::StatusOr<int> GetErrnoFromStatus(const absl::Status &status) {
  if (status.ok()) return 0;

  std::optional<absl::Cord> payload = status.GetPayload(kErrnoTypeUrl);
  if (!payload) {
    return NotFoundErrorBuilder()
           << "cannot get errno from Status: no payload in Status: " << status;
  }

  return ErrorNameToErrno(std::string(*payload));
}

namespace {

// StatusCode -> errno fallback used by StatusToErrno when a status carries
// no errno payload, so that all status<->errno logic lives in one place.
int StatusCodeToErrno(absl::StatusCode code) {
  switch (code) {
    case absl::StatusCode::kOk:
      return 0;
    case absl::StatusCode::kInvalidArgument:
      return EINVAL;
    case absl::StatusCode::kDeadlineExceeded:
      return ETIMEDOUT;
    case absl::StatusCode::kNotFound:
      return ENOENT;
    case absl::StatusCode::kAlreadyExists:
      return EEXIST;
    case absl::StatusCode::kPermissionDenied:
      [[fallthrough]];
    case absl::StatusCode::kUnauthenticated:
      return EPERM;
    case absl::StatusCode::kOutOfRange:
      return ERANGE;
    case absl::StatusCode::kFailedPrecondition:
      return EBUSY;
    case absl::StatusCode::kResourceExhausted:
      return ENOSPC;
    case absl::StatusCode::kCancelled:
      return ECANCELED;
    case absl::StatusCode::kAborted:
      return EDEADLK;
    case absl::StatusCode::kUnimplemented:
      return ENOSYS;
    case absl::StatusCode::kUnavailable:
      return EAGAIN;
    case absl::StatusCode::kDataLoss:
      return ENOTRECOVERABLE;
    case absl::StatusCode::kInternal:
      return ELIBBAD;
    case absl::StatusCode::kUnknown:
      [[fallthrough]];
    default:
      return EPROTO;
  }
}

}  // namespace

int StatusToErrno(const absl::Status &status) {
  if (status.ok()) return 0;
  if (absl::StatusOr<int> eno = GetErrnoFromStatus(status); eno.ok()) {
    return *eno;
  }
  return StatusCodeToErrno(status.code());
}

}  // namespace dcfs
