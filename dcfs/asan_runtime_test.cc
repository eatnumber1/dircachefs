// Self-check of the ASan build (step 7.1, review): the sanitizer's C++
// runtime is linked, so ASan reports the C++-only errors. Only built under
// --config=asan (see BUILD.bazel).
#include <cstdlib>

#include "gtest/gtest.h"

namespace {

// Keeps the compiler from pairing (and dropping) the new and the delete.
void Escape(void *p) { asm volatile("" : : "g"(p) : "memory"); }

// Out of line, so the compiler cannot see the mismatch (-Wmismatched-new-
// delete) or pair the calls.
[[gnu::noinline]] int *NewArray() { return new int[4]; }
[[gnu::noinline]] void DeleteScalar(int *p) { delete p; }

TEST(AsanRuntimeTest, ReportsNewArrayDeletedAsScalar) {
  EXPECT_DEATH(
      {
        int *p = NewArray();
        Escape(p);
        DeleteScalar(p);
      },
      "alloc-dealloc-mismatch");
}

TEST(AsanRuntimeTest, ReportsMallocFreedWithDelete) {
  EXPECT_DEATH(
      {
        void *p = malloc(16);
        Escape(p);
        delete static_cast<char *>(p);
      },
      "alloc-dealloc-mismatch");
}

}  // namespace
