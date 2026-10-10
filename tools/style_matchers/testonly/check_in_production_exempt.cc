// Known-good input of check_in_production.query: so may a testonly/ file.
#include "absl/log/check.h"

void TestOnlyMayCheck(int x) { CHECK(x > 0); }
