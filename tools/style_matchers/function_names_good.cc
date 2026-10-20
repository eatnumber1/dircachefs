// Known-good input of function_names.query: CamelCase functions and methods,
// the members the language names (constructors, destructors, operators,
// conversions, main), and the lower-case man-page names of the wrappers in
// dcfs::syscalls, which this fixture stands in a syscalls file for, and the
// link-time fakes the linker's --wrap names (__wrap_open).
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
};

namespace syscalls {

int read() { return 4; }
int openat() { return 5; }

}  // namespace syscalls

}  // namespace dcfs

int __wrap_open() { return 6; }

int main() { return 0; }
