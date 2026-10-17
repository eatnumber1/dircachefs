// Known-good input of check_in_production.query: logging that does not end
// the process, and a forked child's _exit.
#include <unistd.h>

#include "absl/log/log.h"

void Reports(int x) {
  if (x < 0) {
    LOG(ERROR) << "negative";
  }
  LOG(INFO) << x;
}

void ChildAfterAFailedExec() { _exit(127); }
