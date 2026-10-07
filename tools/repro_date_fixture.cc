// Embeds the date and time of the build (kBuilt comes from the genrule that
// writes repro_date_stamp.h with `date`): what a build that does not redact
// __DATE__ and __TIME__ (toolchains_llvm does, -D__DATE__="redacted") would
// have, for //tools:repro_compare_self_check_test.
#include <cstdio>

#include "tools/repro_date_stamp.h"

int main() {
  std::puts(kBuilt);
  return 0;
}
