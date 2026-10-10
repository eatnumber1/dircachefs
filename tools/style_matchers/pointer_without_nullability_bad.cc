// Known-bad input of pointer_without_nullability.query.
#include <memory>

struct Holder {
  int *field;  // HIT
  std::unique_ptr<int> owned;  // HIT
};

int *Returned();  // HIT
void Parameter(const char *name);  // HIT
