// Self-check of the UBSan build (step 7.4): the checks that matter most are
// compiled in, linked with their runtime (the vptr check lives in the C++
// part, ubsan_standalone_cxx) and fatal (-fno-sanitize-recover=all), so a
// report fails the test. Only built under --config=ubsan (see BUILD.bazel).
#include <climits>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"

namespace {

// Keeps the compiler from folding the undefined operation away.
void Escape(void *p) { asm volatile("" : : "g"(p) : "memory"); }

[[gnu::noinline]] int AddOne(int x) { return x + 1; }

// The load is through a misaligned int*; out of line so it is not folded.
[[gnu::noinline]] int LoadInt(const char *p) {
  return *reinterpret_cast<const int *>(p);
}

struct Base {
  virtual ~Base() = default;
  virtual int Id() const { return 1; }
};
struct Derived : Base {
  int Id() const override { return 2; }
  int extra = 0;
};

[[gnu::noinline]] int CallThroughDerived(Base *b) {
  return static_cast<Derived *>(b)->extra;
}

TEST(UbsanRuntimeTest, ReportsSignedIntegerOverflow) {
  EXPECT_DEATH(
      {
        int x = INT_MAX;
        Escape(&x);
        int y = AddOne(x);
        Escape(&y);
      },
      "runtime error: signed integer overflow");
}

TEST(UbsanRuntimeTest, ReportsMisalignedLoad) {
  EXPECT_DEATH(
      {
        alignas(8) char buf[16] = {};
        const char *p = buf + 1;
        Escape(&p);
        int v = LoadInt(p);
        Escape(&v);
      },
      "runtime error: load of misaligned address");
}

TEST(UbsanRuntimeTest, ReportsVptrMisuse) {
  EXPECT_DEATH(
      {
        Base b;
        Base *p = &b;
        Escape(&p);
        int v = CallThroughDerived(p);
        Escape(&v);
      },
      "runtime error: downcast of address .* which does not point to an "
      "object of type 'Derived'");
}

}  // namespace
