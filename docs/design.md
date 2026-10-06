# dcfs design

This document describes how dcfs works and why, for reviewers and for
whoever implements the next stage. The README covers building and using
dcfs; this covers the reasoning. Where something is planned but not built,
it says so.

File and function names refer to the source tree: `dcfs/schema.sql`,
`dcfs/metadata_cache.{h,cc}` (namespace `cache::`), `dcfs/backing.{h,cc}`
(namespace `backing::`), `dcfs/dir_cache_fs.{h,cc}` (class `DirCacheFS`),
`dcfs/main.cc`.

Contents:

1. [Goals and non-goals](#goals-and-non-goals)
2. [Assumptions](#assumptions)
3. [Architecture and layering](#architecture-and-layering)
4. [Identity model](#identity-model)
5. [File descriptors and handles, never paths](#file-descriptors-and-handles-never-paths)
6. [Filesystem identity and why submounts are refused](#filesystem-identity-and-why-submounts-are-refused)
7. [The schema](#the-schema)
8. [Population policy](#population-policy)
9. [The write-through protocol](#the-write-through-protocol)
10. [Crashes, power loss and recovery](#crashes-power-loss-and-recovery)
11. [Concurrency, today and with coroutines](#concurrency-today-and-with-coroutines)
12. [Writable opens and file contents](#writable-opens-and-file-contents)
13. [Out-of-band change detection](#out-of-band-change-detection)
14. [Caller credentials](#caller-credentials)
15. [Extended attributes](#extended-attributes)
16. [Startup and shutdown](#startup-and-shutdown)
17. [Test strategy](#test-strategy)
18. [Known gaps](#known-gaps)
19. [Future work](#future-work)
20. [The protocol model](#the-protocol-model)

## Goals and non-goals

Goals:

- **Keep spun-down disks asleep for metadata work.** Every read operation
  except reading file contents (lookup, getattr, readdir, readdirplus,
  readlink, getxattr, listxattr, access) is answered from a persistent
  SQLite cache that lives on an SSD. A backing disk spins up for file
  contents, for writes, and the first time a directory is listed.
- **File contents at native speed.** Reads, writes and mmap of file
  contents go from the kernel to the backing file through FUSE passthrough;
  the daemon never copies data.
- **Write-through, with the backing filesystem as the only authority.**
  Every mutation reaches the backing filesystem before the cache records
  it. Deleting the cache database, at any time, never loses or corrupts
  data: it costs a cold cache and stale NFS handles.
- **Safe NFS export.** Handles survive daemon restarts. A handle that
  cannot be honoured fails with `ESTALE`, never by resolving to a
  different object.
- **Never serve state the backing filesystem does not have,** including
  after a daemon crash, a kernel crash or a power loss. What a crash may
  cost is re-reading recently changed entries.
- **Behave like the backing filesystem.** POSIX semantics, ownership,
  errnos and inode numbers as the backing filesystem would give them,
  measured with pjdfstest against raw ext4.

Non-goals:

- **Caching file contents.** The page cache and the backing filesystem do
  that.
- **Coherence with changes made behind dcfs's back.** dcfs assumes it is
  the only way the backing tree changes (see Assumptions). It notices
  out-of-band changes only where that costs nothing extra, and never polls
  or watches.
- **Several backing filesystems under one mount.** Refused for now, for
  the reason given in
  [the section on submounts](#filesystem-identity-and-why-submounts-are-refused).
- **Handles that survive a cache wipe.** Possible only with kernel work
  that dcfs deliberately does not pursue (see
  [Identity model](#identity-model)).
- **Unprivileged operation.** dcfs runs as root, full stop.

## Assumptions

- **Exclusive access.** All access to the backing tree goes through dcfs.
  Mounting dcfs over its own source directory makes accidental bypass
  impossible for anything that uses the path. Nothing in dcfs is designed
  to be correct otherwise; out-of-band changes are detected where it is
  free and otherwise go unnoticed.
- **One daemon per cache database.** `main.cc` takes an exclusive
  `flock(2)` on the database file for the life of the process. SQLite
  would serialize two daemons' transactions but not their three-phase
  protocols or their in-memory state.
- **Root.** dcfs needs `CAP_DAC_READ_SEARCH` for `open_by_handle_at`,
  `CAP_SYS_ADMIN` to mount and to register passthrough backing files, and
  `CAP_SETUID`/`CAP_SETGID` to take on callers' filesystem credentials.
  There are no fallback paths for running without them (for example, no
  name-based walk when `open_by_handle_at` fails with `EPERM`).
- **Kernel features.**
  - `FS_IOC_GETFSUUID` (Linux 6.9) on the backing filesystem.
  - FUSE passthrough (Linux 6.9).
  - File handle support on the backing filesystem (`name_to_handle_at`,
    `open_by_handle_at`).
- **The backing filesystem keeps its promises.** `syncfs` makes earlier
  changes durable; file handles encode enough to reject a stale handle
  (ext4, xfs and btrfs encode the inode generation); inode numbers are
  unique within one filesystem.

## Architecture and layering

```
              kernel FUSE (/dev/fuse)
                       |
                 libfuse low-level session loop (one thread)
                       |
               fuse_ops.cc: one static function per op;
               builds a FuseRequest, runs a sync point if due,
               calls DirCacheFS, replies an error on failure
                       |
               DirCacheFS (dir_cache_fs.cc)
               the FUSE operations: phases of each mutation,
               open files, passthrough registrations, replies
                 |                              |
                 v                              v
      backing:: (backing.cc)            cache:: (metadata_cache.cc)
      the only module that does         pure SQLite: typed reads and
      request work on the backing       writes over schema.sql, no
      filesystem; population policy;    syscalls; fill guards and the
      does I/O first, then one short    dirty set
      cache transaction
                 |                              |
      syscalls::, FileHandle,            sqlite3:: wrapper
      DeviceId, FileDescriptor           (connection factory, statement
      (thin wrappers over the kernel)    cache, transactions)
```

Rules that keep the layering honest:

- **Only `backing.cc` touches the backing filesystem for request work.**
  `DirCacheFS` and `cache::` never call `syscalls::`. The lower-level
  modules `backing.cc` is built from (`file_handle.cc`, `device_id.cc`,
  `fd.cc`) also call `syscalls::`, and `main.cc` opens `--source` once at
  startup; those are peers, not violations. The point is that the
  io_uring rewrite replaces one module.
- **`cache::` is pure SQLite.** It never sees a file descriptor. Each write
  function is one transaction, which nests as a savepoint inside a
  caller's transaction.
- **Every cache and backing call takes an explicit `Context&`**
  (`dcfs/context.h`): the database connection, the mount fds, the random
  source for generations, the set of inodes open for writing, the
  in-memory dirty-set bookkeeping and the fill guards. No globals, no
  singletons, no `thread_local`.
- **No transaction spans a backing syscall.** Backing I/O happens first,
  then one short synchronous transaction records the result. No SQLite
  statement cursor is held across a syscall. This is what makes the
  coroutine future mechanical (a transaction will never span a suspension
  point).
- **The SQLite wrapper opens connections through one factory that takes a
  VFS name,** so an io_uring `sqlite3_vfs` can be dropped in later.

## Identity model

### Node ids

Each backing object dcfs has seen has one row in `inodes`. The row id
(`INTEGER PRIMARY KEY AUTOINCREMENT`, 64-bit) is dcfs's identity for the
object and is the FUSE node id. `AUTOINCREMENT` means an id is never handed
out twice by a database that keeps its committed state. The root is row 1
(`FUSE_ROOT_ID`), created with the database.

Backing identifiers never serve as keys. They are recorded on the row:

| Column | Meaning |
|---|---|
| `device_id` | The filesystem (see [Filesystem identity](#filesystem-identity-and-why-submounts-are-refused)). |
| `backing_ino` | The backing inode number. |
| `backing_gen` | The backing inode generation (`FS_IOC_GETVERSION`), 0 if unknown. |
| `handle_type`, `handle` | The backing file handle from `name_to_handle_at`. |
| `btime_s`, `btime_ns` | The birth time, when the filesystem reports one. |

Rows are unique on `(device_id, backing_ino, backing_gen)`, so hard links
to one backing file share one row.

### Generations

The FUSE generation dcfs reports for a node is `inodes.fuse_gen`, a
uniformly random, nonzero 32-bit value drawn when the row is created
(`cache::UpsertInode`, from `Context::rng`). The root reports 0. The pair
`(node id, generation)` is what the kernel puts in an NFS file handle.

Why random rather than a counter: under `synchronous=NORMAL`, a power loss
can roll back the last few committed transactions, including a row insert
whose `(id, gen)` was already given to the kernel and on to an NFS client.
`AUTOINCREMENT` would then hand the same id out again. With a counter the
next row would also get the same generation (since `id - gen` would be
constant), and the client's old handle would resolve to an unrelated new
object. With a random generation the reissued id gets a fresh generation,
and the old handle gets `ESTALE`, except with probability 2^-32 per
reissued id. The same argument covers a deleted and rebuilt database, whose
ids restart from 2.

### What users see

`st_ino` is the backing inode number, so hard-link-aware tools (`tar`,
`rsync`, `cp -a`, `find`) behave as they would on the backing filesystem.
The kernel does not require `st_ino` to equal the node id. Directory
listings report the same numbers as `d_ino`, `.` and `..` included (`..`
of the root is the root itself, as for a mount's root). `st_dev` is the
FUSE mount's.

### Matching a row to a backing object

A row describes the same backing object only if all of these hold
(`cache::UpsertInode` when recording, `VerifyBackingIdentity` in
`backing.cc` when reopening):

- the device and inode number match;
- the generations match, when both are known (nonzero);
- the stored handle bytes match;
- the birth times match, when both are known.

The generation is only read for regular files and directories (an ioctl
needs a real file descriptor, and reopening a FIFO or device for one could
block or have side effects), and is 0 on filesystems without
`FS_IOC_GETVERSION`. The handle bytes still encode the real generation on
ext4, xfs and btrfs, so comparing them covers symlinks and special files.
The birth time covers btrfs, which can reissue a byte-identical handle
(same inode number and generation) after its own power loss. All three are
already in hand when a row is written or verified, so this costs no
syscall.

When the backing filesystem recycles an inode number, the old row no
longer matches. dcfs invalidates it (deletes it; see the trigger in
[the schema](#the-schema)) and creates a new row with a new id and
generation. A handle for the old row then gets `ESTALE` from dcfs itself:
the old node id never resolves to the new row, so `OpenNode`'s
`VerifyBackingIdentity` rejects the mismatch whenever the kernel reaches
dcfs for it.

### Why handles survive restarts but not a cache wipe

Rows persist in the database, so after a restart the same node id still
names the same backing object with the same generation. NFS reconnection
works through two lookups the kernel sends when the node id is not in its
inode cache:

- `LOOKUP(nodeid, ".")`: dcfs answers from the row (`DirCacheFS::Lookup`,
  `EntryFor`), with the row's generation. The kernel compares it with the
  handle's generation.
- `LOOKUP(dir, "..")`, to reconnect a directory to the dentry tree: dcfs
  answers from the directory's cached dentry, or, if that is unknown,
  from the backing filesystem: it opens the directory by handle, opens
  `..` relative to it, identifies the result (handle, inode number,
  generation) and finds or creates its row (`backing::ParentOf`).

After a cache wipe the node id in a client's handle means nothing. The
kernel sends only the node id when reconnecting, so the daemon has no
backing identity to go on. Encoding `(device, ino, gen)` in the node id
would not fit in general, and some filesystems (xfs) need the exact
original generation to decode a handle. Lifting this would mean the daemon
owning handle encoding in the kernel, which is not needed: a wipe yields
`ESTALE` (the new row for a reissued id has a fresh random generation),
which is safe.

## File descriptors and handles, never paths

After startup the daemon holds no path strings. This is what makes
mounting dcfs over the directory it caches a supported configuration: once
the mount covers the path, the path leads back into dcfs.

1. At startup, before mounting, `--source` and `--cache_db` are opened by
   path once. The submount check reads `--source`'s path and
   `/proc/self/mountinfo` in the same window; it is a policy decision that
   identity never depends on. `/proc/sys/kernel/random/boot_id` is read
   once too.
2. Backing filesystems are reached through one **mount fd** per device id
   (`MountFds`). It must be a real directory descriptor (`O_RDONLY |
   O_DIRECTORY`), not `O_PATH`: `open_by_handle_at` resolves its mount fd
   argument through the kernel's non-raw fd lookup (`fs/fhandle.c`,
   `get_path_from_fd()`), which rejects `O_PATH` descriptors with `EBADF`.
   The source's mount fd is the descriptor `main.cc` opened on
   `--source`.
3. Objects are reopened from their stored handles with
   `open_by_handle_at(mount_fd, handle, flags)` (`FileHandle::Open`,
   wrapped by `backing::OpenNode`, which also verifies identity). The root
   is reached through the mount fd itself (`openat(mount_fd, ".")`).
4. Children are reached with `openat(dir_fd, name, ...)`, never a joined
   path. Everything else uses `*at` calls on descriptors: `statx` with
   `AT_EMPTY_PATH`, `readlinkat`, `linkat` with `AT_EMPTY_PATH`,
   `renameat2`, `unlinkat`, `mkdirat`, `mknodat`, `symlinkat`.
5. Where a syscall rejects `O_PATH` descriptors (`fgetxattr`, `fchmod`,
   `FS_IOC_GETVERSION`), dcfs reopens the descriptor through its
   `/proc/self/fd/N` magic link, for regular files and directories only.
   For symlinks and special files it uses the path-based `*xattr` and
   `fchmodat` calls on the same magic link. These are derived from a
   descriptor, not from a stored path.
6. Handles are stored serialized with their device id and are the only
   durable reference to a backing object.

## Filesystem identity and why submounts are refused

### Device ids

A filesystem is identified by a `DeviceId` (`dcfs/device_id.h`): a 16-byte
UUID, plus, on btrfs, the subvolume id (all subvolumes of one btrfs
filesystem share a UUID but have separate inode number spaces). On ext4
and xfs the UUID comes from `ioctl(fd, FS_IOC_GETFSUUID)` directly. btrfs
does not implement that ioctl at all -- step 5.2 found that no file under
`fs/btrfs/` calls the kernel's `super_set_uuid()` (the call every other
UUID-reporting filesystem, including ext4 and xfs, makes), on any kernel
version, so the kernel's generic handler always returns `ENOTTY` for it;
`GetDeviceId` falls back to `BTRFS_IOC_FS_INFO`'s `fsid` field there
instead, the same UUID `btrfs filesystem show`/`blkid` report. The UUID is
stable across reboots on ext4, xfs and btrfs. There is no fallback to
`f_fsid` and no libmount: a filesystem without either ioctl (ZFS today) is
refused at startup with `Unimplemented`.

The database records the source filesystem's device id when it is created
(`cache_state.source_device_id`), and startup refuses a `--source` on a
different filesystem. It also refuses a different source directory on the
same filesystem, by comparing the root row's backing inode number,
generation and handle with the directory being opened: otherwise the old
directory's cached tree, whose handles still decode on this filesystem,
would be served under the new one.

### Why submounts are refused

A FUSE mount is one superblock with one `st_dev`. dcfs shows backing inode
numbers as `st_ino`, and inode numbers are only unique within one
filesystem: every ext4 root directory is inode 2, for example. If dcfs
served a second filesystem under the source, two different objects could
present the same `(st_dev, st_ino)` pair, and tools that rely on that pair
would misbehave: hard-link detection in `tar`, `rsync` and `cp -a`, and
GNU `find`'s loop detection (which was observed to refuse to descend
across such a boundary over NFS). Offsetting or remapping inode numbers
would break the promise that `st_ino` is the backing inode number.

So (amendment 12 of the plan):

- At startup, before mounting, `MountsBelow()` (`dcfs/mounts_below.h`)
  reads `/proc/self/mountinfo` and dcfs refuses to start if any mount
  point lies strictly below `--source`, naming them. A mount on `--source`
  itself is fine.
- A boundary that appears at runtime, which the startup check cannot see
  (a filesystem mounted later, or a btrfs subvolume, which is not a mount),
  is detected when its parent is listed or the name is probed:
  `IsBoundary()` in `backing.cc` compares the child's device numbers and
  mount id (`STATX_MNT_ID_UNIQUE`) with its parent's. The name is
  recorded as a `refused` dentry, logged at ERROR, left out of listings,
  and answered with `EXDEV` on lookup. It is never reported as absent.

The machinery for several filesystems is kept rather than deleted: device
ids in every row, the `filesystems` table, `MountFds` keyed by device id,
`IsBoundary`, and `StartupPurge`, which today only forgets
`filesystems` rows left by databases built before submounts were refused.

### How kernel submount support would plug in (not built)

The kernel already supports FUSE submounts: an entry whose attributes carry
`FUSE_ATTR_SUBMOUNT` becomes the root of a separate superblock with its own
`st_dev`, so inode numbers no longer have to be unique across filesystems.
Today only virtiofs can use it (the connection must opt in with
`auto_submounts`); a `/dev/fuse` server would need an INIT-time opt-in.
With that, dcfs would, at a boundary: open the child as the new
filesystem's mount fd and register it in `MountFds`; record a
`filesystems` row with the parent inode and boundary name; cache the
child's subtree like the source's; and set `FUSE_ATTR_SUBMOUNT` in the
entry reply. `StartupPurge` would go back to re-checking, at each start,
that every recorded filesystem is still mounted where it was found, and
forget the ones that are not. The comment at `LogRefusedBoundary()` in
`backing.cc` marks the spot.

## The schema

`dcfs/schema.sql` is the full definition, schema version 3. All tables are
`STRICT`. `Migrate()` (`dcfs/migrate.cc`) creates a fresh database in one
transaction, or upgrades an older one (one version step at a time) to
version 3 in one transaction, and refuses versions it does not understand.

### `cache_state`: one row of cache-wide state

| Column | Meaning |
|---|---|
| `id` | Always 1. |
| `schema_version` | The schema version (`kSchemaVersion` in `dcfs/migrate.h`). |
| `source_device_id` | `DeviceId::Serialize()` of the source filesystem, written once at creation. |
| `clean_shutdown` | 1 after a clean shutdown (backing synced, dirty set empty, WAL checkpointed); set to 0, durably, at every start. 0 at the next start means the last run crashed. |
| `boot_id` | `/proc/sys/kernel/random/boot_id` at the last start, so that recovery can log whether the machine rebooted (a kernel crash or power loss) or only the daemon died. |

Access goes through typed accessors in `dcfs/migrate.h`.

### `filesystems`

| Column | Meaning |
|---|---|
| `device_id` | Primary key. |
| `fstype` | `statfs` `f_type`, for messages. |
| `parent_inode` | The directory containing the filesystem's mount point; NULL for the source. Deleting that inode cascades to this row. |
| `boundary_name` | The mount point's name in that directory; NULL for the source. |

Today it only ever holds the source.

### `inodes`: one row per backing object

| Column | Meaning |
|---|---|
| `id` | The FUSE node id. Never reused. |
| `device_id` | References `filesystems`; deleting a filesystem cascades to its inodes. |
| `backing_ino`, `backing_gen` | Backing identity; see [Identity model](#identity-model). |
| `fuse_gen` | The FUSE generation: random nonzero 32-bit; 0 for the root. |
| `handle_type`, `handle` | The backing file handle. |
| `attrs_valid` | 1: the attribute columns are current. 0: unknown; the values are a stale hint and are never served. |
| `mode`, `nlink`, `uid`, `gid`, `rdev`, `size`, `blocks`, `blksize` | From `statx`. |
| `atime_*`, `mtime_*`, `ctime_*`, `btime_*` | Timestamps, seconds and nanoseconds. |
| `xattrs_complete` | 1: every xattr name without a row in `xattrs` is absent. 0: such a name is unknown. |

Unique on `(device_id, backing_ino, backing_gen)`.

### `dentries`: directory entries with explicit states

| Column | Meaning |
|---|---|
| `parent` | The directory. Deleting it cascades to its entries. |
| `name` | The entry name (a blob; any bytes but `/` and NUL). |
| `state` | `present`, `absent`, `unknown` or `refused`. |
| `inode` | The child, if and only if `state` is `present`. |

- `present`: the name exists and is `inode`.
- `absent`: the name is known not to exist (a negative entry).
- `unknown`: nothing is known (for example, phase 1 of a mutation that is
  about to change it, or the object it named was invalidated).
- `refused`: the name exists but is a mount point or subvolume boundary
  that dcfs will not cache. Never reported absent; lookups answer `EXDEV`.

A name with **no** row is absent if its directory's `children_complete` is
set, and unknown otherwise. The table is an ordinary rowid table on
purpose: the readdir cursor is a dentry's rowid, and updating an existing
entry in place (rather than deleting and reinserting it) keeps its rowid,
so a reader part way through a listing never sees an unchanged name twice.
An index on `inode` supports looking entries up by child.

Two partial indexes serve the directory-wide questions a readdir request
asks: `dentries_present (parent) WHERE state = 'present'` makes "the
present names of this directory after rowid `cursor`, in rowid order" a
range scan with no sort (an index's entries are ordered by rowid after its
key columns), and `dentries_unknown (parent) WHERE state = 'unknown'`
answers "does this directory have an unknown name" without looking at the
other entries. Without them SQLite used the primary key's index for
`parent = ?` and scanned and sorted every entry of the directory for each
page of 64 names, which made listing a directory of n names O(n^2): a
cached 10000-name listing took 0.7 s (optimized build; 2.4 s in the
unoptimized one) against 0.03-0.05 s on the backing filesystem, and now
takes about 0.08 s. `ListDir` and `IsDirComplete` must keep the literal
`state = '...'` in their SQL or the planner will not use the indexes; the
unit tests count SQLite virtual-machine instructions per page for that
reason.

**The inode-delete trigger.** Deleting an inode row (an invalidation, or a
cascade from its filesystem) must forget what the object was, not the
names that led to it. `inodes_delete_unknowns` turns every dentry pointing
at the deleted row into `unknown` (never `absent`) before the delete:

```sql
CREATE TRIGGER inodes_delete_unknowns BEFORE DELETE ON inodes BEGIN
  UPDATE dentries SET state = 'unknown', inode = NULL WHERE inode = OLD.id;
END;
```

An `ON DELETE SET NULL` foreign key, which the first schema used, would
have turned those names into negative entries, which read as absent.

### `directories`

| Column | Meaning |
|---|---|
| `inode` | The directory. |
| `children_complete` | 1: every name without a dentry row is absent. Only a full listing sets it; no single-name operation clears it. |
| `epoch` | Bumped by every write that clears `children_complete`. A listing records itself as complete only if the epoch did not move while it was reading (a compare-and-set; see [Concurrency](#concurrency-today-and-with-coroutines)). |

A listing can be served from the cache only if the directory is complete
**and** none of its rows is `unknown` (`cache::IsDirComplete`): a listing
must neither include nor silently omit a name whose state is unknown.

### `symlinks`

`inode`, `target`. Symlink targets are immutable for a given backing
object, so a target is either present or not cached; there is no unknown
state to track.

### `xattrs`: per-name states

| Column | Meaning |
|---|---|
| `inode`, `name` | Primary key. |
| `state` | `present`, `absent` or `unknown`. |
| `value` | The value, if and only if `state` is `present`. |

A row exists only where the name's state differs from what
`inodes.xattrs_complete` says about names without a row: `present` with
its value, `absent` while the set is incomplete, or `unknown` while the
set is complete (for example, during phase 1 of a mutation that is about to
change it). Only a full listing (`cache::ReplaceXattrs`) makes the set
complete; only `MarkXattrsUnknown` and recovery make it incomplete.

### `dirty`: the durable dirty set

`inode` (primary key, no foreign key). Every inode whose cached
attributes, dentries (as a parent), symlink target or xattrs a mutation has
changed since the backing filesystem was last synced. See
[Crashes, power loss and recovery](#crashes-power-loss-and-recovery). No
foreign key, because a row may outlive its inode (recovery skips it) and
must not vanish with it.

### Completeness flags, summarized

Both aggregate flags have exactly one meaning: **names without a row are
absent**. They are set only by a full listing (of a directory, or of an
inode's xattrs), and cleared only by something that may have added names
nobody knows about: an out-of-band change, recovery after a crash, or an
invalidation. A mutation never clears them; it marks only its own names
`unknown`.

## Population policy

**One spin-up per directory.** The first time a directory's contents are
needed and its listing is not complete, `backing::PopulateDirectory` reads
all of it: `getdents64`, then for each child `openat(dir_fd, name,
O_PATH | O_NOFOLLOW)`, `statx` (all basic attributes, birth time and mount
id), the generation, `name_to_handle_at`, the symlink target and every
xattr. All of this I/O happens first. Then one transaction upserts every
child's row, links its dentry, records refused boundaries, prunes cached
names that no longer exist, and marks the directory complete. After that
every lookup and listing in the directory is answered from the cache,
including lookups of names that do not exist (a complete listing makes a
missing name absent; `LookupOrPopulate` records it as a negative entry).

**Single-name resolve.** If the listing is complete but one name is
`unknown` (left by a mutation, a failed mutation, or an invalidation),
`backing::ResolveName` probes just that name instead of relisting the
directory. A failed mutation (an `rmdir` that gets `ENOTEMPTY`, a `mkdir`
that gets `EEXIST`) re-resolves its names the same way, so the common
failures cost one probe rather than a relist.

**Readdir** needs the listing recorded, not merely read, because its
offsets are dentry rowids: `DirCacheFS::ListCached` populates and
retries if a concurrent mutation prevented the listing from being recorded.
It takes the reply's entries from the cache right after the completeness
check that vouches for them, before any backing syscall ("." and "..",
which may need `ParentOf` or an attribute refresh, come after): a name made
unknown in between would otherwise be left out, since a listing skips
every entry that is not present (the model's finding
`readdirplus_unlocked`). Readdirplus additionally returns each child's
attributes, refreshing any that are unknown.

**What still touches the backing filesystem** when the cache is warm:
opening a file (contents go through passthrough), any mutation, `statfs`
(answered with `fstatfs` on the mount fd; normally served from the
superblock in memory), `fsync`, attribute refreshes of entries left
unknown, and, by design, nothing else.

## The write-through protocol

Every mutation runs in three phases:

```
 phase 1   one transaction, committed durably before the syscall:
           mark the records the mutation will change "unknown",
           insert the affected inodes into `dirty`
               |
 phase 2   the backing syscall(s), as the caller where it matters
               |
 phase 3   short transactions recording the new state, then refreshes
           (attributes, side-effect xattrs) as ordinary guarded fills;
           then the reply
```

### Phase 1

`cache::BeginMutation` runs the "mark unknown" writes and inserts the
affected inodes into `dirty`, in one transaction committed with
`sqlite3::Durability::kSync`: `PRAGMA synchronous=FULL` just before
`BEGIN` and `synchronous=NORMAL` again after `COMMIT` (SQLite refuses to
change it inside a transaction). With WAL and `FULL`, the commit fsyncs the
WAL. The phase 1 of each mutation kind is one named function:

| Mutation | Phase 1 (`cache::Begin*`) | Dirty |
|---|---|---|
| create, mknod, mkdir, symlink | the new name unknown; parent's attributes unknown | parent (the new child is added in phase 3) |
| unlink, rmdir | the name unknown; parent's and child's attributes unknown (after verifying the resolved child, see below) | parent, child |
| rename (incl. `RENAME_NOREPLACE`, `RENAME_EXCHANGE`) | both names unknown; attributes of both parents, the source and any replaced target unknown | all of those |
| link | the new name unknown; new parent's and source's attributes unknown | new parent, source |
| setattr, writable open, fallback write, fallocate | attributes unknown; side-effect xattrs unknown | the inode |
| setxattr, removexattr | that one xattr name unknown; attributes unknown (ctime changes) | the inode |

**The fast path.** If every inode a phase 1 names is already durably
dirty since the last sync point (tracked in memory in
`Context::dirty.durable`), the transaction commits at normal durability:
recovery after a crash will forget all of those inodes' state anyway, so
nothing this phase 1 writes needs to survive a power loss. A burst of
creates in one directory costs one WAL fsync, not one per file.

### Phase 2

The backing syscall, through `backing::`. Syscalls whose outcome depends on
who makes them run with the caller's filesystem credentials (see
[Caller credentials](#caller-credentials)). A failure is replied as is,
after re-resolving the names phase 1 made unknown (one probe each).

### Phase 3

Short transactions record the outcome: the new dentry state (present or
absent), the new child's row (made dirty too, in the same transaction), a
read-back xattr value. Phase-3 writes that are not fills themselves are
made only if the mutation still `Owns()` the inode, meaning no other
mutation of it began since this one's phase 1 and none is in flight (see
[Concurrency](#concurrency-today-and-with-coroutines)); otherwise the
record stays `unknown`. Attribute refreshes after that run as ordinary
guarded fills.

**Once phase 2 has succeeded, nothing is replied as a failure.** A
phase-3 error is logged at WARNING and leaves what it did not record
`unknown` (`DirCacheFS::LogPhase3Failure`). Replying an error for a
rename that happened would make the caller believe nothing changed and
leave the kernel's own dentries wrong.

**Phase 3 never removes dirty rows.** Only a sync point does.

**Kernel caches after dcfs's own mutations** are kept right by the kernel
itself: it invalidates the parent's attributes and dentries for the
operations it forwards (`fuse_dir_changed`, `fuse_update_ctime`,
`fuse_entry_unlinked`). dcfs sends no notifications.

### Row lifetime

Removing a dentry never deletes an inode row by itself, because the number
of cached dentries is not the link count (only some links may be cached).
After an unlink or a rename over an existing file, `SettleUnlinkedFile`
applies the rule: if dcfs holds an open of the file, its attributes are
refreshed from that descriptor and the row is kept until the last release,
which deletes it if the link count is then 0; otherwise the row is deleted
if the backing object is gone or has no links left. A removed directory's
row is deleted at once. Attributes with `nlink` 0 are never recorded as
current, so a crash cannot leave a row serving a deleted file as current.

**Removed objects the kernel still references.** The kernel can keep a
nodeid after its object is removed: a process's working directory, an
`O_PATH` descriptor, a dentry still in use. It keeps sending requests for
it (`GETATTR` for a `stat`, `OPENDIR` for an open of `.`, `GETXATTR` for
its ACLs) until its last `FORGET`. dcfs counts the kernel's lookups in
memory (`lookups_`: +1 for every entry reply that hands out a nodeid --
lookup, mknod, mkdir, symlink, link, create, and readdirplus entries other
than `.` and `..`, which the kernel does not count -- and -n for every
`FORGET`). Before the backing syscall of an unlink, rmdir or rename-over,
dcfs opens an `O_PATH` descriptor on the object if the kernel holds its
nodeid. If the object turns out to be gone (or, for an open file, at its
last release), its row is still deleted at once, but the descriptor and a
copy of the row go into an in-memory record (`removed_`) that answers the
kernel's reads (attributes with `nlink` 0, xattrs, a symlink's target,
`statfs`) until the last `FORGET` closes it. The descriptor keeps the
backing object alive exactly as the kernel's reference would on a local
filesystem.

Nothing about such an object is in the database, so nothing about it
survives a crash or a restart (the kernel of a new mount holds no
nodeids), and an NFS handle to it fails with `ESTALE` as it should: the
`.` lookup that resolves a handle (and every mutation) uses the cache
rows only. Changing a removed object, or reopening an unlinked file, also
fails with `ESTALE` (listed in the README's Limitations).

## Crashes, power loss and recovery

### A daemon crash

After a daemon crash the database is exactly its last committed state
(every commit reached the WAL with `write(2)`, which the page cache keeps),
and every backing syscall that returned is kept. Each mutation is
therefore safe at every commit boundary: between phase 1 and phase 3 the
affected records read `unknown` and are re-read on demand.

### A power loss or kernel crash

This is harder. SQLite runs in WAL mode at `synchronous=NORMAL`, so an
ordinary commit is not fsynced, and the backing filesystem commits its
journal on its own schedule. Each comes back as some prefix of what was
written, independently. Without further measures the database could come
back **behind** the backing filesystem (phase 1 lost, the syscall kept: a
deleted file still cached as present) or **ahead** of it (phase 3 kept, the
syscall lost: a created file cached as present, or an unlinked name cached
as absent while the file still exists, invisible forever).

The durable dirty set closes both:

- **Behind:** phase 1's dirty rows are fsynced before the syscall is
  issued. If the syscall survived, the dirty rows did too.
- **Ahead:** dirty rows are removed only by a sync point, after `syncfs`
  has made the backing filesystem durable. If the syscall was lost, its
  dirty rows are still there (the phase 1 commit was durable, and nothing
  removed them since).

At startup, if `clean_shutdown` is 0 or the dirty set is not empty,
`cache::RecoverDirty` forgets everything cached about each dirty inode, in
one transaction: attributes unknown, xattr rows deleted and the set
incomplete, the symlink target dropped, every dentry in it (as a
directory) forgotten and the listing incomplete (a lost change may have
added names), and every dentry pointing at it unknown (its name may have
changed). Inode rows are kept, so NFS handles still resolve and are
verified when next opened. The dirty set is then emptied, and a WARNING
reports the count and whether `boot_id` changed.

So a power loss costs re-reading the entries mutated since the last sync
point, and never serves state the backing filesystem did not keep.

### Sync points

`backing::SyncBacking`: `syncfs(2)` on every mount fd, then, if all
succeeded, empty the dirty set in one transaction, except inodes with a
writable open outstanding (the kernel may still be writing to them through
passthrough) and rows the `syncfs` may not cover. On a `syncfs` failure
nothing is cleared.

A dirty row may go only once a `syncfs` that began after its mutation's
backing syscall has returned. So the sync point first takes a snapshot
(`cache::BeginSync`: the fill guards' clock and the dirty set), then runs
`syncfs`, then (`cache::ClearDirty`) removes only rows that were in the
snapshot and whose inode no mutation began or ended since the snapshot or
has in flight (`CanFill`'s test). A row added after the snapshot, or whose
inode a mutation was changing meanwhile, waits for the next sync point.
Today nothing runs during a sync point; under coroutines a mutation may
issue its syscall while the sync point waits on `syncfs`, or the sync point
may run while a mutation waits on its syscall (the model's finding
`sync_during_mutation`). If the fill guards were pruned since the snapshot
(their floor passed it), every row stays.

Writes through a writable open count the same way. dcfs never sees them
one by one: they lie between the open's phase 1 (`BeginWriting`) and the
last writable `Release`. So a row stays if its inode was open for writing
at any moment between the snapshot and the clear: open at the snapshot
(the snapshot lists `Context::open_for_write`), open at the clear (the
writable opens outstanding then), or opened or released in between (both
are guard events: `BeginWriting` is a phase 1, and the last release calls
`cache::EndWrites`, which advances the clock and touches the inode before
the release records anything). Without that, a release while the sync
point waits on `syncfs` could let the clear drop the row of a file whose
last writes the `syncfs` did not cover, and a power loss could then keep
the attributes the release recorded and lose the writes. The snapshot of
the writable opens is a backstop: the guard event alone covers a release
after the snapshot.

When nothing moved during the sync point (the guards' clock is where the
snapshot left it and no mutation is in flight, which is every sync point
today), `ClearDirty` empties the table in one statement and puts back the
kept rows: every row then is in the snapshot and passes the per-row test,
so the result is the same, without a statement per row.

Sync points run:

- after the kernel's `FSYNC` or `FSYNCDIR` (after the fsync itself),
  because the caller asked for what it did to be durable, and that
  includes what dcfs cached about it;
- at the start of the first request `--sync_interval_sec` (default 5)
  after the previous sync point, while the dirty set may be non-empty
  (`DirCacheFS::MaybeSyncBacking`, called from `fuse_ops.cc`);
- at clean shutdown (`backing::FinishRun`).

dcfs is single-threaded inside libfuse's blocking loop, which has no idle
hook, so there is no timer: an idle daemon keeps a non-empty dirty set
until its next request, fsync or shutdown. That is safe; it only makes the
re-read after a crash larger. (libfuse 3.18.2 has no `SYNCFS` handler, and
the kernel only sends `SYNCFS` to fuseblk servers anyway.)

### Why not `synchronous=FULL` everywhere

- **It does not fix "ahead".** Making every commit durable does nothing
  about a backing change that was lost after dcfs recorded it. That needs
  the backing change to be durable before the record is, which per
  mutation means an fsync of the backing directory or a `syncfs`: roughly
  10 to 30 ms per operation on spinning disks. The dirty set gets the same
  guarantee with one `syncfs` per sync interval.
- **It costs the read path.** Every fill (populating a directory, caching
  a negative lookup, refreshing attributes) commits; with `FULL` each would
  pay a flush.
- **It costs mutations several times over.** A create commits about five
  times (phase 1, the new child, the parent refresh, and so on); only phase
  1 needs to be durable.

Measured during development, in QEMU on a virtual disk: about 3.3 ms per
create with the fast path, against 9.4 ms when every phase 1 flushed. For
real devices the race audit estimated one WAL flush at 20 to 60 µs on an
SSD with power-loss protection, 0.2 to 2 ms on a consumer NVMe drive
without it, and 1 to 5 ms on a SATA SSD; an untar-like workload issues two
to four phase-1 commits per file.

### What the clean-shutdown flag adds

`StartRun` writes `clean_shutdown = 0` and the boot id durably before the
first request; `FinishRun` writes 1 durably only after a sync point and a
WAL checkpoint, and only if the dirty set is then empty (a writable open
outstanding at shutdown, possible after a lazy unmount, leaves it 0).
Recovery runs on either signal: a 0 flag, or a non-empty dirty set.

## Concurrency, today and with coroutines

### Today

dcfs serves one request at a time on one thread, in libfuse's
`fuse_session_loop`. `DirCacheFS::Init` explicitly unsets
`FUSE_CAP_OVER_IO_URING`: libfuse enables it by default when the kernel
offers it, and with `FUSE_URING_ENABLE=1` in the environment or
`-o io_uring` it would serve requests from one thread per CPU, against
state that has no locking. dcfs does not request
`FUSE_CAP_PARALLEL_DIROPS`, so the kernel also serializes lookups and
readdirs per directory.

Within one thread, nothing can interleave with a request, so today every
request sees the cache in a consistent state between its own steps.

### Rules that hold now so that coroutines need no redesign

The planned architecture (not built) runs requests as C++ coroutines over
io_uring, with several requests in flight on one thread and possibly
several threads. Every backing syscall becomes a suspension point, and so
might every SQLite call if SQLite's I/O moves onto the ring through a
custom VFS. The code already follows the rules that make that safe:

- **One SQLite connection per thread (per ring), shared by its
  coroutines.** Transactions never span a suspension point: backing I/O
  first, then one short synchronous transaction. Connection pooling is only
  needed if transactions must span awaits, which write-through never
  requires.
- **Fill guards** (`cache::BeginFill`, `CanFill`, `Mutation`;
  `FillGuards` in `dcfs/context.h`). A *fill* reads the backing filesystem
  and then records what it read as present: an attribute refresh, an xattr
  refresh, a directory population. Without a guard, a fill that read before
  a mutation's syscall landed could commit after the mutation's phase 3,
  overwriting the fresh state with stale state forever. The guard:
  - a logical clock `seq`, advanced by every mutation's phase 1 and end;
  - `inflight[id]`: how many mutations of the inode are between phase 1
    and their end;
  - `touched[id]`: `seq` at the latest phase 1 or end of a mutation of the
    inode (pruned past `max_touched` entries, 65536 by default, by raising
    a `floor` below which every snapshot is invalid; also the end of a
    file's writable open, `cache::EndWrites`);
  - a fill takes a snapshot of `seq` before its first syscall, and may
    record something about inode `id` only if, at commit time and in the
    commit's transaction, no mutation of `id` is in flight and none began
    or ended since the snapshot. Otherwise it records nothing and answers
    its own caller from what it read.

  The guard lives in memory, not in the database: it protects only
  in-process concurrency (a crash makes everything a mutation touched
  unknown through the dirty set anyway), and a fill must be able to check
  rows it did not know about when it started (a population's children).
- **Compare-and-set completeness.** A directory listing is recorded as
  complete only if no mutation of the directory interfered (the fill guard)
  and its `epoch` did not move while it was reading (something else, such
  as an invalidation or an out-of-band reconciliation, cleared its
  completeness meanwhile).
- **Mutations own their phase-3 writes** only while no overlapping
  mutation of the same inode exists (`Mutation::Owns`).
- **A sync point clears only what its `syncfs` covers**: rows in its
  snapshot of the dirty set whose inode no mutation touched since (see
  [Sync points](#sync-points)).
- **A check and what it vouches for have no suspension point between
  them.** A directory listing is taken from the cache right after the
  completeness check (`DirCacheFS::ListCached`), and everything that needs
  a syscall comes after.
- **A mutation verifies in phase 1 what it resolved before it.** `Rename`
  resolves its source and destination (which may take syscalls) and phase
  3 links the names to those ids, but `Mutation::Owns` only notices
  overlaps from phase 1 on. So `Rename` takes a fill snapshot before
  resolving, and `cache::BeginRename` checks, in phase 1's transaction,
  that no mutation of the parents, the source or the destination began or
  ended since or is in flight; if one did, it writes nothing and `Rename`
  resolves again (a few times, then `EAGAIN`). `RemoveChild` (unlink,
  rmdir) does the same for the parent and the child it resolved
  (`cache::BeginRemove`): its `unlinkat` removes whatever the name holds
  when it runs, while phase 1 marks the resolved child's attributes
  unknown and phase 3 settles that child's row. Without the check, a
  rename onto the name between the resolve and phase 1 would make the
  unlink remove a different object, whose link count would stay cached as
  current, and settle the wrong row.
- **Credential switches never span a suspension point.** `AsCaller` wraps
  exactly one synchronous syscall, and the switch is per thread (see
  [Caller credentials](#caller-credentials)).
- **Replies can be deferred.** `FuseRequest` is movable and owns exactly
  one reply; libfuse allows replying from any thread.

Places still marked `TODO(coroutines)`: `ListCached` retries a
population a few times when a concurrent mutation keeps it from being
recorded, and `Rename` and `RemoveChild` retry their resolve when phase 1
finds it stale, where a coroutine would wait for the mutation instead.
These loops are not real retries yet: when the resolve is a cache hit,
nothing suspends between the attempts, so an overlapping mutation still in
flight makes every attempt fail at once (`EAGAIN`, which `rename(2)` and
`unlink(2)` callers do not expect). The coroutine design needs "wait until
the overlapping mutation ends" there. The
status macros embed `return` and will need `co_return` variants.

## Writable opens and file contents

### One backing file per inode

`Open` and `Create` reopen the object by handle and register the
descriptor with the kernel for passthrough (`fuse_passthrough_open`). The
kernel refuses a second, different backing file for one inode (`EBUSY` in
`fuse_inode_uncached_io_start`), and every `fuse_passthrough_open` call
creates a new kernel-side backing object even for the same descriptor. So
dcfs keeps one `BackingFile` per inode, shared by every open of it,
registered once: opened `O_RDWR` if possible, falling back to `O_RDONLY`
only for `EACCES`, `EROFS` or `EPERM` (an immutable file, a read-only
filesystem). Whether a given open may write is still decided by the
kernel from the FUSE file's own mode; a read-only open cannot write
through a shared mapping even though the backing descriptor is `O_RDWR`.
If the kernel did not grant passthrough, `Read` and `Write` serve the data
through the same descriptor.

`FUSE_CAP_ATOMIC_O_TRUNC` is turned off, so an `O_TRUNC` open arrives as
an OPEN followed by a separate `SETATTR(size=0)`, which the ordinary
write-through setattr path handles.

### Attributes while a file is open for writing

With passthrough, dcfs never sees the writes. A writable open is therefore
phase 1 of a mutation (`DirCacheFS::BeginWriting`): before the open is
replied to, the file's cached attributes, and the side-effect xattr
`security.capability`, are marked unknown and the inode becomes durably
dirty. They stay unknown until the last writable open of the file is
released (phase 3), however often they are read meanwhile:

- every attribute write for an inode in `Context::open_for_write` stores
  the fresh values but keeps them marked unknown, in the same transaction;
- attribute reads are answered from a `statx` of the shared open
  descriptor, which costs no reopen and no disk access;
- replies carry an attribute timeout of 0, so the kernel asks again each
  time rather than caching values that writes through a shared mapping
  would not invalidate;
- the inode stays in the dirty set across sync points until the first
  sync point that begins after that last release.

`Flush` and `Fsync` refresh the attributes from the descriptor; the last
writable `Release` records them as current and reads `security.capability`
back. A crash while the file is open leaves its attributes unknown, never
the pre-write size and mtime marked current.

The last writable `Release` first tells the fill guards the writes are over
(`DirCacheFS::EndWriting`: `cache::EndWrites` touches the inode, and it
leaves `open_for_write` in the same synchronous step), and only then
refreshes the attributes. A fill that read them while the file was open
can then not record them over the release's fresher ones, and a sync point
whose `syncfs` began before the last writes keeps the dirty row (see
[Sync points](#sync-points)). A writable `Create` runs `BeginWriting` as
`Open` does, right after registering its `BackingFile` and before the
reply: the new row is dirty from phase 3's `MarkDirty`, which no guard
sees, so a sync point while the create waits on a later syscall could clear
it, and the writes after the reply would have no dirty row.

### The mmap caveat

`mmap` of a passthrough file swaps the mapping's file to the backing file
(`backing_file_mmap`, `vma_set_file`), so the mapping no longer holds the
FUSE file. `close()` after `mmap(MAP_SHARED, PROT_WRITE)` therefore drops
the last reference and the kernel sends RELEASE while the mapping is still
writable. Stores through it reach the backing file with no FUSE request at
all. dcfs records the attributes as current at that release, and later
stores change the backing mtime, ctime and size without dcfs knowing. NFS
clients, which detect changes from ctime because FUSE reports no change
cookie, may then keep stale data. This is documented as unsupported
(amendment 16). The proper fix is in the kernel: keep the FUSE file
referenced by passthrough shared writable mappings until `munmap`, so that
RELEASE means no more writers. Its feasibility is unverified.

## Out-of-band change detection

dcfs requires exclusive access and does not look for changes made behind
its back: no fanotify, no revalidation timer. But it compares wherever a
fresh `statx` is already in hand, at no extra syscall (amendment 9):

- `backing::OpenNode`, which `statx`es every object it reopens by handle
  to verify its identity;
- `PopulateDirectory`, which `statx`es every child.

If the object is no longer the one the row describes (a recycled inode
number), the row is invalidated and the caller gets `ESTALE`. If it is the
same object but its cached attributes are marked current and differ in
mode, owner, group, link count, size, mtime or ctime,
`backing::ReconcileAttrs` logs one WARNING ("out-of-band change on the
backing filesystem (unsupported)") naming the fields, and then:

- adopts the fresh attributes;
- for a directory whose mtime or ctime changed, forgets its negative and
  refused entries and marks its listing incomplete, so that new names
  become visible on the next lookup or readdir (positive entries are kept,
  which keeps subdirectories' `..` resolvable);
- for any object whose ctime changed, marks its xattrs unknown.

atime and block counts are not compared: reads and delayed allocation
change them legitimately. Nothing is compared when the cached attributes
are already unknown, or when a dcfs mutation of the inode began, ended or
is in flight since the cached values were read (that would be a false
positive). The QEMU tests check that dcfs's own mutations never produce
the warning.

**Not detected:** anything answered purely from the cache. A `stat`, a
lookup, a readdir or an xattr read that needs no backing I/O notices
nothing.

### Why the kernel is not told

dcfs updates its own cache but does not call
`fuse_lowlevel_notify_inval_entry` or `_inval_inode` (amendment 11). The
kernel keeps its cached attributes and dentries until their timeouts expire
or it evicts them. The reason is a deadlock in the single-threaded design:

- A notification is a write to `/dev/fuse` that the kernel processes
  synchronously in the writer's context. `inval_entry` takes the parent
  directory's inode lock (`fuse_reverse_inval_entry`).
- Detection happens while dcfs is serving a request, and the kernel holds
  locks for that request until dcfs replies: the parent's `i_rwsem`
  exclusively for a create, unlink or rename, shared for a lookup or
  readdir.
- If dcfs's only thread writes a notification that needs one of those
  locks, it blocks on a lock that is released only when it replies, which
  it can no longer do.

Doing it safely needs a separate notifier thread that sends notifications
asynchronously while the request thread carries on. That is not worth its
complexity for changes that are unsupported anyway; the accepted cost is
staleness until the kernel's timeouts expire. (The deadlock analysis is
from reading the kernel source; it was not reproduced in a test.)

## Caller credentials

dcfs runs as root, but a backing syscall made for a FUSE request must
behave as if the caller had made it: new objects must belong to the caller
(and take their group from the caller or from a setgid parent), and the
rules for chown, utimes, truncate, xattrs and sticky directories must be
decided for the caller. `AsCaller` in `backing.cc` switches the thread's
filesystem credentials around exactly one syscall, the approach virtiofsd
and nfsd take:

- `setfsgid`, then the caller's supplementary groups through the raw
  per-thread `setgroups` system call (glibc's wrapper applies it to every
  thread), then `setfsuid`; each is read back, since `setfsuid` and
  `setfsgid` report no errors, and a switch that did not take fails with
  `EPERM`;
- afterwards, back to root; failing to restore is fatal, since carrying on
  would perform later operations as the wrong user.

Caller identity comes from the FUSE request (`fuse_req_ctx`) and the
supplementary groups from `fuse_req_getgroups`.

**Run as the caller:** `mkdirat`, `mknodat`, `symlinkat`,
`openat(O_CREAT)`, `unlinkat`, `renameat2`, the chown (including the
otherwise empty `chown(path, -1, -1)` the kernel forwards, which still
updates ctime), utimes, the truncate, `setxattr` and `removexattr`.

**Stay root:**

- reaching objects: `open_by_handle_at` needs `CAP_DAC_READ_SEARCH`, which
  the kernel drops while fsuid is not 0 (together with every other
  filesystem capability), and the `/proc/self/fd` reopens;
- `linkat` with `AT_EMPTY_PATH` (also needs `CAP_DAC_READ_SEARCH`; a new
  link creates no new owner or group, and the kernel already checked the
  caller's permissions on the FUSE side);
- chmod: the kernel itself sends a mode change to clear setuid and setgid
  after a truncate or chown by a non-owner, which the caller's own chmod
  would be refused; a chmod the caller asked for has already been checked
  by the kernel;
- opening an existing file, and every probe, read and refresh.

The mount always uses `default_permissions`, so the kernel checks the
caller's permissions against dcfs's cached attributes before a request is
sent; `Access` has nothing left to check.

**POSIX ACLs** are enforced by the kernel: dcfs requests
`FUSE_CAP_POSIX_ACL`, and the kernel then fetches
`system.posix_acl_access` and `system.posix_acl_default` through
`GETXATTR` (answered from the cache like any xattr; see below) and uses
them in its permission checks. Without it the kernel would check the mode
bits alone, and on a file with an ACL the group bits are the ACL mask, so
named-user and named-group entries would be ignored. The kernel leaves the
rest to the filesystem, and dcfs leaves it to the backing filesystem:
setting an ACL updates the mode (the backing setxattr runs as the caller,
so the backing filesystem also clears setgid for a caller outside the
file's group), a chmod rewrites the ACL (read back as a side effect), and
a create inherits the parent's default ACL. dcfs refuses to mount if the
kernel does not offer `FUSE_CAP_POSIX_ACL`.

**The umask.** Default ACL inheritance means the caller's umask must not
be applied where the parent has a default ACL, so it cannot be applied
before the request reaches dcfs. dcfs requests `FUSE_CAP_DONT_MASK`: the
kernel sends `CREATE`, `MKDIR` and `MKNOD` with the requested mode unmasked
and the caller's umask alongside (`fuse_ctx::umask`), and `AsCaller` makes
that the process umask around the backing syscall, so the backing
filesystem applies it, or the default ACL instead, exactly as for a local
create. The umask is per process, not per thread, which is fine while dcfs
is single-threaded; worker threads will need `unshare(CLONE_FS)`, as
virtiofsd does. Outside a switch dcfs's umask is 0 (set at startup), so the
umask it inherited never alters a mode on the backing filesystem.

## Extended attributes

xattr names and values are cached like everything else; only file
contents bypass the cache.

- **Per-name states.** Each name is present (with a value), absent or
  unknown, as described in [the schema](#the-schema). `getxattr` of an
  unknown name resolves just that name with one backing `getxattr`;
  `listxattr` needs the whole set known and refreshes it with one
  `listxattr` plus a `getxattr` per name otherwise.
- **Setxattr and removexattr** mark only their own name unknown in phase 1
  (and the attributes, since ctime changes). After a successful
  `setxattr`, phase 3 reads the value back through the same object and
  records **what the backing filesystem stored**, not what the caller sent.
  The two differ: ext4, xfs and btrfs store a `system.posix_acl_access`
  that is exactly equivalent to the mode as no xattr at all, and update the
  mode instead.
- **Side-effect names** (the list in `dir_cache_fs.cc`): some operations
  other than setxattr change xattrs on the backing filesystem. Their phase
  1 marks the affected names unknown and their phase 3 reads them back:
  - `system.posix_acl_access` on any chmod (including the chmod dcfs makes
    to clear setuid or setgid), which rewrites the ACL to match the mode;
  - `security.capability` on chown or chgrp (even `chown(-1, -1)`),
    truncate, and any write or fallocate, which remove it.

  The kernel also removes `security.capability` itself through the mount
  (dcfs does not request `FUSE_CAP_HANDLE_KILLPRIV`), so the read-back is a
  backstop for what the backing filesystem does on its own. LSM relabeling
  and EVM/IMA rewrites are not covered.

## File names are bytes

A name (a directory entry, a symlink target, an xattr name) is a sequence
of bytes: any byte but NUL, and but `/` in a name. dcfs never decodes,
normalizes, folds or reorders one, because nothing about a Linux
filesystem allows it to. WTF-8 and similar schemes were rejected: they
round-trip ill-formed UTF-16, not arbitrary bytes (a lone `0xFF` has no
WTF-8 form).

- **Storage.** Names, targets and xattr names are `BLOB` columns, bound as
  blobs and ordered by `memcmp`; in C++ they are `std::string` and
  `std::string_view`, never `char *`. The only C strings made from them
  are the arguments of the backing syscalls and of libfuse's reply
  functions, where a NUL cannot occur anyway.
- **Printing.** A name must never reach a log line, an error message or
  any other text raw: a name can contain a newline (forging a log line),
  terminal control characters, or invalid UTF-8. `EscapeBytes`
  (`dcfs/escape.h`) is the one escaping for logs and error text: printable
  ASCII stays, `\`, `"`, newline, carriage return and tab get a backslash
  escape, and every other byte becomes `\xNN`. `UnescapeBytes` is its exact
  inverse. Every call site that prints a name, a symlink target or an xattr
  name goes through it. `mountinfo`, `fstab` and `exports(5)` use a
  different, octal escaping (`\040`); the functions for it arrive with
  plan phase 15, next to these.
- **Tests.** `names_test` runs a corpus built from where a name can
  break rather than from every byte value; see the table under
  [Test strategy](#end-to-end-suites-and-what-each-proves).

## Startup and shutdown

### Startup (`main.cc`)

1. `umask(0)`; parse flags.
2. Open `--source` (`O_RDONLY | O_DIRECTORY`). This is the only use of the
   path.
3. Submount check: refuse to start if `/proc/self/mountinfo` shows any
   mount point strictly below `--source`.
4. Cache database directory: create it mode 0700 if it does not exist; warn
   (but still start) if an existing one is group- or world-accessible. The
   cache holds metadata as sensitive as `--source`'s -- every cached name,
   attribute, xattr and symlink target, including those of directories a
   reader cannot list -- so it must not be readable by anyone but root.
5. Open `--cache_db` (and check any existing `-wal`/`-shm`) with
   `O_NOFOLLOW`, creating the database mode 0600 (SQLite gives the
   `-wal`/`-shm` files it creates the same mode). Refuse to start if the
   path is a symlink or not a regular file. Refuse if the file grants
   more access than `--source`'s root directory does: its owner must be
   root or that directory's owner; group read/write only if its group is
   the directory's group and the directory grants the group the same;
   other read/write only if the directory grants others the same. The
   error names both sets of owner, group and mode. A file that passes but
   is not mode 0600 (made by an older build) is `fchmod`ed to 0600 with a
   WARNING. This is a configuration check, not a defense against a
   hostile cache directory: the directory is not treated as a trust
   boundary (decision 2026-10-06). Then take an exclusive, non-blocking
   `flock`; refuse to start if another process holds it.
6. Open the SQLite connection: WAL, `synchronous=NORMAL`, foreign keys on,
   `busy_timeout=5000`, `temp_store=MEMORY`. dcfs refuses to start if
   SQLite cannot put the database in WAL mode (a filesystem without the
   shared memory WAL needs): in rollback-journal mode a `NORMAL` commit
   is not durable.
7. Probe the root: its device id (`FS_IOC_GETFSUUID`, or `BTRFS_IOC_FS_INFO`
   on btrfs -- see "Device ids" above), filesystem type,
   inode number and generation.
8. `Migrate()`: create the schema and seed the `cache_state` row, the
   source's `filesystems` row and the root row, or upgrade an older
   schema, then check that the root row exists.
9. Refuse a database built for a different filesystem
   (`source_device_id`), or for a different directory on the same one (the
   root row's inode number, generation and handle).
10. Read the boot id; `backing::StartRun`: recover the dirty set if the last
    run was not clean or anything is dirty, then durably record
    `clean_shutdown = 0` and the boot id.
11. `backing::InitRoot`: refresh the root row's handle and attributes and
    register the source descriptor as the source filesystem's mount fd.
12. `backing::StartupPurge`: forget any non-source `filesystems` row (left
    by databases built before submounts were refused).
13. Mount with `default_permissions`, plus `allow_other` if requested,
    plus `--fuse_opt`; install libfuse's signal handlers; daemonize if
    asked; run the session loop. `Init` requests export support,
    readdirplus, symlink caching and passthrough; requires POSIX ACLs and
    `FUSE_CAP_DONT_MASK` (refusing the mount without them); turns off
    atomic `O_TRUNC` and FUSE-over-io_uring; and logs whether the kernel
    granted passthrough.

### Shutdown

`SIGTERM`, `SIGINT` or `SIGHUP` stop the session loop (libfuse's
handlers), as does an external unmount. Then, in order:

1. unmount, so the kernel stops sending requests;
2. remove the signal handlers and destroy the session;
3. `backing::FinishRun`: a sync point (`syncfs`, empty the dirty set), a
   `TRUNCATE` WAL checkpoint, and, if the dirty set is then empty,
   `clean_shutdown = 1` committed durably. A failure is logged and leaves
   the flag at 0, so the next start recovers;
4. close the database (releasing the lock with the process).

After a crash the dead FUSE mount stays in place and must be unmounted
before dcfs can start again.

## Test strategy

### Everything runs in QEMU

dcfs requires root, real `open_by_handle_at`, `FS_IOC_GETFSUUID` and FUSE
passthrough, so there is no host-side test execution at all (amendment 3).
Every test target, unit tests included, boots the project's own minimal
kernel in a QEMU guest and runs there as root. To make that cheap
(amendment 4), the runner (`test/qemu/scripts/run-qemu.sh`) boots the
`microvm` machine type with qboot and direct PVH kernel boot, no PCI, no
ACPI, virtio-mmio disks, a kernel trimmed to what the guests use, KVM, and
a small amount of memory. The kernel reaches userspace in about a second.
`qemu_cc_test` wraps a GoogleTest binary in a per-test initramfs;
`qemu_test` runs a guest shell script against the real daemon. Test-only
helpers are fakes, never mocks, and live in test files or `testonly`
targets; syscall fault injection uses link-time `--wrap` fakes, so that
production code carries no test hooks.

### Test-first

Every bug found (by audits, review or test failures) gets a regression
test first, shown to fail on the unfixed code, then the fix (amendment 19).
Where a bug cannot be reproduced deterministically (coroutine-only
interleavings, a real power loss), the closest honest test is written and
its limits are stated.

### Conformance

`pjdfstest_test` runs all of pjdfstest (238 files, about 8800 checks) as
root inside the guest, once through a dcfs mount over ext4 and once
directly on the same ext4 filesystem, and diffs the failure sets. Any
dcfs-specific failure not in the (now empty) baseline fails the test. dcfs
and raw ext4 currently fail the same 28 checks. It also checks that dcfs
logged no out-of-band warning. See `docs/conformance.md`.

### End-to-end suites and what each proves

Each runs the real daemon on a scratch ext4 disk (`/dev/vdb`, with a
second disk where a test needs a second filesystem), checks results both
through the mount and directly on the backing filesystem, and in most
cases repeats after killing and restarting the daemon against the same
database. "Zero sectors" below means the backing device's read counter in
`/sys/block/<dev>/stat` did not move after dropping every kernel cache.

| Test | What it proves |
|---|---|
| `boot_test` | The guest environment works: dcfs and helpers are present, disks mount. |
| `readonly_test` | Read-only operations are served from the cache with backing inode numbers; a warm metadata pass reads zero sectors, also after a restart; startup refuses a mount below `--source`, and a boundary that appears at runtime is refused rather than cached. |
| `passthrough_test` | File contents go through passthrough: reads match, move the backing read counter, and cost the daemon almost no CPU for 64 MiB; opens do not leak descriptors; all of it survives a restart. |
| `lifecycle_test` | Flag validation, a bad `--source`, a database for another filesystem refused, `--fuse_opt`, clean `SIGTERM` shutdown (exit 0, unmounted, WAL checkpointed), mounting over the source, restarting against a used database. |
| `handles_test` | NFS-style handles via `name_to_handle_at`/`open_by_handle_at`: generation 0 for the root and nonzero and stable otherwise; handles survive a restart; a doctored generation, a recycled inode number and a wiped database each give `ESTALE`; no handle can be made behind a refused boundary. |
| `setattr_test` | chmod (file, directory, FIFO; `EOPNOTSUPP` on a symlink), chown, truncate and utimes land on the backing filesystem and are then served from the cache with zero sectors, also after a restart. |
| `create_test` | mkdir, create, mknod, symlink and link, including error cases; the whole tree's listing agrees with the backing filesystem; zero sectors for a full metadata pass over everything created; `EXDEV` at a refused boundary. |
| `rename_test` | unlink (including of an open file, whose row and handle live until the last close), rmdir, and every rename variant (across directories, over an existing file, `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, a directory with its cached subtree); negative entries and completeness are recorded, not re-read. |
| `write_test` | Writes, appends, `O_TRUNC`, a 64 MiB passthrough write, concurrent opens of one file (the one-backing-file rule), fsync, fallocate, xattrs on files, directories and symlinks, ACL read-back after setxattr and chmod, `security.capability` removal on chown, truncate and write; served from the cache after a restart. |
| `credentials_test` | As two unprivileged users: ownership of every create, setgid inheritance, supplementary groups, chown and chgrp rules, sticky directories, truncate, utimes, chmod and user xattrs, allowed and denied, agree with the backing filesystem; POSIX ACLs (named entries denying and granting access, default ACL inheritance and the umask) are enforced as on the backing filesystem; the daemon is back to root afterwards. |
| `crash_test` | `SIGKILL` while files are open for writing with unflushed passthrough writes: after a restart, sizes and mtimes match the backing files (this failed before writable opens marked attributes unknown). An out-of-band change is noticed on open and logged exactly once; dcfs's own mutations log no false positive. |
| `power_test` | The state a power loss leaves, produced deterministically: mutate through the mount, `SIGKILL`, undo each mutation directly on the backing filesystem, restart. Recovery logs a warning, every touched entry shows the backing filesystem's truth, untouched entries stay warm. With recovery disabled, the checks fail. A periodic sync point empties the dirty set. It cannot produce a real power loss, since a guest's page cache survives anything short of a reboot. |
| `release_leak_test` | A failed attribute refresh on the last writable close (forced by holding the SQLite write lock) does not leak the backing descriptor or passthrough registration. |
| `removed_test` | A removed working directory (`stat` reports `nlink` 0, `open(".")` works, listing it fails `ENOENT`) and an `O_PATH` descriptor on an unlinked file behave as on ext4 instead of failing `ESTALE`, also when their rows and attributes were cached; no `FORGET` exceeds dcfs's lookup count after a tree walk and dropping the kernel's caches. |
| `readdir_boundary_test` | A directory too large for one READDIR or READDIRPLUS reply lists every entry exactly once across several replies, and in time linear in its size. |
| `names_test`, `names_random_test` | File names are bytes: about 60 names, one per hazard class (format delimiters, control and high-bit bytes, invalid UTF-8, the overlong "fake slash", NFC/NFD and other look-alike sets in the spirit of xfstests generic/453 and generic/454, path-walk specials, ordering and prefixes, 255-byte names), go through create, mkdir, symlink (including a 4095-byte target; 1023 on xfs), link, xattrs with NUL-containing values, a rename chain, handles, listing and removal, both created directly on the backing filesystem (dcfs populates from it) and created through dcfs, and are compared with the backing filesystem byte for byte; after a restart the same checks pass, the handles taken before it still open and a metadata pass reads zero sectors. Errors for `.`, `..` and 256-byte names match the backing filesystem's, a directory chain deeper than `PATH_MAX` works by descriptors and handles, and a newline in a logged name cannot forge a log line. The random test makes 1,000 seeded names of random bytes (100,000 in the slow tier, `names_random_slow_test`), half through dcfs and half on the backing filesystem, and compares the trees. |
| `nfs_test` | dcfs re-exported over loopback NFSv4 from a Debian chroot: listings match, a metadata pass over NFS reads zero sectors, contents match, a file held open over NFS survives a dcfs restart (after `exportfs -f`), writes over NFS land, and a wiped database gives `ESTALE` for an old handle without touching the backing file; a refused boundary stays invisible even with `crossmnt`. |
| `pjdfstest_test` | POSIX conformance, as above. |

What is not covered: real concurrency (there is none to test until
coroutines exist). Step 5.2 built the per-filesystem suites the plan
promised: every e2e test in the table above (and pjdfstest; see
docs/conformance.md) now runs against ext4, xfs and btrfs, not just ext4.

## Known gaps

These are known and accepted for now; the README's Limitations section
lists the user-visible ones.

- **Removed objects can be read but not changed.** A removed object the
  kernel still references is answered from its `removed_` record (see
  [Row lifetime](#row-lifetime)), but `SETATTR`, xattr changes, `OPEN`
  and `LINK` of it fail with `ESTALE`, where a local filesystem allows
  them. Supporting them would need the backing operations to run through
  the held descriptor instead of a handle (`open_by_handle_at` refuses an
  unlinked inode).
- **atime is not maintained** after passthrough reads, and `st_blocks`
  may lag behind delayed allocation until the next attribute refresh.
- **A residual "ahead" window depends on the backing filesystem.** The
  dirty-set argument assumes `syncfs` really makes earlier changes durable
  on the backing device.

## Future work

None of this is built.

- **C++ coroutines over io_uring.** FUSE-over-io_uring for requests
  (libfuse's `fuse_uring.c` is already compiled in through the BCR module,
  and the test kernel has `CONFIG_FUSE_IO_URING`), io_uring for backing
  I/O (`statx`, `openat2`, `read`/`write`, `fsync`, `fallocate`, xattrs,
  `linkat`, `unlinkat`, `renameat`, `mkdirat`, `symlinkat`), possibly an
  io_uring `sqlite3_vfs` for SQLite's own I/O, and one ring plus one SQLite
  connection per worker thread. Needs a coroutine task type, `co_return`
  status macros, and waiting on in-flight mutations where today's code
  retries.
- **Kernel io_uring operations** for what io_uring lacks today:
  `getdents64`, `open_by_handle_at`, `name_to_handle_at`, and `ioctl`
  (for `FS_IOC_GETVERSION`).
- **Kernel FUSE submounts for `/dev/fuse` servers**, an INIT-time opt-in
  to `FUSE_ATTR_SUBMOUNT`, so that filesystems mounted below the source can
  be cached with their own `st_dev` (see
  [above](#how-kernel-submount-support-would-plug-in-not-built)).
- **A kernel fix for passthrough shared writable mappings,** so that
  RELEASE is not sent while a writable mapping still exists.
- **A notifier thread** for kernel cache invalidation, if out-of-band
  detection ever needs to reach the kernel.
- **ZFS**, once OpenZFS ships `FS_IOC_GETFSUUID`. A test gated on
  `DCFS_TEST_ZFS_PATH` documents today's `ENOTTY` and is meant to flip.

## The protocol model

`formal/dcfs.tla` is a TLA+ model of the write-through protocol described
above: the three phases of a mutation, fills and their guards
(`CanFill`, `Mutation::Owns`, the completeness epoch), the durable dirty set
and its fast path, sync points, crashes that keep any prefix of each disk's
unsynced writes, and `StartRun`/`RecoverDirty`/`FinishRun`. Requests
interleave at every backing syscall, as they will under coroutines. The TLC
model checker checks, within small bounds, that nothing served from the
cache disagrees with the backing filesystem (also after a crash and
recovery), that a mutation's records read unknown from phase 1 until phase 3,
that completeness never hides a name, and that recovery terminates. Variants
that put back the historical bugs (from the audits: crash F1 and F3,
tri-state F1 and F4; found by the model: the gaps below, once fixed) must
produce counterexamples. `bazel test //formal/...`
runs all of it; `formal/README.md` explains the model, what it leaves out,
and how to read a counterexample.

The model describes the code as it is. It found three gaps that only today's
single thread and the kernel's per-directory lock kept unreachable, all
fixed since (plan step R4), and each now a known-bug variant: a sync point
cleared the dirty rows of mutations still in flight (see
[Sync points](#sync-points)); Readdirplus listed after a suspension point
without checking completeness again (see Readdir under
[Population policy](#population-policy)); and Rename's phase 3 trusted a
source resolved before its phase 1 (see the rule on resolves under
[Rules that hold now](#rules-that-hold-now-so-that-coroutines-need-no-redesign)).
The configurations that found them are now part of the real model.

Trace validation checks the other direction: that the code does what the
model says. Every step of the protocol the model has (a mutation's phase 1,
its syscall and its phase 3, a fill's read and commit, an answer served
from the cache, a sync point, a crash, recovery, shutdown) is reported
through `Context::events` (`dcfs/protocol_events.h`), which production
binaries implement as a no-op; testonly builds link a recorder instead
(`dcfs/testonly/`), which writes each step as a line of the trace of every
directory it concerns, with that directory's cached state after it.
`formal/Trace.tla` takes one model step per event and requires the model's
state to match the recorded one after each; TLC must find a behavior of the
model that matches the whole trace. The traces come from the forged-request
harness (`dcfs:dir_cache_fs_trace_test`: the interleavings coroutines will
produce, such as the stale-resolve races and mutations during a sync
point's syncfs) and from guest runs of the crash, power-loss, rename and
create tests; a build whose phase 1 skips marking a name unknown is
rejected at that phase 1. A step the model does not have (a link, a rename
across directories, an out-of-band change, a syscall error it does not
know) ends that directory's trace where it happens, and only such steps
may: a step the model forbids fails validation. Validation found places
where the code is more conservative than the model (recovery forgot more
dentries than the model's, which the model now has), listed with the event
table, the projection and the action coverage in `formal/README.md`
("Trace validation").
