// SPDX-License-Identifier: MIT
#ifndef DCFS_TESTONLY_PROCESS_H_
#define DCFS_TESTONLY_PROCESS_H_

#include <string>
#include <vector>

#include "absl/status/status.h"

namespace dcfs::testonly {

// How a child ended, from the status waitpid(2) stored: "exited with status N"
// or "was killed by signal N", for a message.
std::string DescribeWaitStatus(int wait_status);

// Runs a command and waits for it, as system(3) does without a shell:
// `argv[0]` is the program's path (no PATH search), the rest its arguments;
// `argv` is not empty. OK if it exits 0; otherwise the Status names the
// command and how it ended (DescribeWaitStatus; "exited with status 127" if
// the program could not be executed). Its output goes to this process's.
absl::Status RunCommand(const std::vector<std::string> &argv);

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_PROCESS_H_
