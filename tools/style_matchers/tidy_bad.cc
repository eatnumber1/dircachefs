// Known-bad input of .clang-tidy: each marked line is a finding of the check
// it names. (The matchers' fixtures are *_bad.cc; this one is for the tool
// that is not a matcher.)

int ElseAfterReturn(int x) {
  if (x > 0) {
    return 1;
  } else {  // HIT readability-else-after-return
    return 2;
  }
}

int ElseAfterBreak(int x) {
  int total = 0;
  for (int i = 0; i < x; ++i) {
    if (i == 3) {
      break;
    } else {  // HIT readability-else-after-return
      total += i;
    }
  }
  return total;
}

int ElseAfterContinue(int x) {
  int total = 0;
  for (int i = 0; i < x; ++i) {
    if (i == 3) {
      continue;
    } else {  // HIT readability-else-after-return
      total += i;
    }
  }
  return total;
}

int MisleadingIndentation(int x) {
  if (x > 0)
    x = 1;
    x = 2;  // HIT readability-misleading-indentation
  return x;
}

int Complicated(int a, int b, int c) {  // HIT readability-function-cognitive-complexity
  int total = 0;
  for (int i = 0; i < a; ++i) {
    if (i % 2 == 0) {
      for (int j = 0; j < b; ++j) {
        if (j > c) {
          total += j;
        } else if (j == c) {
          total -= 1;
        } else {
          while (total > 100) {
            total /= 2;
          }
        }
      }
    }
  }
  return total;
}

struct Converting {
  Converting(int value);  // HIT google-explicit-constructor
  int value;
};

// docs/style.md 1.2 (Google's naming): types and enumerators and constants
// are CamelCase and kName, variables and parameters lower_case, data members
// of a class name_, namespaces lower_case.
class bad_class {};  // HIT readability-identifier-naming

struct bad_struct {};  // HIT readability-identifier-naming

enum class BadEnum {
  bad_enumerator,  // HIT readability-identifier-naming
};

enum class bad_enum { kOnly };  // HIT readability-identifier-naming

[[maybe_unused]] constexpr int BadConstant = 1;  // HIT readability-identifier-naming

namespace BadNamespace {}  // HIT readability-identifier-naming

class BadMember {
 public:
  [[nodiscard]] int Value() const { return value; }

 private:
  int value = 0;  // HIT readability-identifier-naming
};

int BadVariable(int BadParameter) {  // HIT readability-identifier-naming
  int BadLocal = BadParameter;  // HIT readability-identifier-naming
  return BadLocal;
}

typedef int bad_typedef;  // HIT readability-identifier-naming

using bad_alias = int;  // HIT readability-identifier-naming

union bad_union {  // HIT readability-identifier-naming
  int member;
};

template <typename t>  // HIT readability-identifier-naming
struct TemplateParameter {
  t member;
};

class ProtectedMember {
 public:
  [[nodiscard]] int Value() const { return prot; }

 protected:
  int prot = 0;  // HIT readability-identifier-naming
};

[[maybe_unused]] const int bad_global = 1;  // HIT readability-identifier-naming

[[maybe_unused]] static const int bad_static = 2;  // HIT readability-identifier-naming

class ClassMembers {
 public:
  static constexpr int bad_class_constant = 3;  // HIT readability-identifier-naming
  static int pool;  // HIT readability-identifier-naming
};

int BadLocalConstant() {
  const int BadLocal = 4;  // HIT readability-identifier-naming
  return BadLocal;
}

#define bad_macro 5  // HIT readability-identifier-naming

#define BAD_TRAILING_ 6  // HIT readability-identifier-naming

struct PublicMember {
  int BadPublic;  // HIT readability-identifier-naming
};
