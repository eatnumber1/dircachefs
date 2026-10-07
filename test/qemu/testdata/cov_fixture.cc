// A known-covered and a known-uncovered function, for
// //test/qemu:coverage_pipeline_test: the lcov the pipeline writes must show
// Covered() with hits and Uncovered() with none.
#include <cstdio>

extern "C" int Covered(int x) { return x + 1; }

extern "C" int Uncovered(int x) { return x - 1; }

int main() {
  std::printf("%d\n", Covered(1));
  return 0;
}
