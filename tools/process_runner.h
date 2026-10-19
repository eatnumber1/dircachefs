// SPDX-License-Identifier: MIT
#ifndef DCFS_TOOLS_PROCESS_RUNNER_H_
#define DCFS_TOOLS_PROCESS_RUNNER_H_

#include <span>
#include <string>

#include "absl/status/statusor.h"
#include "tools/test_first.h"

namespace dcfs::test_first {

// Runs the programs of the test-first check as child processes: argv[0] is
// searched for in `path` (a ':'-separated list, as PATH) unless it has a '/',
// and the child's standard output and error come back together. A child that
// dies of a signal has exit code 128 plus the signal, as a shell reports it.
class ProcessRunner : public CommandRunner {
 public:
  explicit ProcessRunner(std::string path) : path_(std::move(path)) {}

  absl::StatusOr<CommandResult> Run(std::span<const std::string> argv) override;

 private:
  std::string path_;
};

}  // namespace dcfs::test_first

#endif  // DCFS_TOOLS_PROCESS_RUNNER_H_
