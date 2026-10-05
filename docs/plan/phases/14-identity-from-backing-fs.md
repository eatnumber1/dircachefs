# Phase 14 — Identity from the backing filesystem

**Problem.** dcfs's FUSE nodeid is its own row id and its generation is a
random number stored in the cache DB, so NFS handles do not survive deleting
the DB, and identity needs durability machinery (random generations,
reuse-after-rollback analysis). Since history.md amendment 12 there is exactly one
backing filesystem, so backing inode numbers are unique and can be the
nodeids, as libfuse_passthrough does. The kernel already rejects stale
handles: after `LOOKUP(nodeid, ".")` it compares the reply's generation with
the handle's (`fuse_get_dentry`, fs/fuse/inode.c) and returns ESTALE on a
mismatch.

**Design.**
- nodeid = backing inode number; the source root is nodeid 1; refuse a tree
  that exposes backing inode 1 (as libfuse_passthrough does).
- FUSE generation = backing generation, decoded from the backing file handle
  (works for every file type, unlike FS_IOC_GETVERSION). Decoders for the
  tested filesystems: ext4 and xfs `FILEID_INO32_GEN` (1) / `FILEID_INO64_GEN`
  (0x81) (ino, gen); btrfs `FILEID_BTRFS_WITHOUT_PARENT` (0x4d) and
  `_WITH_PARENT` (0x4e) (`struct btrfs_fid`: objectid, root_objectid, gen u32).
- `LOOKUP(nodeid, ".")` / any request for a nodeid with no cached row
  (restart, cache wipe, NFS reconnect): build a backing handle from the inode
  number and open it: ext4 and btrfs accept generation 0 as "any"
  (`ext4_nfs_get_inode`, `btrfs_get_dentry`); xfs needs the exact generation:
  get it with `XFS_IOC_FSBULKSTAT_SINGLE` (CAP_SYS_ADMIN; dcfs is root). Reply
  with the generation actually found; the kernel enforces the match.
- The cache DB is keyed by backing (ino, gen); losing it costs only a cold
  cache. Removed: random generations, `inodes.id AUTOINCREMENT` as identity,
  the power-loss nodeid-reuse analysis (history.md amendment 14).
- Recycling: with exclusive access a backing inode number cannot be reused
  while dcfs still holds the object (the removed-object records added in the original
  plan's step 6.3, audit crash F10, keep an fd until the kernel's last
  FORGET), so the kernel never sees one nodeid for two live objects.
  Out-of-band recycling stays unsupported (this is where the kernel
  FUSE_ATTR_GENERATION patch would add protection).

**Decisions (russ, 2026-10-04).**
1. Backing filesystems without a known handle format are not supported:
   refuse to start with a clear error naming the filesystem type.
2. Boundaries (submounts and btrfs subvolumes): the entry is a visible stub
   directory, anything inside returns ENOTSUP, rename/link across returns
   EXDEV; access comes from mounting another dcfs instance over it
   (Phase 15 (The `mount.dcfs` wrapper)). Backing inode numbers at or above 2^63 are refused like inode
   1: that range is reserved for stub nodeids (ext4 is 32-bit, xfs < 2^56,
   btrfs objectids grow up from 256).
3. dcfs was never in production: change the schema freely, no migration.
   Delete the migration code; a cache with another schema version is
   rebuilt with a log line.

**14.1 Tests first (must fail on today's main; quote the output).**
- Invert `handles.sh` `db-wipe-*`: take handles of a file, a directory, a
  symlink and a file in a subdirectory; stop dcfs; delete the cache DB;
  restart; every `fhtest open` must SUCCEED (today: ERR ESTALE) on
  ext4/xfs/btrfs (handles_test matrix).
- `nfs.sh`: hold a file open on the NFS client, wipe the DB, restart dcfs
  (with `exportfs -f`), keep reading without ESTALE (today: ESTALE).
- Recycled inode after a wipe: take a handle, stop dcfs, wipe the DB, recycle
  the inode number on the backing fs (rm + create until reused, with dcfs
  stopped), restart: the old handle gets ERR ESTALE (kernel generation
  check); the new file's handle works. Per filesystem.
- `stat -c %i` through the mount equals the nodeid seen in `fhtest handle`
  (nodeid = backing ino) and the backing `st_ino`.
- Unit: handle decoders for each format, using real handles taken in the
  guest on ext4, xfs (inode32 and inode64 mounts) and btrfs; generation for
  symlinks and devices non-zero where the filesystem provides one.
- Unit: `LOOKUP(".")` resolution without a cache row returns the backing
  generation; xfs path uses bulkstat; ext4/btrfs path uses generation 0.

**14.2 Handle decoders** (`dcfs/handle_identity.{h,cc}`): parse ino and
generation from backing handles; build an "any generation" handle from an
inode number; xfs bulkstat helper. Unit tests.

**14.3 Schema and cache identity:** `inodes.id` = backing ino (no
AUTOINCREMENT), `fuse_gen` = backing generation, identity check by (ino, gen,
handle bytes, btime) unchanged; delete Migrate's upgrade paths (decision 3);
update `UpsertInode`, population, recovery, removed-object records.

**14.4 Request path:** entry/attr replies use the new nodeid/generation; any
request for an uncached nodeid resolves through 14.2 (open by inode number,
statx, upsert row) instead of ESTALE; `ParentOf` and `..` resolution use it
too.

**14.5 Cleanup and docs:** remove random generations and their tests, update
design.md identity model, README Limitations (remove "handles do not survive
a cache wipe"), docs/conformance.md if pjdfstest changes.

**14.6 Review:** a read-only audit of the new identity code against the race,
crash and tri-state rules before merge.

Owner: Opus (identity invariants), one step at a time; test-first per item.
Order: Phase 13 (Connected backing fds) first (small, independent), then Phase 14 (Identity from the backing filesystem) (both touch
`OpenNode`).
