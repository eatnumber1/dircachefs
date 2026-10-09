-- dcfs schema v4 (see kSchemaVersion in dcfs/migrate.h for the history;
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
-- (= FUSE_ROOT_ID) and reports generation 0 by convention. Directory
-- entries have explicit states; see the dentries table.
--
-- All tables are STRICT (SQLite 3.37+ -- this build uses 3.53).

-- Cache-wide state: exactly one row (id 1), written by Migrate() when the
-- cache is created and afterwards only through the typed accessors in
-- dcfs/migrate.h.
--
--   schema_version    The schema version of this database (kSchemaVersion
--                     in dcfs/migrate.h). Written at creation and by each
--                     upgrade step Migrate() runs.
--   source_device_id  DeviceId::Serialize() of the source filesystem the
--                     cache was built for. Written once, at creation;
--                     startup refuses a --source on another filesystem.
--   clean_shutdown    1 once a run has shut down cleanly (backing
--                     filesystems synced, dirty set empty, WAL
--                     checkpointed: backing::FinishRun), 0 while a run is
--                     going (set durably by backing::StartRun at every
--                     start). 0 at the next start means the last run
--                     crashed, or the machine lost power, and the dirty
--                     set must be recovered. 1 at creation.
--   boot_id           /proc/sys/kernel/random/boot_id as of the last
--                     start (backing::StartRun), so the next start can
--                     tell a machine crash from a daemon crash in its log.
--                     NULL until the first start.
--   last_stub_id      the highest stub nodeid (stubs.id) ever handed out,
--                     NULL before the first: cache::SetRefused hands out
--                     the next one up and advances it in the same
--                     transaction, so a stub's nodeid is never handed out
--                     again, even after its stub went (a kernel may still
--                     hold it; formal/lifetime.tla's NodeidStable).
CREATE TABLE cache_state (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  schema_version INTEGER NOT NULL,
  source_device_id BLOB NOT NULL,
  clean_shutdown INTEGER NOT NULL,  -- bool
  boot_id TEXT NULL,
  last_stub_id INTEGER NULL
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
  -- 1: every xattr name with no row in `xattrs` is known absent (the name
  -- set is known). 0: such a name is unknown. Only a full listing
  -- (cache::ReplaceXattrs) sets it; no single-name operation clears it.
  xattrs_complete INTEGER NOT NULL DEFAULT 0,  -- bool
  UNIQUE (device_id, backing_ino, backing_gen)
) STRICT;

-- The rows with a recorded link count of 0 (an unnamed O_TMPFILE file, an
-- unlinked file dcfs has open): cache::ForgetUnnamedRows, at every start,
-- reads only these (step 12.4b), not the whole table.
CREATE INDEX inodes_unlinked ON inodes (id) WHERE nlink = 0;

-- Cached directory entries, each in an explicit state:
--   present  the name exists and is `inode`;
--   absent   the name is known not to exist (a negative entry);
--   unknown  nothing is known about the name (e.g. phase 1 of a mutation
--            that is about to change it; its inode row was deleted);
--   refused  the name exists on the backing filesystem but dcfs refuses to
--            cache what is behind it (a mount point or subvolume boundary
--            below --source; see amendment 12 and README.md's
--            Limitations): it is served as a stub directory (its `stubs`
--            row) and must never be reported absent (ENOENT).
-- A name with no row is absent if its directory's children_complete is set,
-- unknown otherwise. Deliberately an ordinary rowid table, not WITHOUT
-- ROWID: the readdir cursor is defined as a dentry's rowid, so the implicit
-- rowid column (and an index to look entries up by inode) is exactly what's
-- needed.
CREATE TABLE dentries (
  parent INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  state TEXT NOT NULL
      CHECK (state IN ('present', 'absent', 'unknown', 'refused')),
  -- No ON DELETE action: the trigger below makes these rows unknown first.
  inode INTEGER NULL REFERENCES inodes (id),
  CHECK ((state = 'present') = (inode IS NOT NULL)),
  PRIMARY KEY (parent, name)
) STRICT;

CREATE INDEX dentries_inode ON dentries (inode);

-- The two directory-wide questions a readdir request asks (cache::ListDir
-- and cache::IsDirComplete), as partial indexes holding only the rows that
-- answer them: "the present names of `parent` after rowid `cursor`, in
-- rowid order" is a range scan of dentries_present (an index's entries are
-- ordered by rowid after their key columns), with no sort; "does `parent`
-- have an unknown name" is a lookup in dentries_unknown, which is almost
-- always empty. Without them the planner used the primary key's index for
-- `parent = ?` and then scanned and sorted every entry of the directory
-- for each page of 64 names, so listing n names cost O(n^2) (Phase 6.2:
-- 10000 names took 3.5 s). The queries must keep the literal state they
-- are indexed by, or SQLite will not use the index.
CREATE INDEX dentries_present ON dentries (parent) WHERE state = 'present';
CREATE INDEX dentries_unknown ON dentries (parent) WHERE state = 'unknown';
-- The stubs a listing shows next to the present names (cache::ListDir
-- merges the two in rowid order); a directory rarely has any.
CREATE INDEX dentries_refused ON dentries (parent) WHERE state = 'refused';

-- Deleting an inode row (an invalidation, or a cascade from its
-- filesystem's row) forgets what it was, not the names that led to it:
-- those become unknown, never absent (audit F8).
CREATE TRIGGER inodes_delete_unknowns BEFORE DELETE ON inodes BEGIN
  UPDATE dentries SET state = 'unknown', inode = NULL WHERE inode = OLD.id;
END;

-- The stub directory a refused dentry is served as (step 23.5; docs/
-- design.md, "Boundaries"): a row here exists while dentry (parent, name)
-- is 'refused', or 'unknown' since it was (forgotten: a mutation's phase 1,
-- cache::ForgetNegativeDentries before a relisting), so that refusing the
-- name again keeps its nodeid and generation. cache::SetRefused writes
-- both in one transaction; the triggers below delete the stub when the
-- dentry is recorded present or absent (relisted as something else, or
-- gone) or deleted, and the foreign key with its parent.
--   id        the stub's FUSE nodeid (and the inode number it reports),
--             from the range at or above 2^63, which no backing inode
--             number may use (backing.cc refuses one): as a signed 64-bit
--             SQLite integer, always negative. Kept for as long as the
--             stub, across restarts, and never handed out again
--             (cache_state.last_stub_id).
--   fuse_gen  its FUSE generation: random, nonzero, as inodes.fuse_gen.
--   the attribute columns: the boundary root's statx when the name was
--             last probed. Nothing dcfs does changes them (every operation
--             on the stub but a read is refused), so they need no unknown
--             state of their own: the dentry's state is the record.
CREATE TABLE stubs (
  id INTEGER PRIMARY KEY CHECK (id < 0),
  parent INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  fuse_gen INTEGER NOT NULL,
  mode INTEGER NOT NULL,
  nlink INTEGER NOT NULL,
  uid INTEGER NOT NULL,
  gid INTEGER NOT NULL,
  rdev INTEGER NOT NULL,
  size INTEGER NOT NULL,
  blocks INTEGER NOT NULL,
  blksize INTEGER NOT NULL,
  atime_s INTEGER NOT NULL,
  atime_ns INTEGER NOT NULL,
  mtime_s INTEGER NOT NULL,
  mtime_ns INTEGER NOT NULL,
  ctime_s INTEGER NOT NULL,
  ctime_ns INTEGER NOT NULL,
  btime_s INTEGER NOT NULL,
  btime_ns INTEGER NOT NULL,
  UNIQUE (parent, name)
) STRICT;

CREATE TRIGGER dentries_unrefused AFTER UPDATE OF state ON dentries
    WHEN OLD.state IN ('refused', 'unknown')
         AND NEW.state IN ('present', 'absent') BEGIN
  DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
END;

CREATE TRIGGER dentries_refused_deleted AFTER DELETE ON dentries
    WHEN OLD.state IN ('refused', 'unknown') BEGIN
  DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
END;

-- epoch: bumped by every write that clears children_complete, so that a
-- mutation's phase 3 can restore the completeness its own phase 1 cleared
-- only if nothing else cleared it meanwhile (cache::RestoreDirComplete).
CREATE TABLE directories (
  inode INTEGER PRIMARY KEY REFERENCES inodes (id) ON DELETE CASCADE,
  children_complete INTEGER NOT NULL DEFAULT 0,  -- bool
  epoch INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE symlinks (
  inode INTEGER PRIMARY KEY REFERENCES inodes (id) ON DELETE CASCADE,
  target BLOB NOT NULL
) STRICT;

-- One row per xattr name whose state is known to differ from what
-- inodes.xattrs_complete says about names with no row: 'present' (with
-- its value), 'absent' (known not to exist, even while the set is
-- incomplete), or 'unknown' (e.g. phase 1 of a mutation that is about to
-- change it, even while the set is complete).
CREATE TABLE xattrs (
  inode INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
  name BLOB NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('present', 'absent', 'unknown')),
  value BLOB NULL,
  CHECK ((state = 'present') = (value IS NOT NULL)),
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
-- have made disagree with the backing filesystems, and keeps the rows: only
-- a sync point removes them. No foreign key: a row
-- may outlive its inode (recovery skips it), and must not vanish with it.
--
-- atime_only (step 23.8): 1 for a row that stands only for a regular
-- file's access time, which the backing filesystem changes on its own
-- reads and writes back lazily: a cold read-only open's row (the reads it
-- allows), and any record of the attributes of a file dcfs holds open
-- (they may carry an access time not written back yet). Such rows are
-- recovered like any other but do not by themselves make a sync point run
-- (Context::dirty.any counts the other rows only): its syncfs would force
-- the write-back the backing mount defers (lazytime: by a day). A mutation's
-- phase 1 makes a row 0 whatever it was.
CREATE TABLE dirty (
  inode INTEGER PRIMARY KEY,
  atime_only INTEGER NOT NULL DEFAULT 0 CHECK (atime_only IN (0, 1))
) STRICT;
