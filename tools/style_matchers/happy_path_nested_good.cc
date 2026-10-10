// Known-good input of happy_path_nested.query: the error returns first and
// the happy path is the unindented one; a short success branch is allowed.
#include "absl/status/status.h"

void Step();

absl::Status Flat(const absl::Status &status) {
  if (!status.ok()) {
    return status;
  }
  Step();
  Step();
  Step();
  return absl::OkStatus();
}

void ShortBranch(const absl::Status &status) {
  if (status.ok()) {
    Step();
  }
}
