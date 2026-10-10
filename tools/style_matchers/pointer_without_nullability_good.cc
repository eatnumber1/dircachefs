// Known-good input of pointer_without_nullability.query: every pointer says
// whether it may be null; references and non-pointers need nothing.
#include <memory>

#include "absl/base/nullability.h"

struct Holder {
  int *absl_nonnull field;
  std::unique_ptr<int> absl_nullable owned;
  int &reference;
  int value;
};

int *absl_nullable Returned();
void Parameter(const char *absl_nonnull name, const int &reference);
