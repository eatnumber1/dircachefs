/*
 * A stand-in for xfsprogs' <xfs/xqm.h>, written for this repository: the
 * kernel's own XFS quota interface (<linux/dqblk_xfs.h>) is all that
 * src/feature.c takes from it (the Q_XGETQSTAT command, fs_quota_stat_t and
 * the XFS_QUOTA_* flags). On a file system without XFS quotas (dcfs's) the
 * quotactl calls fail and feature reports the feature as absent.
 */
#ifndef DCFS_XFSTESTS_SHIM_XQM_H
#define DCFS_XFSTESTS_SHIM_XQM_H

#include <linux/dqblk_xfs.h>
#include <sys/quota.h>

/* Newer kernel headers name the flags FS_QUOTA_*; xfsprogs still says XFS_QUOTA_*. */
#ifndef XFS_QUOTA_UDQ_ACCT
#define XFS_QUOTA_UDQ_ACCT FS_QUOTA_UDQ_ACCT
#define XFS_QUOTA_UDQ_ENFD FS_QUOTA_UDQ_ENFD
#define XFS_QUOTA_GDQ_ACCT FS_QUOTA_GDQ_ACCT
#define XFS_QUOTA_GDQ_ENFD FS_QUOTA_GDQ_ENFD
#define XFS_QUOTA_PDQ_ACCT FS_QUOTA_PDQ_ACCT
#define XFS_QUOTA_PDQ_ENFD FS_QUOTA_PDQ_ENFD
#endif

#endif
