// SPDX-License-Identifier: MIT
#ifndef DCFS_PROC_STATUS_H_
#define DCFS_PROC_STATUS_H_

#include <sys/types.h>

#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "dcfs/capability_set.h"

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
  CapabilitySet cap_effective;
};

// The uid, groups and capabilities in `status`, the contents of a status
// file.
absl::StatusOr<ProcCredentials> ParseProcStatus(std::string_view status);

// The credentials of thread `tid` (a thread group's leader is its process),
// from /proc/<tid>/task/<tid>/status, which any reader may open (the
// kernel's proc_pid_status entry is mode 0444, with no ptrace check:
// fs/proc/base.c:3650 (Linux v7.3-rc7)).
absl::StatusOr<ProcCredentials> ReadProcCredentials(pid_t tid);

// Whether thread `tid`'s user namespace is this process's: the nsfs inode
// that stat(2) of /proc/<tid>/task/<tid>/ns/user reaches, against
// /proc/self/ns/user's. Following that link needs ptrace read access to the
// task (proc_ns_get_link's ptrace_may_access(PTRACE_MODE_READ_FSCREDS),
// fs/proc/namespaces.c:63 (Linux v7.3-rc7)), which a reader without
// CAP_SYS_PTRACE lacks for a task of another uid, or for one holding a
// capability the reader does not (cap_ptrace_access_check,
// security/commoncap.c:164 (Linux v7.3-rc7)); then this fails.
absl::StatusOr<bool> SharesMyUserNamespace(pid_t tid);

// Whether this process runs in the init user namespace: its
// /proc/self/ns/user is the nsfs inode USER_NS_INIT_INO
// (include/uapi/linux/nsfs.h:50 (Linux v7.3-rc7); PROC_USER_INIT_INO, the
// same number, since Linux 3.8).
absl::StatusOr<bool> InInitUserNamespace();

}  // namespace dcfs

#endif  // DCFS_PROC_STATUS_H_
