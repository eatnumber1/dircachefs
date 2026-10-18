// SPDX-License-Identifier: MIT
#ifndef DCFS_TESTONLY_STATUS_WIRE_H_
#define DCFS_TESTONLY_STATUS_WIRE_H_

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs::testonly {

// `status` as bytes: its code, its message and every payload, so that what
// reads a Status (StatusToErrno reads the errno payload) reads the same from
// ParseStatus's result. The one place that serializes a Status.
std::string SerializeStatus(const absl::Status &status);

// The Status that SerializeStatus made `bytes` from; an error if `bytes` is
// not one.
absl::StatusOr<absl::Status> ParseStatus(std::string_view bytes);

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_STATUS_WIRE_H_
