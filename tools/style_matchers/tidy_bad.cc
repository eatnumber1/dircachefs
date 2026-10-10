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
