// Known-good input of .clang-tidy: no check of the configuration finds
// anything here. The file is wrapped in a header guard that
// tools/approved_macros.txt does not list: guards are exempt in general.
#ifndef DCFS_TIDY_GOOD_GUARD_H_
#define DCFS_TIDY_GOOD_GUARD_H_

namespace {

[[maybe_unused]] int ReturnsEarly(int x) {
  if (x > 0) {
    return 1;
  }
  return 2;
}

[[maybe_unused]] int Straight(int x) {
  if (x > 0) {
    x = 1;
  }
  x = 2;
  return x;
}

class Explicit {
 public:
  explicit Explicit(int value) : value_(value) {}
  [[nodiscard]] int Value() const { return value_; }

 private:
  int value_;
};

[[maybe_unused]] int UsesExplicit() { return Explicit(1).Value(); }

}  // namespace

namespace {
namespace good_namespace {

class GoodClass {
 public:
  explicit GoodClass(int value) : value_(value) {}
  [[nodiscard]] int Value() const { return value_; }

 private:
  int value_;
};

struct GoodStruct {
  int plain_member = 0;
};

enum class GoodEnum { kFirst, kSecond };

constexpr int kGoodConstant = 1;

[[maybe_unused]] int GoodFunction(int good_parameter) {
  int good_local = good_parameter + kGoodConstant;
  GoodStruct good_struct;
  good_struct.plain_member = static_cast<int>(GoodEnum::kSecond);
  return good_local + GoodClass(good_struct.plain_member).Value();
}

constexpr int kGlobal = 1;
const int kGlobalConst = 2;

// Automatic variables, const or constexpr, are lower_case or kName.
[[maybe_unused]] int Automatics() {
  constexpr int local_constexpr = 1;
  const int local_const = 2;
  const int kLocalConst = 3;
  static const int kStaticLocal = 4;
  return local_constexpr + local_const + kLocalConst + kStaticLocal;
}

// NOLINTNEXTLINE(modernize-use-using): a typedef exercises TypedefCase
typedef int GoodTypedef;
using GoodAlias = GoodTypedef;

union GoodUnion {
  int member;
};

template <typename ElementType>
struct GoodTemplate {
  ElementType member;
};

class ClassMembers {
 public:
  static constexpr int kClassConstant = 3;
  [[nodiscard]] int Value() const { return protected_ + pool_; }

 protected:
  int protected_ = 0;

 private:
  static int pool_;
};

int ClassMembers::pool_ = 0;

// Granted macros: a header guard, a private helper, a name that mirrors a
// Google-internal macro.
#define DCFS_FIXTURE_H_
#define DCFS_RET_CHECK_OP_(left, right) ((left) == (right))
#define RET_CHECK(condition) (condition)

[[maybe_unused]] int UsesAll() {
  return kGlobal + kGlobalConst + RET_CHECK(true) + DCFS_RET_CHECK_OP_(1, 1) +
         GoodTemplate<GoodAlias>{1}.member + GoodUnion{2}.member +
         ClassMembers::kClassConstant + ClassMembers().Value();
}

}  // namespace good_namespace
}  // namespace

#endif  // DCFS_TIDY_GOOD_GUARD_H_
