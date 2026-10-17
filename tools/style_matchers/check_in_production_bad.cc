// Known-bad input of check_in_production.query: a production-shaped file
// (not *_test.cc, not under testonly/) that crashes on purpose.
#include <cassert>
#include <cstdlib>
#include <exception>

#include "absl/log/check.h"
#include "absl/log/log.h"

void Crashes(int x) {
  CHECK(x > 0);  // HIT
  CHECK_NE(x, 3);  // HIT
  DCHECK_GT(x, 0);  // HIT
  QCHECK(x < 100);  // HIT
  LOG(FATAL) << "no";  // HIT
  assert(x != 7);  // HIT
  std::abort();  // HIT
}

void Exits(int x) {
  if (x == 1) std::exit(1);  // HIT
  if (x == 2) exit(2);  // HIT
  if (x == 3) std::quick_exit(3);  // HIT
  std::terminate();  // HIT
}
