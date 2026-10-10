// Known-good input of check_in_production.query: a test may crash.
#include "absl/log/check.h"

void TestsMayCheck(int x) { CHECK(x > 0); }
