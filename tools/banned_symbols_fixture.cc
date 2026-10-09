// Known-bad input of //tools:banned_symbols_self_check_test (plan step 26.8):
// a tiny program that calls realpath(3) and sleep(3), which
// tools/banned_symbols.txt bans, and defines nftw64 (a banned symbol that is in the binary without
// any scanned object referencing it, as if a runtime had pulled it in).
// Never shipped.
#include <unistd.h>

#include <cstdlib>

extern "C" int nftw64() { return 0; }

int main(int argc, char **argv) {
  char *resolved = ::realpath(argc > 0 ? argv[0] : ".", nullptr);
  ::sleep(argc > 100 ? 1 : 0);  // a timer (step 15.6b)
  return (resolved == nullptr) + nftw64();
}
