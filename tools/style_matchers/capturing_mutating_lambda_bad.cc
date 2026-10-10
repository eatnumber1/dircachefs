// Known-bad input of capturing_mutating_lambda.query.
#include "absl/status/status.h"

void MutatingLambdas() {
  int count = 0;
  absl::Status status;
  auto bump = [&]() { ++count; };  // HIT
  auto add = [&count]() { count += 2; };  // HIT
  auto set = [&]() { status = absl::OkStatus(); };  // HIT
  bump();
  add();
  set();
  status.IgnoreError();
}
