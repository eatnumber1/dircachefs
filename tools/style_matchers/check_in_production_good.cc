// Known-good input of check_in_production.query: logging that does not end
// the process, and a forked child's _exit.
#include <unistd.h>

#include "absl/base/log_severity.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"

void Reports(int x) {
  if (x < 0) {
    LOG(ERROR) << "negative";
  }
  LOG(INFO) << x;
  // A severity chosen between two that are never fatal.
  LOG(LEVEL(x > 0 ? absl::LogSeverity::kInfo : absl::LogSeverity::kWarning))
      << x;
  LOG(LEVEL(absl::LogSeverity::kError)) << x;
  ABSL_RAW_LOG(WARNING, "raw %d", x);
}

void ChildAfterAFailedExec() { _exit(127); }
