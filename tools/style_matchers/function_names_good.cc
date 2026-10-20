// Known-good input of function_names.query: CamelCase functions and methods,
// the members the language names (constructors, destructors, operators,
// conversions, main), and the lower-case man-page names of the wrappers in
// dcfs::syscalls, which this fixture stands in a syscalls file for, and the
// link-time fakes the linker's --wrap names (__wrap_open).
#include <exception>

namespace dcfs {

int CamelFree() { return 1; }

class Widget {
 public:
  Widget() = default;
  ~Widget() = default;
  explicit operator bool() const { return true; }
  bool operator==(const Widget &) const { return true; }
  int CamelMethod() const { return 2; }
  static int StaticMethod() { return 3; }
  // Google's accessors: a getter takes no parameter, a setter is set_name.
  [[nodiscard]] int size() const { return size_; }
  [[nodiscard]] int &count() { return count_; }
  void set_size(int value) { size_ = value; }

 private:
  int size_ = 0;
  int count_ = 0;
};

namespace syscalls {

int read() { return 4; }
int openat() { return 5; }

}  // namespace syscalls

}  // namespace dcfs

int __wrap_open() { return 6; }
int __real_open() { return 7; }

// gtest names what INSTANTIATE_TEST_SUITE_P expands to.
#define INSTANTIATE_TEST_SUITE_P(name) \
  int registered_##name() { return 8; }
INSTANTIATE_TEST_SUITE_P(Suite)

// An override of an external virtual is not ours to name.
struct Failure : std::exception {
  [[nodiscard]] const char *what() const noexcept override { return "x"; }
};

template <typename T>
T Twice(T value) {
  return value + value;
}

[[maybe_unused]] int Uses() {
  auto lambda = [](int x) { return x; };
  return Twice(lambda(1));
}

int main() { return 0; }
