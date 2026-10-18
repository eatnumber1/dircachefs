// SPDX-License-Identifier: MIT
#include "dcfs/testonly/process.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_process.h"

namespace dcfs::testonly {

std::string DescribeWaitStatus(int wait_status) {
  if (WIFEXITED(wait_status)) {
    return absl::StrCat("exited with status ", WEXITSTATUS(wait_status));
  }
  return absl::StrCat("was killed by signal ", WTERMSIG(wait_status));
}

absl::Status RunCommand(const std::vector<std::string> &argv) {
  RET_CHECK(!argv.empty()) << "RunCommand of no command";
  ASSIGN_OR_RETURN(pid_t pid, syscalls::fork());
  if (pid == 0) {
    // Only returns on failure, which is the exit status 127.
    syscalls::execv(argv[0], argv).IgnoreError();
    syscalls::_exit(127);
  }
  int wait_status = 0;
  RETURN_IF_ERROR(syscalls::waitpid(pid, &wait_status, 0).status());
  if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
    return absl::OkStatus();
  }
  return InternalErrorBuilder() << "`" << absl::StrJoin(argv, " ") << "` "
                                << DescribeWaitStatus(wait_status);
}

}  // namespace dcfs::testonly
