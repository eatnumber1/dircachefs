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
// and "CapEff:" lines of /proc/<pid>/task/<tid>/status). These are the
// task's objective (real) credentials, not the ones a kernel caller may be
// acting with for a while (override_creds: nfsd, ksmbd, overlayfs; plan
// step 17.2d), and its capabilities count in its own user namespace only.
struct ProcCredentials {
  uid_t fsuid = 0;
  std::vector<gid_t> groups;
  // A bit per capability (1 << CAP_FSETID, ...).
  uint64_t cap_effective = 0;
};

// The uid, groups and capabilities in `status`, the contents of a status
// file.
absl::StatusOr<ProcCredentials> ParseProcStatus(std::string_view status);

// The credentials of thread `tid` (a thread group's leader is its process),
// from /proc/<tid>/task/<tid>/status, which any reader may open (the
// kernel's proc_pid_status entry is mode 0444, with no ptrace check).
absl::StatusOr<ProcCredentials> ReadProcCredentials(pid_t tid);

// Whether thread `tid`'s user namespace is this process's: the nsfs inode
// that stat(2) of /proc/<tid>/task/<tid>/ns/user reaches, against
// /proc/self/ns/user's. Following that link needs ptrace read access to the
// task (fs/proc/namespaces.c, proc_ns_get_link:
// ptrace_may_access(PTRACE_MODE_READ_FSCREDS)), which a reader without
// CAP_SYS_PTRACE lacks for a task of another uid, or for one holding a
// capability the reader does not (cap_ptrace_access_check); then this
// fails.
absl::StatusOr<bool> SharesMyUserNamespace(pid_t tid);

}  // namespace dcfs

#endif  // DCFS_PROC_STATUS_H_
