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
};

}  // namespace dcfs

#endif  // DCFS_CREDENTIALS_H_
