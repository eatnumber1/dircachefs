/*
 * A stand-in for xfsprogs' <xfs/xfs.h>, written for this repository (the
 * real header is xfsprogs-dev's, which the guest build does not have). xfstests'
 * ltp/fsstress.c names the XFS ioctl structures and calls xfsctl()
 * (and fsx.c getopt_long, which the real header drags in) unconditionally; this declares just enough for it to compile. Every
 * XFS-specific ioctl then goes to the file system under test through
 * ioctl(2): dcfs answers none of them (FUSE has no ioctl for it) and fsstress
 * counts the failure, so the coverage lost is exactly fsstress's XFS-only
 * operations (bulkstat, resvsp/unresvsp, fsgetxattr project ids and extent
 * sizes, direct-I/O alignment query, XFS error injection), which never
 * reach a FUSE file system. Layouts follow the kernel's
 * <linux/fs.h>/<xfs/xfs_fs.h> only as far as the compiler needs; the
 * structures are never interpreted by anything here.
 */
#ifndef DCFS_XFSTESTS_SHIM_XFS_H
#define DCFS_XFSTESTS_SHIM_XFS_H

#include <getopt.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <linux/fs.h>

struct xfs_fsop_geom {
	uint32_t blocksize;
	uint32_t rtextsize;
	uint32_t agblocks;
	uint32_t agcount;
	uint32_t logblocks;
	uint32_t sectsize;
	uint32_t inodesize;
	uint32_t imaxpct;
	uint64_t datablocks;
	uint64_t rtblocks;
	uint64_t rtextents;
	uint64_t logstart;
	unsigned char uuid[16];
	uint32_t sunit;
	uint32_t swidth;
	int32_t version;
	uint32_t flags;
	uint32_t logsectsize;
	uint32_t rtsectsize;
	uint32_t dirblocksize;
};

typedef struct xfs_error_injection {
	int32_t fd;
	int32_t errtag;
} xfs_error_injection_t;

struct xfs_bstat {
	uint64_t bs_ino;
	uint16_t bs_mode;
	uint16_t bs_nlink;
	uint32_t bs_uid;
	uint32_t bs_gid;
	uint32_t bs_rdev;
	int32_t bs_blksize;
	int64_t bs_size;
	struct {
		int32_t tv_sec;
		int32_t tv_nsec;
	} bs_atime, bs_mtime, bs_ctime;
	int64_t bs_blocks;
	uint32_t bs_xflags;
	int32_t bs_extsize;
	int32_t bs_extents;
	uint32_t bs_gen;
	uint16_t bs_projid_lo;
	uint16_t bs_forkoff;
	uint16_t bs_projid_hi;
	unsigned char bs_pad[6];
	uint32_t bs_cowextsize;
	uint32_t bs_dmevmask;
	uint16_t bs_dmstate;
	uint16_t bs_aextents;
};

struct xfs_fsop_bulkreq {
	__u64 *lastip;
	int32_t icount;
	void *ubuffer;
	int32_t *ocount;
};

struct xfs_flock64 {
	int16_t l_type;
	int16_t l_whence;
	int64_t l_start;
	int64_t l_len;
	int32_t l_sysid;
	uint32_t l_pid;
	int32_t l_pad[4];
};

typedef struct xfs_flock64 xfs_flock64_t;

struct dioattr {
	uint32_t d_mem;
	uint32_t d_miniosz;
	uint32_t d_maxiosz;
};

#define XFS_IOC_FSGEOMETRY _IOR('X', 124, struct xfs_fsop_geom)
#define XFS_IOC_ERROR_INJECTION _IOW('X', 116, xfs_error_injection_t)
#define XFS_IOC_ERROR_CLEARALL _IOW('X', 117, xfs_error_injection_t)
#define XFS_IOC_DIOINFO _IOR('X', 30, struct dioattr)
#define XFS_IOC_FSBULKSTAT _IOWR('X', 101, struct xfs_fsop_bulkreq)
#define XFS_IOC_FSBULKSTAT_SINGLE _IOWR('X', 102, struct xfs_fsop_bulkreq)
#define XFS_IOC_RESVSP64 _IOW('X', 42, struct xfs_flock64)
#define XFS_IOC_RESVSP _IOW('X', 40, struct xfs_flock64)
#define XFS_IOC_UNRESVSP64 _IOW('X', 43, struct xfs_flock64)
#define XFS_IOC_FSGETXATTR FS_IOC_FSGETXATTR
#define XFS_IOC_FSSETXATTR FS_IOC_FSSETXATTR

#define XFS_XFLAG_REALTIME FS_XFLAG_REALTIME
#define XFS_XFLAG_EXTSIZE FS_XFLAG_EXTSIZE

static inline int xfsctl(const char *path, int fd, int cmd, void *arg)
{
	(void)path;
	return ioctl(fd, (unsigned long)cmd, arg);
}

#endif
