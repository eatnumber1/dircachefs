// Known-bad input of happy_path_nested.query.
#include "absl/status/status.h"

void Step();

absl::Status Nested(const absl::Status &status) {
  if (status.ok()) {  // HIT
    Step();
    Step();
    Step();
    return absl::OkStatus();
  }
  return status;
}
