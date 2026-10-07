# Kernel facts verified while planning

Checked against kernel sources (7.3-rc era tree) during 2026-09-27 to
2026-10-05; file references are to the upstream tree. Re-verify before
relying on one after a kernel upgrade.

## FUSE

- `fuse_get_dentry` (fs/fuse/inode.c) decodes an NFS handle by sending
  `LOOKUP(nodeid, ".")` and returns ESTALE if the reply's generation
  differs from the handle's. dcfs relies on this from Phase 14.
- FUSE passthrough backing files require CAP_SYS_ADMIN
  (`fuse_backing_open`, fs/fuse/backing.c) and the backing superblock's
  stack depth must be below the connection's `max_stack_depth`.
- The kernel allows one passthrough backing file per inode; dcfs shares
  one O_RDWR backing file between opens.
- The VFS retries an open once after ESTALE.
- The kernel sends FUSE_INIT at mount time (`fuse_fill_super`).

## File handles

- `open_by_handle_at` rejects O_PATH mount fds: dcfs's mount fds are
  O_RDONLY directory fds (history.md amendment 7).
- `may_decode_fh` (fs/fhandle.c): CAP_DAC_READ_SEARCH allows decoding
  through any mount fd, including a detached mount.
- `ext4_nfs_get_inode` and `btrfs_get_dentry` accept generation 0 as "any
  generation"; xfs needs the exact generation (get it with
  `XFS_IOC_FSBULKSTAT_SINGLE`).
- Handle formats: ext4/xfs `FILEID_INO32_GEN` (1) and `FILEID_INO64_GEN`
  (0x81); btrfs 0x4d/0x4e (`struct btrfs_fid`: objectid, root_objectid,
  32-bit generation). `XFS_MAXINUMBER` = 2^56-1; btrfs object ids start
  at 256 (`BTRFS_FIRST_FREE_OBJECTID`).
- `generic_encode_ino32_fh` (fs/libfs.c) is used by ext2, ext4, f2fs, fat,
  jfs, ntfs3, ntfs, squashfs, ufs, affs, befs, jffs2, the SMB client and
  overlayfs.
- The NFS client's handle is the fileid, file type and the server's opaque
  handle (`nfs_encode_fh`, fs/nfs/export.c): no generation.
- `AT_HANDLE_CONNECTABLE` (Amir Goldstein, 2024) makes handles that decode
  to connected dentries; it cannot be combined with `AT_EMPTY_PATH` or
  `AT_HANDLE_FID`.
- `__fsnotify_parent` uses `dget_parent`, so events on disconnected
  dentries miss parent-directory watches.

## Mounts

- `open_tree(OPEN_TREE_CLONE)` without `AT_RECURSIVE` clones only the
  source mount (`__do_loopback` -> `clone_mnt`), and fails if the source
  has locked children.
- A detached tree is dissolved when the last fd referencing it closes
  (`FMODE_NEED_UNMOUNT`, set by `open_tree` and `fsmount`).
- Mount propagation skips anonymous (detached) namespaces (`is_anon_ns`,
  fs/pnode.c).
- `may_copy_tree` allows cloning a detached mount whose anonymous
  namespace originated in the caller's mount namespace
  (`check_anonymous_mnt`).
- `graft_tree` requires the mount and its mount point to agree on being
  directories: a file can be mounted on a file.
- `fsmount` creates an anonymous namespace for its result.

## Filesystems

- btrfs does not implement `FS_IOC_GETFSUUID`; dcfs uses the fsid from
  `BTRFS_IOC_FS_INFO` (history.md amendment 20). tmpfs does implement it.
- `FS_IOC_GETFSUUID` exists since Linux 6.9.
- Encrypted directories are reported by `STATX_ATTR_ENCRYPTED`;
  case-insensitive directories by `FS_CASEFOLD_FL` (`FS_IOC_GETFLAGS`).

## fileattr_set does not invalidate FUSE's attribute cache (found 2026-10-07)

`fuse_fileattr_set` (fs/fuse/ioctl.c) and `vfs_fileattr_set` (fs/file_attr.c)
do not call `fuse_invalidate_attr` after a successful FS_IOC_SETFLAGS /
FS_IOC_FSSETXATTR, so a `stat` after `chattr` through dcfs serves the ctime
cached by the preceding GETATTR until the attribute timeout. Seen as the
one-in-eight `immutable-ctime` failure (seconds differ only across a second
boundary). Decision (russ): documented limitation (README), check kept as
`DISABLED_immutable-ctime`. A fix would be a one-line kernel patch
(invalidate attrs after a successful fileattr_set) or notify_inval_inode
from dcfs (needs the notifier thread).
