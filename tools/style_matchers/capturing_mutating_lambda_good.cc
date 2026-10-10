// Known-good input of capturing_mutating_lambda.query: a lambda that reads
// what it captures, and one that changes only its own locals.
int ReadingLambdas() {
  int count = 3;
  auto read = [&]() { return count + 1; };
  auto own = []() {
    int local = 0;
    ++local;
    local += 2;
    return local;
  };
  return read() + own();
}
