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
#include "dcfs/capability_set.h"
#include "dcfs/proc_file.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"

namespace dcfs {
namespace {

// The decimal numbers of a status line's value ("\t0 10 " -> {0, 10}).
absl::StatusOr<std::vector<uint32_t>> Numbers(std::string_view value) {
  std::vector<uint32_t> numbers;
  for (std::string_view number :
       absl::StrSplit(value, absl::ByAnyChar(" \t"), absl::SkipEmpty())) {
    uint32_t parsed = 0;
    if (!absl::SimpleAtoi(number, &parsed)) {
      return InvalidArgumentErrorBuilder()
             << "Not a decimal number in a status file line: " << number;
    }
    numbers.push_back(parsed);
  }
  return numbers;
}

}  // namespace

// The kernel writes these lines for every task (proc_pid_status:
// task_state's "Uid:" (real, effective, saved, filesystem) and "Groups:",
// task_cap's "CapEff:", fs/proc/array.c:188, :198 and :327 (Linux
// v7.3-rc7)), in decimal but the capabilities in hex; anything else is not
// a status file.
absl::StatusOr<ProcCredentials> ParseProcStatus(std::string_view status) {
  std::optional<std::vector<uint32_t>> uids;
  std::optional<std::vector<uint32_t>> groups;
  std::optional<CapabilitySet> cap_effective;
  for (std::string_view line : absl::StrSplit(status, '\n')) {
    if (absl::ConsumePrefix(&line, "Uid:")) {
      ASSIGN_OR_RETURN(uids, Numbers(line));
    } else if (absl::ConsumePrefix(&line, "Groups:")) {
      ASSIGN_OR_RETURN(groups, Numbers(line));
    } else if (absl::ConsumePrefix(&line, "CapEff:")) {
      uint64_t bits = 0;
      if (!absl::SimpleHexAtoi(absl::StripAsciiWhitespace(line), &bits)) {
        return InvalidArgumentErrorBuilder()
               << "Not a hex capability set in a status file's CapEff line: "
               << line;
      }
      cap_effective = CapabilitySet::FromKernel(bits);
    }
  }
  if (!uids.has_value() || uids->size() != 4) {
    return InvalidArgumentErrorBuilder()
           << "A status file without a Uid line of four uids";
  }
  if (!groups.has_value()) {
    return InvalidArgumentErrorBuilder() << "A status file with no Groups line";
  }
  if (!cap_effective.has_value()) {
    return InvalidArgumentErrorBuilder() << "A status file with no CapEff line";
  }
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

absl::StatusOr<bool> InInitUserNamespace() {
  // USER_NS_INIT_INO; the sysroot's <linux/nsfs.h> (6.12) predates it. Its
  // NS_GET_PARENT cannot stand in: it fails with EPERM in every user
  // namespace, not only the init one (kernel_facts_test's
  // NsGetParentFailsWithEpermInEveryUserNamespace).
  constexpr ino_t kInitUserNamespaceInode = 0xEFFFFFFDU;
  ASSIGN_OR_RETURN(struct stat mine, syscalls::stat("/proc/self/ns/user"));
  return mine.st_ino == kInitUserNamespaceInode;
}

}  // namespace dcfs
