"""The xfstests src/ programs built for the guest (step 17.1)."""

# The src/ programs built from one file (src/Makefile's TARGETS and
# LINUX_TARGETS) that need no library beyond libc. Left out: the XFS-only ones
# (alloc, fault, godown, t_open_tmpfiles and unwritten_sync, which use XFS
# ioctls our shim does not declare; xfsctl, bstat, bulkstat_*, stale_handle,
# attr-list-by-handle-cursor-test, t_immutable's libacl and libhandle,
# test-nextquota, loggen, xfsfind), those that need libuuid (fake-dump-rootino,
# uuid_ioctl), libaio (t_mmap_dio), liburing (uring_read_fault,
# btrfs_encoded_*), OpenSSL (fscrypt-crypt-util), gdbm (dbtest) and
# libbtrfsutil (t_snapshot_deleted_subvolume, t_btrfs_received_uuid_ioctl).
# src/vfs/ is built too (XFSTESTS_VFS_HELPERS below), without libcap.
XFSTESTS_HELPERS = [
    "af_unix", "allocstale", "append_reader", "append_writer",
    "attr_replace_test", "checkpoint_journal", "chprojid_fail", "cloner",
    "deduperace", "detached_mounts_propagation", "devzero", "dio-append-buf-fault",
    "dio-buf-fault", "dio-interleaved", "dio-invalidate-cache",
    "dio-write-fsync-same-fd", "dio-writeback-race", "dirhash_collide",
    "dirperf", "dirstress", "e4compact", "ext4_resize", "feature",
    "fiemap-fault", "fiemap-tester", "fill", "fill2", "fs-monitor", "fs_perms",
    "fstest", "fsync-err", "fsync-tester", "ftrunc", "genhashnames",
    "getdevicesize", "getpagesize", "holes", "holetest", "itrash",
    "listxattr", "locktest", "looptest", "lstat64", "makeextents",
    "metaperf", "min_dio_alignment", "mkswap", "mmap-rw-fault",
    "mmap-write-concurrent", "mmapcat", "multi_open_unlink", "nametest",
    "nsexec", "open_by_handle", "permname", "preallo_rw_pattern_reader",
    "preallo_rw_pattern_writer", "punch-alternating", "pwrite_mmap_blocked",
    "randholes", "readdir-while-renames", "rename", "renameat2", "resvtest",
    "rewinddir-test", "runas", "rw_hint", "seek_copy_test", "seek_sanity_test",
    "splice-test", "splice2pipe", "stat_test", "swapon", "t_access_root",
    "t_attr_corruption", "t_create_long_dirs", "t_create_short_dirs",
    "t_dir_offset", "t_dir_offset2", "t_dir_type", "t_encrypted_d_revalidate",
    "t_enospc", "t_ext4_dax_inline_corruption", "t_ext4_dax_journal_corruption",
    "t_futimens", "t_get_file_time", "t_getcwd", "t_holes", "t_mmap_collision",
    "t_mmap_cow_memory_failure", "t_mmap_cow_race", "t_mmap_fallocate",
    "t_mmap_stale_pmd", "t_mmap_write_ro", "t_mmap_writev",
    "t_mmap_writev_overlap", "t_mtab", "t_ofd_locks",
    "t_readdir_1", "t_readdir_2", "t_readdir_3", "t_reflink_read_race",
    "t_rename_overwrite", "t_stripealign", "t_truncate_cmtime", "testx",
    "trunc", "truncate", "truncfile", "unlink-fsync", "unwritten_mmap",
    "usemem", "writemod", "writev_on_pagefault", "file_attr",
]

# And the two programs of src/vfs/ (BUILD.xfstests), installed in src/vfs/.
XFSTESTS_VFS_HELPERS = ["vfs_vfstest", "vfs_mount-idmapped"]
