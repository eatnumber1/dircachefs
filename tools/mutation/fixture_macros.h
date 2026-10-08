// Header of fixture.cc (a test fixture, plan step 26.5d): a macro defined
// outside the .cc, as ABSL_RETURN_IF_ERROR is. The AST nodes of its body
// spell in this file, so they must never become mutants of fixture.cc.

#define RETURN_IF_ERROR(expr)   \
  do {                          \
    absl::Status status_ = (expr); \
    if (!status_.ok()) return status_; \
  } while (false)
