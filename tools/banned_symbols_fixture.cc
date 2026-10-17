// SPDX-License-Identifier: MIT
// Known-bad input of //tools:banned_symbols_self_check_test (plan step 26.8):
// a tiny program that calls realpath(3) and sleep(3), which
// tools/banned_symbols.txt bans, refers to usleep(3) from data alone (no
// function's code references it), to alarm(2) both from main and from data,
// and defines nftw64 (a banned symbol that is in the binary without any
// scanned object referencing it, as if a runtime had pulled it in). Never
// shipped.
#include <unistd.h>

#include <cstdlib>

[[gnu::used]] static int (*const kUsleep)(useconds_t) = ::usleep;
[[gnu::used]] static unsigned (*const kAlarm)(unsigned) = ::alarm;

extern "C" int nftw64() { return 0; }

int main(int argc, char **argv) {
  char *resolved = ::realpath(argc > 0 ? argv[0] : ".", nullptr);
  ::sleep(argc > 100 ? 1 : 0);  // a timer (step 15.6b)
  if (argc > 100) ::alarm(1);
  return (resolved == nullptr) + nftw64();
}
