#ifndef DCFS_STATUS_H_
#define DCFS_STATUS_H_

#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {

constexpr inline std::string_view kErrnoTypeUrl = "rus.har.mn/dcfs/status/errno";

// All syscall/libc failures in dcfs/ must be turned into a Status via this
// function (dcfs::ErrnoToStatus), never via absl::ErrnoToStatus directly:
// only this wrapper attaches the kErrnoTypeUrl payload that
// GetErrnoFromStatus()/StatusToErrno() and callers such as
// backing::ReadGeneration() rely on to recover the original errno.
absl::Status ErrnoToStatus(int error_number, absl::string_view message);
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
