// Known-bad input of check_in_production.query: a production-shaped file
// (not *_test.cc, not under testonly/) that crashes on purpose.
#include <cassert>
#include <cstdlib>
#include <exception>

#include "absl/base/log_severity.h"
// raw_logging.h is Abseil's own; its macros reach code of ours through
// raw_hash_set.h.
#include "absl/container/flat_hash_map.h"
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

// Fatal without NDEBUG, or fatal for some values: the severity is not one
// that is never fatal.
void MaybeFatal(int x, absl::LogSeverity severity) {
  LOG(DFATAL) << "no";  // HIT
  DLOG(FATAL) << "no";  // HIT
  LOG(LEVEL(severity)) << x;  // HIT
  ABSL_RAW_CHECK(x > 0, "x");  // HIT
  ABSL_RAW_LOG(FATAL, "no");  // HIT
  ABSL_RAW_LOG(DFATAL, "no");  // HIT
}
