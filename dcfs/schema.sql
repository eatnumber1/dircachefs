-- dcfs schema v2 (see kSchemaVersion in dcfs/migrate.h for the history;
-- Migrate() upgrades older databases in place).
--
-- Executed as a script against a fresh database by Migrate() (see
-- dcfs/migrate.h). PRAGMA foreign_keys is already ON, set by the connection
-- factory (dcfs/sqlite.h) before this runs.
--
-- Identity model: the 64-bit inodes.id is the FUSE nodeid and is never
-- reused. The backing identity is (device_id, backing_ino, backing_gen),
-- unique per row -- hard links to the same backing file share a row. The
-- FUSE generation reported to the kernel is inodes.fuse_gen, a uniformly
-- random nonzero 32-bit value drawn for each new row, so neither a rebuilt
-- cache nor a power loss that rolls back recent inserts (and so lets
-- AUTOINCREMENT hand an id out again) reissues an old (id, gen) pair,
-- except with probability 2^-32 per reissued id. The root row has id 1
-- (= FUSE_ROOT_ID) and reports generation 0 by convention. dentries.inode IS NULL means a
-- cached negative entry.
--
-- All tables are STRICT (SQLite 3.37+ -- this build uses 3.53). meta.value is
-- typed ANY rather than BLOB because it stores a mix of text-encoded
-- integers (schema_version, clean_shutdown), text (boot_id) and raw bytes
-- (source_device_id) -- a STRICT BLOB column rejects TEXT values outright,
-- with no coercion, so ANY (which preserves whatever storage class was
-- bound, unmodified) is the type that actually fits a heterogeneous
-- key/value table. Every other BLOB column below holds only raw bytes and
-- keeps the stricter BLOB type.

CREATE TABLE meta (
  key TEXT PRIMARY KEY,
  value ANY
) STRICT;

-- Every backing filesystem under the source, plus the dentry through which
-- it was entered. The source filesystem itself has NULL parent_inode and
-- boundary_name. This forms a reference cycle with inodes (inodes.device_id
-- references filesystems.device_id, and filesystems.parent_inode
-- references inodes.id) -- SQLite permits this because foreign keys are
-- only enforced at statement end, not at CREATE TABLE time, and the source
-- filesystem's row (the one created before any inode exists) has a NULL
-- parent_inode.
CREATE TABLE filesystems (
  device_id BLOB PRIMARY KEY,
  fstype INTEGER NOT NULL,
  parent_inode INTEGER NULL REFERENCES inodes (id) ON DELETE CASCADE,
  boundary_name BLOB NULL
) STRICT;

-- One row per unique backing identity (device_id, backing_ino, backing_gen).
-- `id` is the FUSE nodeid, minted once and never reused. `fuse_gen` is the
-- FUSE generation reported to the kernel for this nodeid -- the root row
-- (id = FUSE_ROOT_ID = 1) always reports generation 0.
CREATE TABLE inodes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id BLOB NOT NULL REFERENCES filesystems (device_id) ON DELETE CASCADE,
  backing_ino INTEGER NOT NULL,
  backing_gen INTEGER NOT NULL,
  fuse_gen INTEGER NOT NULL,
  handle_type INTEGER NULL,
  handle BLOB NULL,
  attrs_valid INTEGER NOT NULL DEFAULT 0,  -- bool
  mode INTEGER,
  nlink INTEGER,
  uid INTEGER,
  gid INTEGER,
  rdev INTEGER,
  size INTEGER,
  blocks INTEGER,
  blksize INTEGER,
  atime_s INTEGER,
  atime_ns INTEGER,
  mtime_s INTEGER,
  mtime_ns INTEGER,
  ctime_s INTEGER,
  ctime_ns INTEGER,
  btime_s INTEGER,
  btime_ns INTEGER,
  xattrs_complete INTEGER NOT NULL DEFAULT 0,  -- bool
  UNIQUE (device_id, backing_ino, backing_gen)
) STRICT;

-- Cached directory entries. inode IS NULL means a cached negative entry.
-- Deliberately an ordinary rowid table, not WITHOUT ROWID: the readdir
-- cursor is defined as a dentry's rowid, so the implicit rowid column (and
-- an index to look entries up by inode) is exactly what's needed.
CREATE TABLE dentries (
  parent INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  inode INTEGER NULL REFERENCES inodes (id) ON DELETE SET NULL,
  PRIMARY KEY (parent, name)
) STRICT;

CREATE INDEX dentries_inode ON dentries (inode);

CREATE TABLE directories (
  inode INTEGER PRIMARY KEY REFERENCES inodes (id) ON DELETE CASCADE,
  children_complete INTEGER NOT NULL DEFAULT 0  -- bool
) STRICT;

CREATE TABLE symlinks (
  inode INTEGER PRIMARY KEY REFERENCES inodes (id) ON DELETE CASCADE,
  target BLOB NOT NULL
) STRICT;

CREATE TABLE xattrs (
  inode INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  value BLOB NOT NULL,
  PRIMARY KEY (inode, name)
) STRICT;

-- The durable dirty set: every inode whose cached attributes, dentries (as
-- a parent), symlink target or xattrs a mutation has changed since the
-- backing filesystems were last synced (backing::SyncBacking: syncfs, then
-- DELETE FROM dirty). Phase 1 of every mutation inserts the inodes it is
-- about to change, in a transaction committed with synchronous=FULL, before
-- the backing syscall; phase 3 never removes rows. After an unclean
-- shutdown, startup recovery (cache::RecoverDirty) treats everything cached
-- about these inodes as unknown, which is exactly what a power loss may
-- have made disagree with the backing filesystems. No foreign key: a row
-- may outlive its inode (recovery skips it), and must not vanish with it.
CREATE TABLE dirty (
  inode INTEGER PRIMARY KEY
) STRICT;
