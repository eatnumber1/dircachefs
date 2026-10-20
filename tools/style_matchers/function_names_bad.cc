// Known-bad input of function_names.query: lower-case functions and methods
// outside the syscall wrappers' exception, and a wrapper-shaped function in
// a file that is not one of the syscalls files.
namespace dcfs {

int lower_free_function() { return 1; }  // HIT

class Widget {
 public:
  int lower_method() const { return 2; }  // HIT
  static int lower_static() { return 3; }  // HIT
  int CamelMethod() const { return 4; }
};

namespace syscalls {

// In namespace dcfs::syscalls, but this file is not a syscalls file.
int read_here() { return 5; }  // HIT

}  // namespace syscalls

namespace other {

// Not dcfs::syscalls.
int read() { return 6; }  // HIT

}  // namespace other

}  // namespace dcfs
