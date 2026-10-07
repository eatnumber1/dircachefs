#ifndef DCFS_FSUUID_COMPAT_H_
#define DCFS_FSUUID_COMPAT_H_

#include <linux/fs.h>
#include <linux/types.h>

// This host's kernel headers (6.8) predate FS_IOC_GETFSUUID (added in
// 6.9). The definitions below are copied verbatim from the upstream UAPI
// header so callers get correct behavior on newer kernels without a
// rebuild, and correct behavior here without one.
#ifndef FS_IOC_GETFSUUID
struct fsuuid2 {
  __u8 len;
  __u8 uuid[16];
};
#define FS_IOC_GETFSUUID _IOR(0x15, 0, struct fsuuid2)
#endif  // FS_IOC_GETFSUUID

#endif  // DCFS_FSUUID_COMPAT_H_
