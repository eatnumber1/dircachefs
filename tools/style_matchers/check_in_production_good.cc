// Known-good input of check_in_production.query: logging that does not end
// the process.
#include "absl/log/log.h"

void Reports(int x) {
  if (x < 0) {
    LOG(ERROR) << "negative";
  }
  LOG(INFO) << x;
}
