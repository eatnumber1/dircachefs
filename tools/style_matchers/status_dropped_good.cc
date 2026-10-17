// Known-good input of status_dropped.query: a failure branch that mentions
// the Status returns, logs or stringifies it.
#include <string>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

absl::Status Fetch();
absl::StatusOr<int> Count();

absl::Status Kept(std::string *out) {
  absl::Status status = Fetch();
  if (!status.ok()) return status;
  absl::StatusOr<int> count = Count();
  if (!count.ok()) {
    *out = absl::StrCat("count: ", count.status());
    return absl::OkStatus();
  }
  absl::Status again = Fetch();
  if (!again.ok()) LOG(ERROR) << again;
  return absl::OkStatus();
}
