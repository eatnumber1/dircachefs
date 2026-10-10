// Known-good input of status_uninitialized.query: every Status is made by
// the statement that declares it.
#include "absl/status/status.h"
#include "absl/status/statusor.h"

absl::Status Fetch();
absl::StatusOr<int> Count();

void InitializedStatuses(const absl::Status &parameter) {
  absl::Status fetched = Fetch();
  absl::StatusOr<int> counted = Count();
  absl::Status copied(parameter);
  fetched.IgnoreError();
  counted.status().IgnoreError();
  copied.IgnoreError();
}
