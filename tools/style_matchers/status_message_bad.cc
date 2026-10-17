// Known-bad input of status_message.query.
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

absl::Status Fetch();
absl::StatusOr<int> Count();

std::string Describe() {
  absl::Status status = Fetch();
  absl::StatusOr<int> count = Count();
  std::string a = absl::StrCat("fetch: ", status.message());  // HIT
  std::string b = absl::StrCat("count: ", count.status().message());  // HIT
  const absl::Status &ref = status;
  return a + b + std::string(ref.message());  // HIT
}
