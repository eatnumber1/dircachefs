// Known-bad input of status_dropped.query.
#include "absl/status/status.h"
#include "absl/status/statusor.h"

absl::Status Fetch();
absl::StatusOr<int> Count();

bool Dropped() {
  absl::Status status = Fetch();
  if (!status.ok()) return false;  // HIT
  absl::StatusOr<int> count = Count();
  if (!count.ok()) {  // HIT
    return false;
  }
  return true;
}
