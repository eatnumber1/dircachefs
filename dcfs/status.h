#ifndef DCFS_STATUS_H_
#define DCFS_STATUS_H_

#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/status/statusor.h"
#include "absl/types/source_location.h"

namespace dcfs {

constexpr inline std::string_view kErrnoTypeUrl = "rus.har.mn/dcfs/status/errno";

// All syscall/libc failures in dcfs/ must be turned into a Status via this
// function (dcfs::ErrnoToStatus), never via absl::ErrnoToStatus directly:
// only this wrapper attaches the kErrnoTypeUrl payload that
// GetErrnoFromStatus()/StatusToErrno() and callers such as
// backing::ReadGeneration() rely on to recover the original errno.
absl::Status ErrnoToStatus(int error_number, std::string_view message);
// Builders for new errors that do not come from a syscall (docs/style.md
// 1.6). Abseil has no absl::InternalErrorBuilder and the like. Each returns
// an absl::StatusBuilder with the code, so the message is streamed:
//
//   return InternalErrorBuilder() << "fuse_session_new failed";
//
// `loc` defaults to the caller's line, which the status then records.
inline absl::StatusBuilder InternalErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kInternal, loc);
}
inline absl::StatusBuilder FailedPreconditionErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kFailedPrecondition, loc);
}
inline absl::StatusBuilder NotFoundErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kNotFound, loc);
}
inline absl::StatusBuilder InvalidArgumentErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kInvalidArgument, loc);
}
inline absl::StatusBuilder AbortedErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kAborted, loc);
}
inline absl::StatusBuilder UnimplementedErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kUnimplemented, loc);
}
inline absl::StatusBuilder AlreadyExistsErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kAlreadyExists, loc);
}
inline absl::StatusBuilder ResourceExhaustedErrorBuilder(
    absl::SourceLocation loc = absl::SourceLocation::current()) {
  return absl::StatusBuilder(absl::StatusCode::kResourceExhausted, loc);
}

absl::StatusOr<int> GetErrnoFromStatus(const absl::Status &status);

absl::StatusOr<int> ErrorNameToErrno(std::string_view error_name);
std::string ErrnoToErrorName(int error_number);

// The full name -> errno table used by ErrorNameToErrno(), exposed for
// tests that need to exercise the round trip for every known entry.
const absl::flat_hash_map<std::string, int> &ErrnoNameTable();

// Returns the errno corresponding to `status`: the errno payload
// (kErrnoTypeUrl, as set by ErrnoToStatus) if present, otherwise a fixed
// StatusCode->errno mapping. Returns 0 for absl::OkStatus().
int StatusToErrno(const absl::Status &status);

}  // namespace dcfs

#endif  // DCFS_STATUS_H_
