// SPDX-License-Identifier: MIT
#ifndef DCFS_PROC_STATUS_H_
#define DCFS_PROC_STATUS_H_

#include <sys/types.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace dcfs {

// What /proc says of a task's credentials: its filesystem uid,
// supplementary groups and effective capabilities (the "Uid:", "Groups:"
// and "CapEff:" lines of /proc/<pid>/task/<tid>/status), and whether its
// user namespace is the reading process's. These are the task's objective
// (real) credentials, not the ones a kernel caller may be acting with for
// a while (override_creds: nfsd, ksmbd, overlayfs; plan step 17.2d), and
// its capabilities count in its own user namespace only.
struct ProcCredentials {
  uid_t fsuid = 0;
  std::vector<gid_t> groups;
  // A bit per capability (1 << CAP_FSETID, ...).
  uint64_t cap_effective = 0;
  bool same_user_namespace = false;
};

// The uid, groups and capabilities in `status`, the contents of a status
// file (same_user_namespace false: a status file does not say).
absl::StatusOr<ProcCredentials> ParseProcStatus(std::string_view status);

// The credentials of thread `tid` (a thread group's leader is its process):
// /proc/<tid>/task/<tid>/status, and its ns/user against /proc/self/ns/user.
absl::StatusOr<ProcCredentials> ReadProcCredentials(pid_t tid);

}  // namespace dcfs

#endif  // DCFS_PROC_STATUS_H_
