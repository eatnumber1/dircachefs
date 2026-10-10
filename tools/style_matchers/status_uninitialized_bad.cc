// Known-bad input of status_uninitialized.query.
#include "absl/status/status.h"
#include "absl/status/statusor.h"

absl::Status Fetch();
absl::StatusOr<int> Count();

void UninitializedStatuses() {
  absl::Status status;  // HIT
  absl::StatusOr<int> count;  // HIT
  status = Fetch();
  count = Count();
  status.IgnoreError();
}
