// SPDX-License-Identifier: MIT
#ifndef DCFS_SUBPROCESS_H_
#define DCFS_SUBPROCESS_H_

#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace dcfs {

// Everything `fd` yields until its end; a failed read is an error, not the end.
absl::StatusOr<std::string> ReadAll(int fd);

// What of a child's output RunProgram collects; whatever it does not is the
// caller's own descriptor.
enum class Capture {
  kNothing,
  kStdout,  // the child's standard output
  kOutput,  // its standard output and error, interleaved
};

struct ProgramRun {
  int wait_status = 0;  // as wait(2) gives it; decode with WIF* and W*
  std::string output;   // what `capture` asked for
};

// Runs the program at `path` with `argv` (argv[0] is the name it sees for
// itself; `path` is not searched for) and waits for it. A program that cannot
// be executed exits 127 (126 when its output could not be redirected). The
// error is that of starting the child, reading its output or waiting for it; a
// child that was started is always waited for.
absl::StatusOr<ProgramRun> RunProgram(std::string_view path,
                                      std::span<const std::string> argv,
                                      Capture capture);

}  // namespace dcfs

#endif  // DCFS_SUBPROCESS_H_
