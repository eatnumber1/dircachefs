// MainInvariantChecks() for production binaries: nothing is checked. The
// testonly checking build links dcfs/testonly/main_invariant_checker.cc
// instead (see dcfs/invariant_checks.h).

#include "dcfs/invariant_checks.h"

namespace dcfs {

InvariantChecks &MainInvariantChecks() { return NoInvariantChecks(); }

}  // namespace dcfs
