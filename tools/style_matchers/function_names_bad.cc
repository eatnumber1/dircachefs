// Known-bad input of function_names.query: lower-case functions and methods
// outside the syscall wrappers' exception, and a wrapper-shaped function in
// a file that is not one of the syscalls files.
namespace dcfs {

int lower_free_function() { return 1; }  // HIT

class Widget {
 public:
  int lower_method(int x) const { return x; }  // HIT
  static int lower_static() { return 3; }  // HIT
  int CamelMethod() const { return 4; }
  int compute(int x) { return x; }  // HIT
  void stop() {}  // HIT
  void set_two(int, int) {}  // HIT
  static int static_getter() { return 3; }  // HIT
  int out_lower(int x) const;  // HIT
  friend int friend_lower(const Widget &) { return 7; }  // HIT
};

int Widget::out_lower(int x) const { return x; }  // HIT

// A project macro does not hide a lower-case name.
#define MINE(name) \
  int name() { return 9; }
MINE(mine_lower)  // HIT

template <typename T>
T template_lower(T value) {  // HIT
  return value;
}

[[maybe_unused]] int UsesTemplate() { return template_lower(1); }

namespace syscalls {

// In namespace dcfs::syscalls, but this file is not a syscalls file.
int read_here() { return 5; }  // HIT

}  // namespace syscalls

namespace other {

// Not dcfs::syscalls.
int read() { return 6; }  // HIT

}  // namespace other

}  // namespace dcfs

extern "C" int c_def() { return 0; }  // HIT
