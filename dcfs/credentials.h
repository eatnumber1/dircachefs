#ifndef DCFS_CREDENTIALS_H_
#define DCFS_CREDENTIALS_H_

#include <sys/types.h>

#include <vector>

namespace dcfs {

// The filesystem identity of whoever a backing operation is performed for:
// a FUSE request's caller (FuseRequest::Caller), as the kernel reported it
// (fuse_req_ctx) plus the caller's supplementary groups (read from
// /proc by fuse_req_getgroups). The backing layer switches the thread's
// filesystem credentials to this around the syscalls whose outcome depends
// on who asks (see backing.cc's AsCaller).
struct Credentials {
  uid_t uid;
  gid_t gid;
  std::vector<gid_t> groups;
  // The caller's umask (fuse_ctx::umask), which the kernel sends with
  // CREATE, MKDIR and MKNOD instead of applying it itself (dcfs requests
  // FUSE_CAP_DONT_MASK); 0 for every other request, where no object is
  // created. AsCaller makes it the process umask around the backing
  // syscall, so that the backing filesystem applies it, or does not where
  // the parent directory has a default ACL, exactly as for a local create.
  mode_t umask = 0;
};

}  // namespace dcfs

#endif  // DCFS_CREDENTIALS_H_
