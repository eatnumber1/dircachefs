// SPDX-License-Identifier: MIT
#include "dcfs/testonly/status_wire.h"

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "dcfs/status.h"
#include "dcfs/testonly/status_wire.pb.h"

namespace dcfs::testonly {

std::string SerializeStatus(const absl::Status &status) {
  StatusWire wire;
  wire.set_code(static_cast<int>(status.code()));
  wire.set_message(std::string(status.message()));
  status.ForEachPayload([&wire](std::string_view type_url,
                                const absl::Cord &value) {
    StatusWire::Payload *payload = wire.add_payloads();
    payload->set_type_url(std::string(type_url));
    payload->set_value(std::string(value));
  });
  return wire.SerializeAsString();
}

absl::StatusOr<absl::Status> ParseStatus(std::string_view bytes) {
  StatusWire wire;
  if (!wire.ParseFromString(bytes)) {
    return InvalidArgumentErrorBuilder()
           << "The bytes are not a serialized Status";
  }
  absl::Status status(static_cast<absl::StatusCode>(wire.code()),
                      wire.message());
  for (const StatusWire::Payload &payload : wire.payloads()) {
    status.SetPayload(payload.type_url(), absl::Cord(payload.value()));
  }
  return status;
}

}  // namespace dcfs::testonly
