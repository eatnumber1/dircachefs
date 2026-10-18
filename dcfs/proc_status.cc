// SPDX-License-Identifier: MIT
#include "dcfs/proc_status.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "dcfs/proc_file.h"
#include "dcfs/ret_check.h"
#include "dcfs/syscalls.h"

namespace dcfs {
namespace {

// The decimal numbers of a status line's value ("\t0 10 " -> {0, 10}).
absl::StatusOr<std::vector<uint32_t>> Numbers(std::string_view value) {
  std::vector<uint32_t> numbers;
  for (std::string_view number :
       absl::StrSplit(value, absl::ByAnyChar(" \t"), absl::SkipEmpty())) {
    RET_CHECK(absl::SimpleAtoi(number, &numbers.emplace_back()))
        << "a number in a status file line: " << number;
  }
  return numbers;
}

}  // namespace

// The kernel writes these lines for every task (proc_pid_status:
// task_state's "Uid:" (real, effective, saved, filesystem) and "Groups:",
// task_cap's "CapEff:"), in decimal but the capabilities in hex; anything
// else is not a status file.
absl::StatusOr<ProcCredentials> ParseProcStatus(std::string_view status) {
  std::optional<std::vector<uint32_t>> uids;
  std::optional<std::vector<uint32_t>> groups;
  std::optional<uint64_t> cap_effective;
  for (std::string_view line : absl::StrSplit(status, '\n')) {
    if (absl::ConsumePrefix(&line, "Uid:")) {
      ASSIGN_OR_RETURN(uids, Numbers(line));
    } else if (absl::ConsumePrefix(&line, "Groups:")) {
      ASSIGN_OR_RETURN(groups, Numbers(line));
    } else if (absl::ConsumePrefix(&line, "CapEff:")) {
      RET_CHECK(absl::SimpleHexAtoi(absl::StripAsciiWhitespace(line),
                                    &cap_effective.emplace()))
          << "a status file's CapEff line: " << line;
    }
  }
  RET_CHECK(uids.has_value() && uids->size() == 4)
      << "a status file without four uids";
  RET_CHECK(groups.has_value()) << "a status file with no Groups line";
  RET_CHECK(cap_effective.has_value()) << "a status file with no CapEff line";
  return ProcCredentials{
      .fsuid = (*uids)[3],
      .groups = std::vector<gid_t>(groups->begin(), groups->end()),
      .cap_effective = *cap_effective};
}

absl::StatusOr<ProcCredentials> ReadProcCredentials(pid_t tid) {
  ASSIGN_OR_RETURN(
      std::string status,
      ReadProcFile(absl::StrCat("/proc/", tid, "/task/", tid, "/status")));
  return ParseProcStatus(status);
}

absl::StatusOr<bool> SharesMyUserNamespace(pid_t tid) {
  // procfs, off the backing filesystem: stat'ed by path (syscalls.h). A
  // user namespace is its nsfs inode, which stat(2) of the magic link
  // reaches.
  ASSIGN_OR_RETURN(
      struct stat theirs,
      syscalls::stat(absl::StrCat("/proc/", tid, "/task/", tid, "/ns/user")));
  ASSIGN_OR_RETURN(struct stat mine, syscalls::stat("/proc/self/ns/user"));
  return theirs.st_dev == mine.st_dev && theirs.st_ino == mine.st_ino;
}

}  // namespace dcfs
