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

- **Syscalls that can reach the backing filesystem are made only in
  `backing.cc`** (anything given a backing fd, file handle or name), where
  the idle guarantees are reasoned about, and in the lower-level modules it
  is built from and that only it calls (`file_handle.cc`, `device_id.cc`,
  `fd.cc`); `main.cc` opens `--source` once at startup. Process-local
  syscalls (resource limits, credentials, `/proc` reads, the cache
  database file, mount tables) may call the `syscalls::` wrappers from any
  file. The point is that the io_uring rewrite replaces one module.
  The build graph enforces the split: the backing-reaching wrappers are
  `//dcfs:syscalls_backing`, the process-local ones `//dcfs:syscalls`, and
  the targets that depend on the former are a golden
  (`dcfs/syscalls_backing_users.txt`, checked by
  `//tools:syscalls_backing_users_test`).
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

**`ESTALE` from `open_by_handle_at` is not proof of absence** (step
11.3b). xfs and btrfs also return it for an inode the device cannot read,
and dcfs used to forget the row and reply `ENOENT` for a name that
exists: an unknown answered as negative. Now `OpenNode` asks the parent,
through its own descriptor, about the object's name
(`cache::NamesToAskAbout`, `FileHandle::FromDirEntry`): if its present name
is gone (`ENOENT` from a readable parent) or names another handle, the
object is gone, the row is forgotten and the reply is `ESTALE`, as before;
if it names the same handle, or the parent cannot be opened or read, the
reply is `EIO` and the row stays, its attributes unknown. An object whose
names a failed mutation left unknown has no present name, and an unknown
dentry does not say which object it named: the unknown names (at most 16)
are asked instead, and one that names the same handle, one that cannot be
checked, or more unknown names than that, is `EIO`. With no name at all
the `ESTALE` is believed. The parent is opened with `OpenNode`, which may
ask its own parent the same way: the recursion climbs the cached
ancestors one level per call, so it is bounded by the depth, and ends at
the root, which is opened through the mount fd (no handle). The checks
cost a parent open and a `name_to_handle_at` per name, on an error path
only, with no checkpoint before them: `OpenNode` also runs inside some
mutations' phase 3 (Setattr's attribute refresh), where an interruption
must not happen.

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
  recorded as a `refused` dentry and logged at ERROR, and served as a
  stub directory (below). It is never reported as absent.

### Boundary stubs

Step 23.5 (Phase 15's decision 3, pulled forward): a refused name is shown
as a stub directory, so that it appears in listings and can serve as a
mount point for another instance, while nothing behind it is cached or
served.

- **Identity.** A stub's nodeid is at or above 2^63 (`cache::IsStub`: a
  negative `InodeId`), a range `inodes.id` never reaches. Its inode
  number (`st_ino`, `d_ino`) is its nodeid, so a 32-bit program built
  without large-file support gets `EOVERFLOW` listing a directory that
  holds a stub (the compat `getdents` cannot return it; review L8). From
  Phase 14 on, nodeids are
  backing inode numbers, so dcfs already refuses any backing object whose
  inode number is in that range (`RefuseReservedIno` in `backing.cc`, at
  every probe, `ParentOf` and the source root: ERROR and `ENOTSUP`; ext4's
  inode numbers are 32-bit, xfs's below 2^56, btrfs's objectids count up
  from 256, so none is ever met). A directory containing one cannot be
  listed.
- **Record.** `cache::SetRefused` writes the dentry and its `stubs` row in
  one transaction: the next nodeid up from the highest ever handed out
  (`cache_state.last_stub_id`, advanced in the same transaction, from
  2^63; step 12.4b: a stub's nodeid is never handed out again, since a
  kernel may still hold it), a random nonzero generation (as for inode
  rows), and the boundary root's attributes from the probe's `statx`. A
  name refused again keeps its stub (nodeid and generation) and refreshes
  the attributes, also after its refusal was forgotten meanwhile (the
  kernel's revalidation of the name must find the nodeid it holds:
  another would detach a mount sitting on the stub). They are therefore
  as of the last probe (review L4): the other filesystem's root changes
  through its own mount without dcfs hearing of it, and the stub keeps the
  old values, across restarts, until the directory is listed again. A
  refusal forgotten (a mutation's phase 1, or `ForgetNegativeDentries`
  before an out-of-band relisting) makes the dentry unknown and keeps the
  stub; triggers delete the stub when the dentry is recorded present or
  absent (relisted as something else, or gone) or deleted (a relisting
  that does not find it, recovery), and the foreign key with its parent,
  so a `stubs` row exists while its dentry is `refused`, or `unknown`
  since it was. An unknown dentry is served as unknown (a lookup probes
  it again); its stub only answers requests on its nodeid meanwhile. So a
  forgotten boundary's stub can outlive the boundary until its parent is
  listed again (or the name looked up): bounded, and deliberate, since it
  keeps a mount sitting on the stub attached. The attributes have no unknown state of their own: dcfs never
  changes them (every change to a stub is refused), so the dentry's
  present/absent/unknown/refused state is the record the tri-state rule
  applies to. A listing or probe that could not record its result (a
  concurrent mutation of the directory, possible only under coroutines)
  has no stub to serve and answers `EAGAIN`.
- **Serving.** `ListDir` merges present names and stubs in rowid order
  (each half a range scan of its own partial index). `GETATTR`, `LOOKUP`
  of the stub and of its `.` and `..`, `STATFS` (the source's), `GETXATTR`
  (`ENODATA`: no ACLs, so its mode decides permission checks),
  `LISTXATTR` (empty), `ACCESS` and `FORGET` are answered from the stub's
  row. Everything else -- any lookup, listing, open or creation inside it,
  and `SETATTR` or xattr changes of it -- is refused with `ENOTSUP`
  (`DirCacheFS::RefuseStub`, logged at ERROR once per stub per run, naming
  the errno), `UNLINK`/`RMDIR` of it with `EBUSY` (as for a mount point),
  an ioctl with `ENOTTY`, and a `RENAME` or `LINK` across it, or of the
  stub itself, with `EXDEV`. A stub whose row is gone (its dentry was
  recorded present or absent, or deleted) is a stale nodeid: `ESTALE`, so the kernel looks the
  name up again. The kernel looks a link's or rename's target name up
  before sending the request, so a link or rename *into* a stub fails at
  that lookup, with `ENOTSUP`.
- **Not done here** (Phase 15.4): the bind form's recorded mount points,
  reverting stubs that are no longer boundaries without a relisting, and
  non-directory boundaries (file mount points).

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

`dcfs/schema.sql` is the full definition, schema version 4. All tables are
`STRICT`. `Migrate()` (`dcfs/migrate.cc`) creates a fresh database in one
transaction, or upgrades an older one (one version step at a time) to
version 4 in one transaction, and refuses versions it does not understand.
The step to version 4 adds the `stubs` table; a version 3 cache's refused
dentries, which have no stub, become unknown and are probed again.

### `cache_state`: one row of cache-wide state

| Column | Meaning |
|---|---|
| `id` | Always 1. |
| `schema_version` | The schema version (`kSchemaVersion` in `dcfs/migrate.h`). |
| `source_device_id` | `DeviceId::Serialize()` of the source filesystem, written once at creation. |
| `clean_shutdown` | 1 after a clean shutdown (backing synced, dirty set empty, WAL checkpointed); set to 0, durably, at every start. 0 at the next start means the last run crashed. |
| `boot_id` | `/proc/sys/kernel/random/boot_id` at the last start, so that recovery can log whether the machine rebooted (a kernel crash or power loss) or only the daemon died. |
| `last_stub_id` | The highest boundary stub nodeid ever handed out (NULL before the first): the next stub's is the one above, so none is handed out twice ([Boundary stubs](#boundary-stubs)). |

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
  that dcfs will not cache behind. Never reported absent; served as its
  stub directory (its `stubs` row; see
  [Boundary stubs](#boundary-stubs)).

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

### `stubs`

| Column | Meaning |
|---|---|
| `id` | The stub's nodeid: at or above 2^63, so negative as a SQLite integer. |
| `parent`, `name` | The `refused` dentry it stands for (unique). Deleting the parent cascades. |
| `fuse_gen` | Random, nonzero. |
| `mode`, ..., `btime_*` | The boundary root's attributes, from the last probe. |

A row exists while its dentry is `refused`, or `unknown` since it was
(`cache::SetRefused` and the triggers `dentries_unrefused` and
`dentries_refused_deleted`); `cache_state.last_stub_id` is the highest id
ever handed out. See
[Boundary stubs](#boundary-stubs).

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

`atime_only` (step 23.8, schema v6): 1 for a row that stands only for a
regular file's access time, which the backing filesystem changes on its own
reads (see [Access times](#access-times)); a mutation's phase 1 sets it to
0. Recovery treats both alike; only rows with 0 make a sync point run.

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
id), `name_to_handle_at`, the generation, the symlink target and every
xattr, all through that one descriptor. It pins the inode, so its number
cannot be freed and recycled between the reads, and the statx, the handle
and the generation describe one object whatever their order; read by name
instead, a recycling after the statx would go unnoticed in either order
without a birth time (`formal/README.md`, "The order of the handle and the
generation"). All of this I/O happens first. Then one transaction upserts every
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
`readdirplus_unlocked`). The listing statement is one join of the
dentries with the children's inode rows (`cache::ListDir`), so a child whose
attributes are valid arrives with them (plain READDIR needs only the inode
number and type; READDIRPLUS the whole entry, through the same
`FreshAttr` as any other entry), and only a child with unknown attributes
(or a stub) is read, and refreshed, per entry afterwards. The query reads
only as many rows as the reply can hold at its smallest entry (about 26 for
a 4 KiB READDIRPLUS reply), not the cache's usual 64, and goes on a batch at
a time if more fit. Attributes are therefore read at listing time: under
cancellation or coroutines (Phase 22), a mutation that interleaves during
an earlier entry's refresh does not make a later entry's attributes unknown
before they are served. That is safe: the kernel's `attr_version` discards
stale READDIRPLUS attributes, and today dcfs is single-threaded. The cost
is counted, not timed: `ReaddirWorkTest` (`dir_cache_fs_test.cc`) bounds the
SQLite steps of a warm listing of 100 and of 1000 entries per entry
(about 1.1 to 1.3 steps each, plain or plus), and `readdir_boundary_test`
compares the daemon's CPU ticks listing 6000 entries with 1500 (a linear
listing is about 4x, a quadratic one 10x or more; it fails above 8x).

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
| setattr, writable open, fallback write, fallocate, copy_file_range (its destination), an ioctl that sets flags | attributes unknown; side-effect xattrs unknown | the inode |
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

The one exception is a create (create, mknod, mkdir, symlink) whose new
row cannot be recorded (or, for a create, whose open cannot be set up),
for instance because the cache database's disk is full: its reply must
name the object by a row. If its probe found the object, it is replied as
`EEXIST`, which is true now (`CreatedButNotCompleted`, step 11.4); if the
probe itself failed (the name gone again, `ENOENT`, or a failing backing
filesystem, `EIO`), that error is replied. Any other error left the kernel
with the negative entry of the `LOOKUP` it made before the create, which
answered `ENOENT` for the new file for `--entry_timeout_sec`
(`enospc_cache_test` showed it); on `EEXIST` the kernel drops it
(`fuse_invalidate_entry`, `fs/fuse/dir.c`, Linux 6.6), so the next lookup
asks dcfs, which resolves the name (unknown since phase 1). A caller that
retries on `EEXIST` with another name (`mkstemp`) can leave an empty
object per retry on the backing filesystem while only phase 3 fails (the
README's limitation); dcfs does not fail later creates in phase 1 for it,
since a cache disk that cannot record phase 3 rarely lets the next
phase 1's durable commit through, and that ends the loop with `ENOSPC`. A create whose
row was recorded but whose attributes cannot be refreshed is replied as
done, with the row's last attributes and a zero attribute timeout
(`EntryAfterPhase2`), as a link is.

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
`.` lookup that resolves a handle uses the cache rows only.

**Changing a removed object** (step 23.2) works as on the backing
filesystem: `SETATTR` (truncate, chmod, chown, utimes), `SETXATTR`,
`REMOVEXATTR` and `FSYNCDIR` are applied through the record's descriptor
(`backing::SetAttrFd`, `SetXattrFd`, `RemoveXattrFd`, `FsyncDirFd`: the
same calls and credential rules as for an object with a row, reaching it
through `/proc/self/fd` instead of a handle), and `OPEN` (an open of
`/proc/<pid>/fd/<n>` of an `O_PATH` descriptor on an unlinked file)
reopens it through the descriptor and registers it for passthrough like
any open (`MakeBackingFile`); its writes, `FSYNC` and `RELEASE` work as
for any open file, minus the cache bookkeeping. None of these has a phase
1 or 3, and the record needs no tri-state of its own, because the record
caches nothing that can change: every read of the object (attributes,
xattrs, a symlink's target) goes to the descriptor (a `statx` or
`getxattr` of an inode the kernel already holds in memory, no disk
access), so a change leaves nothing stale behind, and a crash loses only
the record, which is in memory and which a new mount never needs. (The
protocol model is unaffected: it has no child objects' attributes.)
`LINK` of a removed object links the descriptor its record holds
(`linkat` with `AT_EMPTY_PATH`, step 23.9: `DirCacheFS::LinkRemoved`), so
the answer is the backing filesystem's own: `ENOENT` for a file with no link
left (`vfs_link` refuses one unless `O_TMPFILE` made it linkable, and an
unnamed `O_TMPFILE` file is no removed object: it has a row, step 23.4),
`EPERM` for a directory. Its phase 1 is a create's (the name and the new
parent's attributes unknown, the parent dirty: the object has no row a
link could be recorded against), and phase 3 resolves the name again.

An unlinked file that dcfs itself still has open keeps its row until the
last release (above); changing it goes the ordinary way, by handle:
`open_by_handle_at` does reach an unlinked inode while something holds it
(the backing filesystem finds it in its inode cache; checked on ext4, xfs
and btrfs by `removed_test`'s `unlinked-open-mutate`).

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
verified when next opened. The dirty set is kept, and a WARNING reports
the count and whether `boot_id` changed. Then, once the mount fds
exist (`backing::Startup`, which main.cc and the tests call: `StartRun`,
`InitRoot`, `StartupPurge`, then the probe), every inode that was in the
set (except the root) is probed by handle (`ProbeRecoveredRows`, step
12.4b): a crash between a removal's backing syscall and its phase 3
leaves the removed object's row, which no name leads to any more (recovery
made them unknown) and whose recorded link count is not 0; if the object is
gone (`ESTALE`) or has no link left, the row goes, a directory's too. One
that still exists keeps its row, its attributes unknown for the next access
to read. The probe is bounded by the dirty set and costs one
`open_by_handle_at` and `statx` per dirty inode: after a daemon crash (same
boot) the inodes are in the backing filesystem's cache, about 10-20 µs each;
after a power loss each is an inode-table read, which on a spinning disk
can take seconds for thousands of dirty inodes. Start-up after a crash is
slower by that much (`BM_Recovery`'s numbers move with it). The probed
inodes stay in the dirty set, as every recovered one does: after a daemon
crash the crashed run's backing changes may be in the page cache only, and
a power loss before the next `syncfs` could still undo them, so only the
first sync point (`syncfs`, then `ClearDirty`; `ctx.dirty.any` makes one
run) takes them out. Recovery may therefore crash and start again any
number of times: a crash during the probe leaves the inodes not yet probed
to the next start, which recovers them again (the second pass finds them
already unknown) and probes them (step 12.6b; before it the dirty set was
emptied with recovery, and such a crash lost them:
`formal/known_bugs/lifetime_probe_list_in_memory`).

So a power loss costs re-reading the entries mutated since the last sync
point, and never serves state the backing filesystem did not keep.

**A crashed backing filesystem** (step 11.5: `FS_IOC_SHUTDOWN`, xfstests'
`godown`, or a disk that fails every I/O): dcfs keeps serving what it has
cached (the backing filesystem's durable state, or a mutation it reported
done, whose dirty row stays); every operation that reaches the backing
filesystem fails with its error (`EIO` after a shutdown): mutations,
including one already past phase 1 whose syscall fails (its names stay
unknown), a create whose syscall succeeded but whose probe of the new
object fails (it replies the probe's error), opens of file contents, and
fills. The clean shutdown's sync point fails, so the
clean-shutdown flag stays 0 and the dirty set survives. A start over the
crashed filesystem without a remount fails on xfs (the open of `--source`
fails) and over a filesystem that went read-only by itself (below); after
an ext4 shutdown it succeeds, serves nothing new (a create through it
fails: the filesystem refuses it), and what it re-reads of a dirty
directory stays dirty (recovery keeps the rows until a sync point
succeeds, and every `syncfs` fails). Once the backing
filesystem is mounted again, the next start recovers and everything served
matches the backing filesystem (`fault_shutdown_test`, ext4, xfs and btrfs,
each flavour of the ioctl; on the pre-12.6 code its "recrash" scenario, a
daemon crash and a re-read before a `nologflush` shutdown, served a name the
backing filesystem had lost).

**A filesystem that went read-only by itself** after an error (btrfs's
transaction abort, ext4's `errors=remount-ro`, which Linux 6.15 marks
`emergency_ro`) still shows changes its disk never got. btrfs, and ext4
before 6.15, answer `syncfs` with success: `sync_filesystem` returns at
once for a read-only superblock, and the write error reaches only the
first `syncfs` of each open file (ext4 from 6.15 keeps failing it, as
`fault_shutdown_test_ext4` shows). So a sync point after the first one cleared the dirty set of
those changes and the clean shutdown recorded a clean run; after the
operator's remount took the changes back, the next start had nothing to
recover and served the lost objects as present (step 11.5's review;
`fault_shutdown_test_btrfs`'s "ro" showed a lost directory served). Two
guards (step 11.5): a sync point fails, keeping the dirty set, when
`fstatvfs` reports the source read-only and it was writable when the run
started or at any sync point since (`StillWritable` in
`backing::SyncBacking`; a source read-only at the start is exempt only until
a sync point finds it writable, step 11.5b); and dcfs refuses to
start over a source whose superblock is read-only (or `emergency_ro`) under
a read-write mount (`ForcedReadOnly`, `dcfs/mounts_below.h`), since its
memory may show recovery and the fills what a remount takes back. A source
mounted read-only on purpose is read-only in both, and is accepted. One
case looks the same and is not an error: `mount -o remount,ro` of one
mount of a superblock makes the superblock read-only while its other
mounts (bind mounts, btrfs subvolumes) stay read-write. dcfs over such
another mount refuses to start, and a running one fails every sync point
(the dirty set stays, the run ends unclean): remount it read-write again,
or mount `--source` read-only (step 11.5b).

### Sync points

`backing::SyncBacking`: `syncfs(2)` on every mount fd, then, if all
succeeded, empty the dirty set in one transaction, except inodes with an
open outstanding (the kernel may still be writing to them, or reading them,
through passthrough) and rows the `syncfs` may not cover. On a `syncfs`
failure nothing is cleared.

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

Reads count the same way (step 23.8): the backing filesystem stamps a
read's access time in memory and writes it back lazily, and dcfs records it
only at the next held fill ([Access times](#access-times)). So a row also
stays if its inode was open at all at the snapshot
(`SyncSnapshot::open_files`) or at the clear (`Context::open_files`), and
every record of an open file's attributes touches the inode
(`cache::MarkAtimeDirty`), so that a row recorded after the snapshot stays
too. A file's row is therefore dirty from its cold open until the first
sync point after its last release.

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
  after the previous sync point, while the dirty set may hold a row that
  is not `atime_only` (`Context::dirty.any`; `DirCacheFS::MaybeSyncBacking`,
  called from `fuse_ops.cc`);
- at clean shutdown (`backing::FinishRun`), which is clean only if no row
  of either kind is left.

An `atime_only` row does not make a sync point run on its own, after an
`FSYNC` either, until it is older than the kernel's dirtytime expiry: the
sync point's `syncfs` would force the backing filesystem's lazy write-back
of access times, which an operator who mounts it `lazytime` deferred on
purpose (the kernel writes them back after
`/proc/sys/vm/dirtytime_expire_seconds`, 12 hours by default, and dcfs
lets them drive a sync point from then on), and a spin-up for an access
time is what dcfs exists to avoid. Such rows are cleared by whichever sync
point runs (for a mutation's row, an `FSYNC` with one, the timer with one,
or their expiry), by the clean shutdown's, or, after a crash, recovered
(their attributes only: [Access times](#access-times)), staying dirty
across the restart until then.

**The clock.** dcfs reads the time only through `Context::clock`, an
`absl::Clock`: the real clock in production, an `absl::SimulatedClock` in
the tests (step 26.10). There are two readers: the periodic sync point
(`DirCacheFS::MaybeSyncBacking`, `--sync_interval_sec`) and the relatime
stamp of a directory's or symlink's access time
([Access times](#access-times)). `absl::Now`, `clock_gettime`, `time` and
`gettimeofday` are banned from the shipped binary (`tools/banned_symbols.txt`)
and from the sources (`//tools:raw_syscalls_test`). The kernel's own clock is
not ours: the times a backing filesystem stamps (including `UTIME_NOW` in
`backing.cc`'s `utimensat`/`futimens` calls) stay the kernel's.

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

### A backing filesystem that is frozen or stalled

One thread means one request at a time, so a request blocked in a backing
syscall blocks the daemon, and with it every request behind it, a read of
cached state included. Step 11.6 measured it with FIFREEZE
(`guest/fault_freeze.sh`, `//test/qemu:fault_freeze_test`, ext4, xfs and
btrfs alike):

- **Nothing held:** a frozen filesystem refuses writes only. A request dcfs
  answers from its cache or by reading the backing filesystem (stat, readdir
  of a filled directory, a lookup of a present or absent name, the open of a
  file for reading or writing, a passthrough read) is answered. A write
  through passthrough blocks its client in the kernel (the daemon is not in
  it), and the daemon serves what comes meanwhile.
- **A mutation held:** create, mkdir, unlink, rename, chmod, setxattr and
  truncate each block in their phase 2 syscall; the daemon is in state D in
  it. Phase 1 has committed: the database shows the names (or the inode's
  attributes) unknown and the dirty set holds the objects, which the checking
  build's hook before the syscall verified. A request for a name the kernel
  has not cached (a lookup the daemon would answer from its cache) waits
  behind the held one, and nothing is served until the thaw; then the mutation
  completes (phase 3 records the outcome: no name is left unknown) and what is
  served equals the backing filesystem. A power cut while a rename or an
  unlink is held leaves the old names on the disk and the dirty rows in the
  cache: the next start recovers them (`fault_power_kill_test`, `frozen_unlink`
  and `frozen_rename`).
- **A sync point** during a freeze does not block: `syncfs(2)` of a frozen
  filesystem returns at once (everything was flushed by the freeze), and the
  dirty set is cleared. **Shutdown** by `SIGTERM` with nothing held completes
  within a moment, the last sync point included, and is recorded as clean.
  With a mutation held the signal waits behind it; after the thaw the mutation
  completes, the loop ends before the client's `RELEASE` of the created file,
  so the clean-shutdown flag is left unset ("a writable open is still
  outstanding") and the next start recovers the dirty entry, which is correct
  and only costs a re-read.

For the cancellation phase (22): a checkpoint is before a backing syscall, so
a request held inside one cannot be interrupted, and with one thread the whole
daemon is held with it. Only running requests on more threads (coroutines
with a thread per queue) would let the rest of the filesystem go on, and then
only for requests that touch no inode the held one has unknown.

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

## Cancellation

**Goal** (Phase 22, russ 2026-10-06): a process interrupted while it waits
on dcfs (Ctrl+C, a signal, a timeout) gets its answer promptly, and the
slow work dcfs was doing for it stops. The kernel sends `FUSE_INTERRUPT`
for a request whose caller got a signal, but keeps waiting for the
request's reply, even for a SIGKILLed caller (`request_wait_answer` in
`fs/fuse/dev.c`: once dcfs has read a request, only its reply ends the
wait). So a request dcfs stops must still be replied: `EINTR`.

**Checkpoints, not threads** (russ approved option (b), 2026-10-07). dcfs
serves one request at a time, so an interrupt cannot stop a backing
syscall already issued; dcfs looks for one at checkpoints
(`Checkpoint`, `dcfs/checkpoint.h`), each just before a backing syscall:

- before `LookupOrPopulate`'s probe of a name or population of a
  directory, and every 16 probes inside `PopulateDirectory` (the slow
  case: one request of 14-20 s for 20,000 entries at 10 ms per I/O,
  `cancel_inventory_test`);
- before every mutation's phase-2 syscall (create, unlink, rename, link,
  setattr, the xattr changes, a fallback write, fallocate,
  copy_file_range, ioctl);
- before a cold open's open by handle, and before `FSYNC`'s and
  `FSYNCDIR`'s fsync and their sync point.

How a checkpoint learns of the interrupt: the kernel queues a request's
`FUSE_INTERRUPT` only after dcfs read the request, and libfuse's own loop
reads the next message only once the handler has returned, so a request
never saw its own interrupt. `SessionLoop` (`dcfs/session_loop.h`, the
daemon's loop) drains `/dev/fuse` without blocking at each checkpoint (one
`poll`): an interrupt goes to libfuse at once (`do_interrupt` marks the
request, `fuse_req_interrupted` reports it), and the drain stops at the
first other message, which is queued and served after the current request:
at most one request is read ahead per checkpoint, so a long request does
not pull the kernel's whole queue into dcfs's memory. The kernel delivers
interrupts ahead of queued requests (`fuse_dev_do_read`), so the drain
finds an interrupt before any request. dcfs turns `FUSE_CAP_SPLICE_READ`
off, so every message arrives in memory and a drained one is never a pipe
that libfuse would have to read during the current request. No thread, and
nothing runs while dcfs is idle. In `cancel_test` (dm-delay, 10 ms per
I/O) an interrupted `ls` of an unlisted 20,000-entry directory returns
0.7-0.9 s after the signal (12.1 s without checkpoints), and a SIGKILLed
`find` of another one 0.7-1.0 s after the kill (under TCG: 1.2 s and
0.8 s).

Owning the loop also means owning its end. libfuse refuses an `INIT` it
cannot accept (a `max_read` the kernel did not offer, an old protocol
version) by ending the session, and keeps the error in a private field;
its own loop returns it, and `SessionLoop::Run` returns `-EPROTO` when the
message it just processed was `INIT` and the session has ended, so the
daemon exits non-zero instead of reporting a clean unmount.

**The tri-state rule holds through it** (`formal/dcfs.tla`'s `Interrupt`,
with `GuardsBalanced`; `formal/README.md`): an interrupted fill commits
nothing (a population's probes so far are dropped, the directory stays
incomplete); a mutation interrupted after its phase 1 and before its
syscall ends (`Mutation::End`, releasing its guard) with its names and
attributes still unknown and dirty, which is sound because the backing
filesystem is unchanged; a mutation has no checkpoint after its syscall,
so it is never interrupted between its syscall and its phase 3: the change
exists, phase 3 records it, and the request replies success. A failed
mutation's re-resolution of its names (`ReresolveAfterFailure`) is a fill
with checkpoints of its own: its first `EINTR` ends the request with
`EINTR` (the names stay unknown), and a rename skips its second name. An
interrupted `FSYNC` may have synced (its sync point has a checkpoint after
the backing `fsync`); repeating it is harmless. The model's variants show what breaks otherwise (an
interrupt after the syscall that puts the old name back: `CacheNeverWrong`;
the same before the syscall without the kernel's lock: `TriState`; an
interrupted mutation that never Ends: `GuardsBalanced`).

**What cannot be interrupted yet**: a backing syscall already blocked in
the kernel: the first I/O to a disk spinning up (seconds), a hung network
mount, `syncfs` or `fsync` of much dirty data, DESTROY's reconciliation
and start-up recovery (no caller). The request answers at its next
checkpoint after the syscall returns. The coroutine and io_uring rewrite
lifts this: each request carries a cancellation token checked at every
await (the same checkpoints), and `IORING_OP_ASYNC_CANCEL` cancels
in-flight backing I/O that io_uring can cancel; the rules above carry over
unchanged.

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

### Access times

(Step 23.8; the decision and the alternatives considered are in
`docs/plan/notes/atime-alternatives-2026-10-08.md`.)

**Regular files: from the held descriptor.** Reads go through
passthrough, so dcfs never sees them, and the backing filesystem stamps the
access time itself, by its mount's rule and the file's own flags
(`FS_NOATIME_FL`; the kernel opens a passthrough open's backing file with
the open's own flags, so `O_NOATIME` holds too). dcfs predicts nothing. While
a file is open (dcfs holds the shared backing descriptor of its passthrough
opens; the `O_PATH` one a written file keeps until its last `FORGET`, see
below, is no reason: no read can move its access time), the file's
attributes come from a `statx` of that descriptor, a *held fill* (`backing::FillHeldAttrs`): at every `FLUSH`, at
every `RELEASE`, and for every attribute reply while it is held (`GETATTR`,
`LOOKUP`: `DirCacheFS::FreshAttr`), unless a writable open is outstanding
(then the attributes are kept unknown as above, and served from the same
descriptor). It costs one `statx` per `FLUSH`, per `RELEASE` and per held
attribute reply, and no transaction when nothing changed since the last
record (the common case: a stat of an open file nobody read since). It is
always current, and needs no disk access:

- an open file pins its dentry and inode (reference counts, not an LRU), so
  the inode cannot be evicted under memory pressure, and ext4, xfs and btrfs
  answer `getattr` from the in-core inode, where `touch_atime` put the read's
  access time;
- every access-time update of a regular file goes through an open file
  description (the read family, `mmap` at map time, `splice`, `sendfile`,
  `copy_file_range`, `execve`, nfsd, io_uring), so there is always a held
  descriptor to ask, and after the last `RELEASE` no read can come until the
  next open: the release's held fill is the truth until then;
- FUSE passthrough reads and mmaps call `fuse_invalidate_atime`, so the
  kernel drops its cached access time and the next `stat` reaches dcfs.

The protocol around it, for the crash cases (the model's "Access times of a
file", `formal/dcfs.tla`):

- The backing filesystem writes the access time back lazily, and only a
  `syncfs` makes it durable. So every record of the attributes of a file
  dcfs holds open (`Context::open_files`), by the held fill or by any other
  path (a population's probe, a mutation's refresh: `cache::UpdateAttr` and
  `UpsertInode` call `cache::MarkAtimeDirty`), marks its row dirty with the
  reason `atime_only` and touches the inode's fill guard in the same
  transaction. A power loss that keeps the record and loses the backing
  filesystem's write-back then finds the row dirty, and recovery forgets
  it: the cache is never ahead.
- A cold read-only open marks the row dirty (`atime_only`, one write
  transaction, no `fsync`, only if it is not dirty already: a read first)
  before the reply, from which on the kernel may read; sync points keep
  the row while the file is open (one whose `syncfs` covered everything
  else the row stood for, a mutation of the open file, keeps it as
  `atime_only`, so that it drives no further sync point)
  ([Sync points](#sync-points)). A daemon crash while the file is open and
  has been read, before any held fill, then finds the row dirty too, and the
  restart reads the attributes again: never behind either. The residue: a
  power loss that loses the open's commit (normal durability: the WAL may
  not have reached the disk) while the backing filesystem's journal kept a
  read's access time leaves the old access time, until the file is next
  opened and closed (`formal/limitations/`, `atime_power_loss_while_open`;
  README "Limitations"). A `fsync` per open would close it, at the cost of
  a cache-disk flush per cold open.
- A held fill whose guard fails (a mutation of the file began or ended
  since its snapshot, or is in flight) records nothing and marks the
  attributes unknown; it touches the inode even with a mutation in flight,
  so that the mutation's phase 3 does not record attributes read before the
  reads the fill saw. A failed `statx` likewise leaves them unknown and the
  row dirty.
- An `atime_only` row does not make a sync point run (see there) until it
  is older than the kernel's dirtytime expiry
  (`/proc/sys/vm/dirtytime_expire_seconds`, read at startup, 12 hours by
  default: `Context::dirty.atime_since`, `atime_expiry`), by which time the
  kernel writes access times back anyway.
- Recovery after a crash makes an `atime_only` row's attributes unknown and
  nothing else: its names, xattrs, symlink target and the listing it is in
  stay, and the start does not probe it (`ListDirty(mutations_only)`). A
  read-mostly workload thus costs, after a crash, one `statx` per file read
  since the last sync point, when it is next looked at.
- At `DESTROY` with files still open for reading (a `SIGTERM`, a lazy
  unmount), the kernel sends no more requests: dcfs makes a last held fill
  of each and stops counting them as open, so that the shutdown's sync
  point clears their rows and the shutdown is clean (the lifetime model's
  `DestroyWithOpens`). Passthrough reads go on without the daemon, so a
  read the kernel still makes after that is not seen, as a write through a
  mapping after the unmount is not: after the restart dcfs may serve that
  file's access time from before it, until its next open and close (README
  "Limitations"). Not modelled (`dcfs.tla`'s `BeginShutdown` waits for the
  release).

**Directories and symlinks: stamped in the cache only.** A listing and a
symlink's target are served from the cache, so the backing filesystem's
access time genuinely does not move (except at dcfs's own population, which
reads the directory). dcfs applies the backing mount's rule
(`Context::atime`, from its `statvfs` flags at startup: relatime, the
default: if the cached access time is not after the modification or change
time, or is a day old; strictatime: always; noatime: never) at every
`READDIR`, `READDIRPLUS` and `READLINK` it serves, with the injected clock,
and records the result in the database only (`cache::TouchAtime`: a read,
and a write transaction only when the time changes). Every later record of
the row's attributes keeps the cached access time when it is the later one,
so a refresh from the backing filesystem does not take it back; an explicit
access time (`SETATTR` with `ATIME` or `ATIME_NOW`) drops the stamp first
(`cache::DropAtimeStamp`), so the backing filesystem's value wins, earlier
or not. The stamp survives restarts and is lost with the cache. It is never
written to the backing filesystem: `utimensat` would bump the change time,
which a real read never does (alternative 4 of the note). The fault tests'
"served equals backing" comparisons therefore leave directories' and
symlinks' access times out, and only those (`guest/lib.sh`'s `snapshot
DIR atime`).

### mmap after close (held-fd workaround)

This section is the reason for a workaround and the condition for removing
it. The code marks each of its three parts with the comment
`// See design.md "mmap after close" (held-fd workaround)`: the written-file
set (`DirCacheFS::written_`), the descriptor held for each file in it
(taken in `DirCacheFS::Release`), and the FORGET hook
(`DirCacheFS::ReconcileWritten`, called from `Forget`, `ForgetMulti` and
`Destroy`). The kernel change that makes it unnecessary is listed in
`docs/plan/README.md` ("Future work").

**Why.** `mmap` of a passthrough file swaps the mapping's file to the
backing file (`backing_file_mmap`, `vma_set_file`), so the mapping keeps
only the backing file, not the dcfs (FUSE) file. `close()` after
`mmap(MAP_SHARED, PROT_WRITE)` therefore drops the last reference to the
dcfs file, and the kernel sends RELEASE before `munmap`, while the mapping
can still store. The last writable release records the file's attributes
as current; stores through the mapping after it reach the backing file
with no FUSE request at all and change its mtime, ctime and blocks
(amendment 16). The mapping's backing file does hold the dcfs file's
*path* (`backing_file_open` takes a reference on its `user_path`), so the
kernel cannot forget the dcfs inode while the mapping exists: its last
`FORGET` comes after the last store, and it is the only later event dcfs
sees. Hence the reconciliation there (steps 23.1 and 23.6): every inode
that had a writable open during this run is in `written_`, and at its last
`FORGET` (and at `DESTROY`, since the kernel sends no `FORGET`s at
unmount) dcfs re-reads its attributes. If they match the cache nothing
happens. If they differ, or are unknown, they are recorded as a mutation
records them: a phase 1 (`cache::BeginAttrChange`: unknown, durably
dirty), then a refresh as a fill, so that a power loss before the next
sync point cannot keep the new attributes and lose the stores. If the
backing object has no link left (removed behind dcfs's back since), the
row goes.

**Why a held descriptor.** At the file's last close (the release that
tears down its shared backing descriptor), dcfs reopens it `O_PATH` and
keeps that until the last `FORGET`. The reconciliation's `statx` goes
through it, and the kernel answers from the pinned inode: no disk read,
so a backing disk that has spun down stays asleep (`idle_short_test` checks it:
the `FORGET` of a written file whose backing inode the caches had dropped;
step 23.1's `statx` by handle read the inode back from the disk there).
`O_PATH` is only a reference: no open file, no writer, no lease. Costs and
limits:

- One descriptor per written file the kernel still caches, as many as its
  inode cache keeps (the same bound as `lookups_`), up to a cap
  (`Options::max_held_fds`, review M-1). `main.cc` raises `RLIMIT_NOFILE`
  to `fs.nr_open` at startup, and the cap defaults to what the soft limit
  leaves after a reserve of half the limit, at least 16Ki and at most 64Ki
  descriptors: 983,040 at the default `fs.nr_open` of 1,048,576, 32,768 at
  a container's 65,536, and none at 16Ki or less (the usual 1,024, when
  the limit cannot be raised without `CAP_SYS_RESOURCE`). `main.cc` warns
  when the raise to `fs.nr_open` fails, and dcfs warns at startup when the
  cap is 0, since the workaround is then off. The reserve is for what requests open (shared
  backing descriptors, removed objects' holds, SQLite's files): without a
  cap, enough cached written files would make every one of them fail with
  `EMFILE`. A file written beyond the cap holds none, and neither does one
  whose descriptor cannot be opened (`EMFILE`): its reconciliation is the
  phase 1 alone (its attributes unknown, no I/O), and the next access
  re-reads them. A `FORGET` gives its descriptor's place back.
- A file whose last link dcfs removes leaves the set at once
  (`RetireRemoved`) and its held descriptor is closed; if the kernel still
  holds the nodeid, the removed record's descriptor keeps the space
  allocated until the last `FORGET`, as the kernel's own reference would
  on a local filesystem. One unlinked behind dcfs's back stays allocated
  until its `FORGET` too.
- The files one `FORGET` batch (`FORGET_MULTI`) lets go of, and those
  `DESTROY` reconciles in batches of 4,096, share one phase 1 for every
  file that needs one (changed attributes, or no held descriptor): one
  durable transaction per batch, not one per file.
- `DESTROY` reconciles every file left in the set: one `statx` through a
  held descriptor each, no disk. Measured (`destroy_test`, review M2):
  with 100,000 written files cached, SIGTERM to exit took 4.8 s, the
  final sync point and checkpoint included, and dcfs held 100,010
  descriptors; the test bounds it at 60 s. It only delays the daemon's
  exit, never `umount(2)`.

**What is still not covered.**

- Between `munmap` and the last `FORGET` (the kernel keeps an inode it is
  not short of memory for as long as it likes), dcfs serves the attributes
  the release recorded, and NFS clients, which detect changes from ctime,
  may serve stale data. A daemon crash or power loss in that window leaves
  those attributes recorded as current.
- A mapping still live at unmount. As root, libfuse's
  `fuse_kern_unmount` closes `/dev/fuse` (which aborts the connection) and
  then unmounts lazily, so a process's passthrough mapping keeps storing to
  the backing file after dcfs has stopped. `DESTROY` reconciles what was
  stored by then, `FinishRun` records a clean shutdown, and the next run
  serves the attributes as of `DESTROY` with nothing to tell it otherwise.
  (`DESTROY` also cannot wait for the mapping: the kernel sends it only for
  fuseblk mounts, and libfuse calls it at session teardown.)

**When to remove it.** The fix belongs in the kernel: a passthrough
mapping should keep the FUSE file referenced, so that RELEASE follows
`munmap` and the last writable release means no more writers. Once that
lands and dcfs's minimum kernel requires it, delete the written-file set,
the held descriptors and the FORGET hook (the three marked places, and
the `RLIMIT_NOFILE` raise in `main.cc`), and let the existing
last-writable-release path cover mmap.

### copy_file_range, reflinks, ioctls and O_TMPFILE

Step 23.4.

- **`copy_file_range`** (`DirCacheFS::CopyFileRange`, `FUSE_COPY_FILE_RANGE`)
  is the backing filesystem's own `copy_file_range(2)` on the two files'
  shared backing descriptors, so btrfs and xfs share the extents (a
  reflink) and ext4 copies in the kernel. The destination's bookkeeping is
  a fallback write's: phase 1 (attributes and `security.capability`
  unknown, durably dirty), the copy, then refreshes as fills. An error is
  replied as the backing filesystem gives it; for `EOPNOTSUPP` and `EXDEV`
  the kernel falls back to copying the data itself.
- **Reflinks** (`FICLONE`, `FICLONERANGE`, `FIDEDUPERANGE`) never reach
  dcfs: `do_vfs_ioctl` handles them through `remap_file_range`, which FUSE
  does not have, so they fail with `EOPNOTSUPP` whatever the backing
  filesystem (verified by `copy_test` on all three). Doing them would need
  that kernel operation; the ioctl path cannot carry them (its argument,
  the source descriptor, is a number in the caller's process).
- **Ioctls** (`DirCacheFS::Ioctl`): an allowlist is forwarded to a real
  descriptor on the backing object (its shared backing descriptor if open,
  else one by handle): `FS_IOC_GETFLAGS`/`SETFLAGS` and
  `FS_IOC_FSGETXATTR`/`FSSETXATTR`, which the VFS's fileattr calls
  (`chattr`, `lsattr`) send as `FUSE_IOCTL` on a private open after
  checking the caller's right to change the flags, and
  `FS_IOC_GETVERSION`. Their arguments are plain buffers. A set is a
  mutation of the object's attributes (its ctime changes): phase 1, the
  ioctl, a refresh. A `SETFLAGS` that changes `FS_CASEFOLD_FL`
  (`chattr +F`/`-F`) is refused with `EOPNOTSUPP` (review M1): on an ext4
  with the casefold feature it would make a directory case-insensitive,
  which dcfs's cache of byte names cannot follow (Phase 16 refuses such
  directories); `FSSETXATTR` has no casefold bit. Everything else, and
  anything from a 32-bit caller, is `ENOTTY`. A kernel gap: after a
  successful set, neither `fuse_fileattr_set` (`fs/fuse/ioctl.c`) nor
  `vfs_fileattr_set` invalidates the FUSE inode's cached attributes, so
  `stat` serves the ctime of the `GETATTR` before the `chattr` until the
  attribute timeout, although dcfs's cache has the new one (the refresh
  above). `copy_test`'s `DISABLED_immutable-ctime` shows it on every run;
  dcfs could push the change with `fuse_lowlevel_notify_inval_inode`, not
  done (the maintainer's call). dcfs requests `FUSE_CAP_IOCTL_DIR` for directories.
- **Writable opens and the flags.** The backing filesystem decides at open
  time whether a file may be written (immutable, append-only, a read-only
  filesystem), and the kernel's check on the FUSE side does not see those
  flags. Since every open of an inode shares one backing descriptor, which
  may predate a `chattr +i` or be read-only only because `O_RDWR` was
  refused while this open's `O_WRONLY | O_APPEND` would be allowed,
  `DirCacheFS::Open` checks every writable open that shares an existing
  descriptor. If that descriptor is read-write, the backing filesystem
  allowed writing when it was opened and cannot be remounted read-only
  while it is open, so only an immutable or append-only flag set since
  can refuse the open, through dcfs or behind its back (review L-b): one
  `FS_IOC_GETFLAGS` on the shared descriptor reads the flags from the
  inode in memory (one syscall, no disk I/O, no open file the backing
  filesystem would see), and the open is refused with `EPERM` as
  `may_open` would (immutable, or append-only without `O_APPEND`). If it
  is read-only, the shared descriptor is reopened through `/proc/self/fd`
  with the open's access mode, refused as the backing filesystem refuses
  it. When the shared descriptor is read-only and the open is allowed, that
  reopened descriptor becomes the inode's write descriptor (the first
  writer's; an `O_APPEND` one gives way to one without, review L-a; it
  goes with the last writable open), which fallback writes,
  `fallocate` and `copy_file_range` use (review L1: they got `EBADF`). (Passthrough opens its own
  backing file with the caller's flags, from dcfs's descriptor's path, and
  without that check.)
- **`O_TMPFILE`** (`DirCacheFS::Tmpfile`, `FUSE_TMPFILE`): an unnamed file
  made in the backing directory with `openat(".", O_TMPFILE | O_RDWR)`, as
  the caller, and recorded as a row without a dentry
  (`backing::RecordTmpfile`: dirty, its attributes unknown while it is
  open for writing). The plan said "an in-memory record"; a row is simpler
  and safe: the nodeid has to come from somewhere rows never collide with,
  and the row-lifetime rule already retires the row at the last release
  with `nlink` 0 (into a `removed_` record while the kernel holds the
  nodeid). Creating it changes nothing cached about the directory (ext4,
  xfs and btrfs change neither its entries nor its times). Linking it into
  a name (`linkat` with `AT_EMPTY_PATH`, or of `/proc/self/fd/<n>`) is an
  ordinary `LINK` of that row (`open_by_handle_at` reaches the unlinked
  inode the descriptor holds), which to the directory is a create whose
  object is already known: the model's `linkcreate` (a create with no
  probe), as which `fuse_ops.cc` reports it to the protocol events
  (`DirCacheFS::IsUnnamedTmpfile`). An `O_EXCL` one cannot be linked (the
  kernel refuses before asking dcfs). A tmpfile whose create fails after
  its row was recorded deletes the row again, and `StartRun` deletes the
  rows whose last release never came (a non-directory with no link and no
  name: `cache::ForgetUnnamedRows`, review L5), so crashed tmpfiles do not
  accumulate rows. It does at every start (step 12.4b), not only after an
  unclean shutdown: a `DESTROY` with an unlinked file or a tmpfile still
  open for reading ends in a clean shutdown. The partial index
  `inodes_unlinked` (`nlink = 0`) keeps it from reading the whole table.

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

**`default_permissions` is required** (russ, 2026-10-08). The kernel
checks the caller's permissions against dcfs's cached attributes before a
request is sent, which costs no backing I/O; the alternative, an `ACCESS`
request per permission decision, would be a backing syscall per check or
no more faithful than the kernel's own. `DirCacheFS::Init` refuses a mount
whose options lack it (libfuse then refuses the `INIT` and the daemon
exits non-zero), and `--fuse_opt=default_permissions` is rejected as
redundant. Under it the kernel never sends `ACCESS`; `Access` stays as a
fail-closed path: it replies `EACCES` and logs "ACCESS received:
default_permissions is not in effect". `ENOSYS` would be wrong there: the
kernel takes it to mean "allow every `access(2)` from now on"
(`fc->no_access`).

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
   mount point strictly below `--source`; also refuse a source whose
   superblock went read-only by itself under a read-write mount (see "A
   filesystem that went read-only by itself").
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

### Logging

dcfs uses Abseil logging with Abseil's own flags and semantics
(`docs/style.md` 1.7 has the rules for code). A line goes to standard
error when its level is at least `--stderrthreshold` (dcfs defaults it to
WARNING, so an operator sees problems and nothing else) and is dropped
everywhere when it is below `--minloglevel`. `--v=N` turns on `VLOG(n)`
for `n <= N`; `--vmodule=file=N` does so per source file. What each
threshold shows, with an example of a line (the `I20261008 12:00:00.001234
4242 main.cc:540]` prefix is Abseil's):

| Level | Meaning | Example |
|---|---|---|
| `FATAL` | dcfs cannot continue safely | `F... check.cc] Check failed: fs != nullptr` |
| `ERROR` (default) | the caller got an error dcfs produced, or dcfs refused its job | `E... fuse_request.cc] INTERNAL: RET_CHECK failure: Read on unknown handle 7` |
| `WARNING` (default) | nothing failed for the caller, state is degraded or surprising | `W... backing.cc] inode 42: out-of-band change on the backing filesystem (unsupported): size 4096 -> 8192; adopting the new attributes` |
| `INFO` (`--stderrthreshold=0`) | the lifecycle: start, recovery, sync points, shutdown, the first backing access after an idle period | `I... main.cc] dcfs 1.0 starting: source=/srv/media cache_db=/var/lib/dcfs/media.db mountpoint=/mnt/media mount_options=default_permissions attr_timeout_sec=3600 entry_timeout_sec=3600 sync_interval_sec=5 foreground=true` |
| `--v=1` | one line per request that reached the backing filesystem (the request, how many backing calls, the first), after the lines for the cache decisions behind it | `I... fuse_ops.cc] Lookup(ino=1, name="a") reached the backing: 3 calls, the first getdents64` |
| `--v=2` | every request, with its reply | `I... fuse_ops.cc] Lookup(ino=1, name="a") -> OK` |
| `--v=3` | SQL statements and one line per step | `I... sqlite.cc] sqlite3_step: SELECT ...`, then `sqlite3_step: -> row` |

The INFO lines: the start line (source, cache database, mount point and
options); the recovery summary (`recovery: the last run ended cleanly; 0
dirty entries made unknown, 0 rows of unnamed or unlinked files
forgotten`, then `recovery: probed 12 recovered rows, 3 gone, 0 could not
be probed`); each periodic sync point (`sync point: cleared 14 of 16 dirty rows in
3.2ms`; an fsync's is shown at `--v=1` only); the shutdown (`shutdown: clean (the dirty set is empty and the
clean-shutdown flag is committed)`; one that could not be clean is a
WARNING, `clean shutdown incomplete, the next start will recover the
dirty set: <reason>`, because the next start has work to do); and the
first backing access after an idle period (`first backing access after
1m12s idle (getdents64)`, the first backing call of that request). "Idle"
is a gap of more than 60 seconds (`DirCacheFS::kIdleThreshold`, a constant of
its own: the sync interval is no good, since a sync point only runs while
the dirty set is non-empty, so a read-only workload would log a line after
every five-second pause) between two requests that reached the backing
filesystem, measured from the clock at each request's first backing call
(`Context::first_backing_at`, read once per such request through
`Context::clock`, no syscall). Blind spot: only requests served through
`Serve` (`fuse_ops.cc`) count. Reads and writes of an open file go through
FUSE passthrough and never reach dcfs, and FORGET, the sync points and the
start's own reads do not go through `Serve`, so a daemon whose only traffic
is passthrough I/O logs the line at the next request that does reach the
backing.

A request's failure is logged once, by the FUSE handler that replies: at
ERROR if dcfs produced the error (a status with no errno, one built by
`DcfsErrnoToStatus`, one marked as dcfs's own by `MarkProducedByDcfs` (a
backing change that could not be recorded), or an errno that describes
dcfs's own process: `EMFILE`, `ENFILE`, `ENOMEM`, `EBADF`, `EFAULT`), and
not at all (`--v=2` shows the reply) if the backing filesystem or the
request itself answered with an errno such as `ENOENT`, `ESTALE` or `EINTR`. The
functions below the handler return the status and log nothing
(`docs/style.md` 1.7), so a failure carries one line with all the context
the call chain added.

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

### Runtime invariant checks

The rules this document states in prose are also checked while tests run
(step 26.2), in a testonly build of the daemon and in the forged-request
harness, never in what ships. Every layer calls hooks through
`Context::events`, the one testonly observer (`dcfs/protocol_events.h`,
step 26.4b: the protocol events, these checks' hooks and the cost
counters, with one no-op in production): `backing.cc` before each
backing syscall it makes from a function holding a `Context` (and before
each call into code without one that makes them: `FileHandle`,
`GetDeviceId`, `AsCaller`, its descriptor-only helpers), `DirCacheFS`
before each of those helpers it calls, `fuse_ops.cc` around every request
(after the reply) and every `FORGET` entry, `Startup` at its end (after
its probe of the recovered rows; `StartRun` marks the run's start, with no
check, so that the opens the harness keeps across it are left out), and
DESTROY. Code without a `Context` cannot open a transaction (the database
is reachable only through `Context::db`), so the hook before such a call
covers every syscall the call makes. Production links the no-op
(`protocol_events_main.cc`): one call to an empty function per hook, per
backing syscall and per request, against syscalls that cost microseconds.
A compile-time switch would have made every object differ between the
two builds; a hook inside the `syscalls::` wrappers would have needed a
global, since they have no `Context`.

Not hooked, because no transaction can be open there: `close(2)` of a
backing descriptor by a `FileDescriptor` destructor (a scope's end, in code
that holds no transaction body open: a body that held one across a close
would also hold it across the syscalls before it, which are hooked);
`main.cc`'s startup `open`, `fstat` and `FileHandle::FromFd` of
`--source`, which run before anything opens a transaction and outside any
request; and `fuse_passthrough_open`/`close` (`FuseRequest`), an `ioctl`
on `/dev/fuse` that never reaches the backing filesystem. The harness also
holds the hooks in place from below: it wraps, with `-Wl,--wrap`, every
libc call through which `backing.cc`, `file_handle.cc` and
`device_id.cc` reach the backing filesystem (`openat`, `statx`,
`open_by_handle_at`, `name_to_handle_at`, the `*at` mutations, `syncfs`,
the xattr calls, `fstatat`, `fstatfs`, `fstatvfs`, `fallocate`,
`copy_file_range`, `futimens`; not those SQLite itself makes inside its
transactions, `open`, `pread`, `pwrite`, `fsync`, `ftruncate`, `fstat`,
nor the variadic `ioctl` and `syscall`), and each aborts if a transaction
or a cursor is open, so a backing syscall added without its hook fails
there.

The checking build (`//dcfs:main_static_checked`, linking
`dcfs/testonly/main_invariant_checker.cc`) and the harness install
`testonly::InvariantChecker`, which checks:

- at every backing syscall: no transaction is open and no statement is
  part way through its rows (a read cursor holds a read transaction):
  `no-transaction-at-backing-call`; every inode with a mutation in flight
  has its dirty row (`dirty-set`);
- at the end of every request, after its reply, for the rows the request
  changed (SQLite's update hook names them, so the work is proportional to
  them, not to the database) and the inodes it named. A `DELETE` with no
  `WHERE` (`ClearDirty`'s one-statement clear, `RecoverDirty`) would be a
  truncation, which calls no hook and would hide a dropped dirty row of a
  file no request names (one the kernel writes through passthrough); the
  checker gives `dirty` a no-op `TEMP` trigger (its connection's only,
  never in the database file), and SQLite never truncates a table with a
  trigger: no transaction or cursor open (`no-transaction-at-request-end`);
  attributes recorded as current have every column and a link count above
  0, and a dentry is `refused` only with its stub and a stub's dentry is
  `refused` or `unknown` (`tri-state`); only the root has FUSE generation
  0 (`identity`); an inode in `Context::dirty.durable` has
  its dirty row, `dirty.any` false means no row that is not `atime_only`
  and `dirty.atime` false no `atime_only` row (`dirty-set`); an inode with
  an open backing file has its dirty row unless it is a removed object
  (`open-file`, step 23.8);
  an inode open for writing has its attributes unknown, its dirty row, and
  a `written_` entry unless it is a removed object, and a `BackingFile`
  with writable opens is open for writing (`writable-open`); lookup counts
  are positive and no `FORGET` drops more than was counted
  (`lookup-count`); `held_fds_` is the number of `written_` entries holding
  a descriptor, at most the cap (`held-fds`); a removed record exists only
  while the kernel holds a lookup of it, and never beside a `written_`
  entry (`removed-record`);
- at `Startup`'s end and after DESTROY: all of it over the whole database
  and every in-memory entry.

The checks also run after injected failures (step 26.6,
`//dcfs:dir_cache_fs_fault_sites_test`): one short workload per operation
type (lookup, create, write, unlink, rename, mkdir, rmdir, setattr, xattr,
readdir, open and release, forget) is run once recording the backing call
sites it reaches (a hook's location and each wrapped libc call after it:
the harness wraps them for the backstop), then again on a fresh tree and
cache for each site not yet failed, with that site's first call failed:
`EIO`, and each errno the code branches on there (`ENOENT` and `EACCES`
for `openat`, `ESTALE` and `EPERM` for `open_by_handle_at`, `ENODATA`,
`EOPNOTSUPP` and `ERANGE` for the xattr reads). After each: `CheckAll`, an
unclean `Startup` on the same database, and `CheckAll` again; a violation
fails the test. Three hooks are not failed: their calls (`pwrite`,
`fsync`, `getdents64`) are not wrapped, as SQLite makes the first two
inside its own transactions and the third goes through `syscall`.

What the checklist states but the checker does not: "the dirty set equals
the set of unknown rows" is not an invariant of this design (a population
leaves rows unknown that no mutation touched, an invalidation makes names
unknown without dirtying anything, and phase 3 makes a dirty inode's
records present again), so the checks are the one-way rules above; and
"no held descriptor beside a writable shared descriptor" is not one either
(`Release` keeps a held descriptor across a later writable open of the same
file). Trace validation (`formal/`) checks what the checker cannot see
from one moment's state: the order of a mutation's phases.

A violation aborts the daemon (`LOG(FATAL)`: "invariant violated:
<invariant>: <what> (in request <OPCODE> nodeid <n>)") after writing the
same line, as `DCFS-INVARIANT-VIOLATION ...`, to the console, where
`run-qemu.sh` fails the run on it. Every `small` and `medium` `qemu_test`
(the fast and presubmit tiers) boots the checking build, except
`memory_test` and `readdir_boundary_test`, which measure what ships (its
memory, a listing's time); `large` and `enormous` boot the plain one
(`test/qemu/README.md`, "Test tiers").

The same observer counts what operations cost (step 26.4b,
`dcfs/testonly/cost_counter.h`): SQLite statement steps and outermost
transactions (durable ones apart: each is a WAL fsync), from the cache
database's `Connection` (`set_observer`, kept on the `sqlite3` handle),
FUSE requests by opcode, and backing calls; the checker's own statements
are left out. The harness reads the counts directly; the checking daemon
writes them to `$DCFS_COUNTERS_FILE` after every request, for the guest
budgets (`test/qemu/README.md`, "Budgets").

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
| `readonly_test` | Read-only operations are served from the cache with backing inode numbers; a warm metadata pass reads zero sectors, also after a restart; startup refuses a mount below `--source`, and a boundary that appears at runtime is refused rather than cached (a stub). |
| `boundary_test` | A mount (every filesystem) and a btrfs subvolume appearing below the source are stub directories: listed, a directory with the boundary root's mode and owner and an inode number at or above 2^63 (`d_ino` agrees), `ENOTSUP` for everything inside (logged once per stub), `EXDEV` for renaming the stub, nothing reaching either side; the same inode numbers after a restart. |
| `passthrough_test` | File contents go through passthrough: reads match, move the backing read counter, and cost the daemon almost no CPU for 64 MiB; opens do not leak descriptors; all of it survives a restart. |
| `lifecycle_test` | Flag validation, a bad `--source`, a database for another filesystem refused, `--fuse_opt`, clean `SIGTERM` shutdown (exit 0, unmounted, WAL checkpointed), mounting over the source, restarting against a used database. |
| `handles_test` | NFS-style handles via `name_to_handle_at`/`open_by_handle_at`: generation 0 for the root and nonzero and stable otherwise; handles survive a restart; a doctored generation, a recycled inode number and a wiped database each give `ESTALE`; no handle can be made behind a boundary, and the stub's own handle decodes to the stub. |
| `setattr_test` | chmod (file, directory, FIFO; `EOPNOTSUPP` on a symlink), chown, truncate and utimes land on the backing filesystem and are then served from the cache with zero sectors, also after a restart. |
| `create_test` | mkdir, create, mknod, symlink and link, including error cases; the whole tree's listing agrees with the backing filesystem; zero sectors for a full metadata pass over everything created; `EEXIST` for a boundary stub's name and `ENOTSUP` inside it. |
| `rename_test` | unlink (including of an open file, whose row and handle live until the last close), rmdir, and every rename variant (across directories, over an existing file, `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, a directory with its cached subtree); negative entries and completeness are recorded, not re-read. |
| `atime_test` | A `stat` through dcfs reports exactly the access time the backing filesystem has: after an open that reads nothing (unchanged), a read, a read while the file stays open (a stat while held), `chattr +A` and `O_NOATIME` (unchanged), an mmap read, lsattr's private open (unchanged), and from the cache with no backing read once closed; a directory listing and a readlink stamp the relatime "now" in the cache only (the backing filesystem's stays), a second within the day changes nothing, and all of it survives a restart of dcfs. (The power cut after a read: `fault_power_test`'s `atime` scenario.) |
| `copy_test` | `copy_file_range` through dcfs shares extents on btrfs and xfs as natively and leaves the copy's attributes cached; `FICLONE` fails `EOPNOTSUPP` (the VFS's answer); `lsattr`/`chattr` (`FS_IOC_GETFLAGS`/`SETFLAGS`, `FSGETXATTR`) and `FS_IOC_GETVERSION` match the backing file, `chattr +i` is enforced (also for a file already open for writing), other ioctls get `ENOTTY`; `O_TMPFILE` linked by `AT_EMPTY_PATH` and through `/proc/self/fd`, `O_EXCL` and never linked, each as on the backing filesystem; a warm metadata pass reads nothing. |
| `write_test` | Writes, appends, `O_TRUNC`, a 64 MiB passthrough write, concurrent opens of one file (the one-backing-file rule), fsync, fallocate, xattrs on files, directories and symlinks, ACL read-back after setxattr and chmod, `security.capability` removal on chown, truncate and write; a store through a shared mapping after the last close is reconciled at the inode's last `FORGET` (no out-of-band warning); served from the cache after a restart. |
| `credentials_test` | As two unprivileged users: ownership of every create, setgid inheritance, supplementary groups, chown and chgrp rules, sticky directories, truncate, utimes, chmod and user xattrs, allowed and denied, agree with the backing filesystem; POSIX ACLs (named entries denying and granting access, default ACL inheritance and the umask) are enforced as on the backing filesystem; the daemon is back to root afterwards. |
| `crash_test` | `SIGKILL` while files are open for writing with unflushed passthrough writes: after a restart, sizes and mtimes match the backing files (this failed before writable opens marked attributes unknown). An out-of-band change is noticed on open and logged exactly once; dcfs's own mutations log no false positive. |
| `power_test` | The state a power loss leaves, produced deterministically: mutate through the mount, `SIGKILL`, undo each mutation directly on the backing filesystem, restart. Recovery logs a warning, every touched entry shows the backing filesystem's truth, untouched entries stay warm. With recovery disabled, the checks fail. A periodic sync point empties the dirty set. It cannot produce a real power loss, since a guest's page cache survives anything short of a reboot. |
| `enospc_backing_test`, `enospc_cache_test` | Out of space (step 11.4): the backing filesystem full (create, write, mkdir, setxattr, rename through dcfs fail with `ENOSPC`, as on the backing filesystem, and nothing is served as done; btrfs reserves metadata apart, so only the write must fail there), and the cache database's filesystem full before a create's phase 1, during its phase 3 and a rename's, and before a sync point. Requests fail with `ENOSPC` or are replied as done; the kernel never answers "no such file" for a created file; the dirty set survives; after space is freed (and a restart) everything served matches the backing filesystem. |
| `fault_shutdown_test` | An instant crash of the backing filesystem under a running dcfs (`FS_IOC_SHUTDOWN` in each flavour on ext4 and xfs; a dead disk on btrfs): completed unsynced mutations, a create held in phase 1 and in phase 2, an idle crash, a daemon crash before the backing crash, starts without a remount. Mutations and reads of contents fail, nothing new is served, the dirty set survives the failed clean shutdown, and after the remount everything served matches the backing filesystem. |
| `fault_backing_test`, `fault_cache_test`, `fault_power_test` | Real disk failures through dm-flakey and dm-error (`test/qemu/README.md`, "Fault injection"): read and write errors on the backing disk, write errors on the cache disk, and power cuts (both disks drop writes at one instant) placed in a create's phases and in a sync point with fsfreeze. The error goes back to the caller, the mutation does not reach the backing filesystem when phase 1 failed, the dirty set survives a failed clearing, and after a restart everything served matches the backing filesystem. The power cuts also run as real ones (`fault_power_kill_test`: the host kills QEMU at the cut and a second boot checks), and as bounded sequences of operations with a cut after them (`fault_ace_a_test`, `fault_ace_b_test`, `fault_ace_fs_test`). |
| `release_leak_test` | A failed attribute refresh on the last writable close (forced by holding the SQLite write lock) does not leak the backing descriptor or passthrough registration. |
| `removed_test` | A removed working directory (`stat` reports `nlink` 0, `open(".")` works, listing it fails `ENOENT`) and an `O_PATH` descriptor on an unlinked file behave as on the backing filesystem instead of failing `ESTALE`, also when their rows and attributes were cached, including changing them (truncate, chmod, chown, utimes, xattrs, fsync, through an open descriptor, an `O_PATH` descriptor's magic link or a removed working directory), reopening an unlinked file through `/proc/self/fd`, and linking a removed
file or directory back (the backing filesystem's `ENOENT` and `EPERM`); no `FORGET` exceeds dcfs's lookup count after a tree walk and dropping the kernel's caches. |
| `readdir_boundary_test` | A directory too large for one READDIR or READDIRPLUS reply lists every entry exactly once across several replies, and in time linear in its size (the daemon's CPU ticks for 6000 entries against 1500, at most 8x). |
| `names_test`, `names_random_test` | File names are bytes: about 60 names, one per hazard class (format delimiters, control and high-bit bytes, invalid UTF-8, the overlong "fake slash", NFC/NFD and other look-alike sets in the spirit of xfstests generic/453 and generic/454, path-walk specials, ordering and prefixes, 255-byte names), go through create, mkdir, symlink (including a 4095-byte target; 1023 on xfs), link, xattrs with NUL-containing values, a rename chain, handles, listing and removal, both created directly on the backing filesystem (dcfs populates from it) and created through dcfs, and are compared with the backing filesystem byte for byte; after a restart the same checks pass, the handles taken before it still open and a metadata pass reads zero sectors. Errors for `.`, `..` and 256-byte names match the backing filesystem's, a directory chain deeper than `PATH_MAX` works by descriptors and handles, and a newline in a logged name cannot forge a log line. The random test makes 1,000 seeded names of random bytes, half through dcfs and half on the backing filesystem, and compares the trees. |
| `nfs_test` | dcfs re-exported over loopback NFSv4 from a Debian chroot: listings match, a metadata pass over NFS reads zero sectors, contents match, a file held open over NFS survives a dcfs restart (after `exportfs -f`), writes over NFS land, and a wiped database gives `ESTALE` for an old handle without touching the backing file; nothing behind a boundary is reachable even with `crossmnt` (the stub is listed). |
| `pjdfstest_test` | POSIX conformance, as above. |

What is not covered: real concurrency (there is none to test until
coroutines exist). Step 5.2 built the per-filesystem suites the plan
promised: every e2e test in the table above (and pjdfstest; see
docs/conformance.md) now runs against ext4, xfs and btrfs, not just ext4.

## Known gaps

These are known and accepted for now; the README's Limitations section
lists the user-visible ones.

- **Access times of directories and symlinks are the cache's.** See
  [Access times](#access-times): stamped in the database only, lost with
  it; and a power loss while a file is open and has been read may leave its
  access time behind until its next open and close.
  `st_blocks` may lag behind delayed allocation until the next attribute
  refresh, when dcfs no longer holds the file.
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
  RELEASE is not sent while a writable mapping still exists (FORGET
  reconciliation covers the stores, but only once the kernel lets go of
  the inode; see [mmap after close](#mmap-after-close-held-fd-workaround)).
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
that completeness never hides a name, and that recovery terminates. It also
checks what each request replies, in SibylFS's terms (call, effect,
return): a mutation's effect is its backing syscall and nothing else, and
it replies that syscall's result; an answer (an entry, a negative entry, a
listing, attributes) is one the backing filesystem gave at some instant
between the request's arrival and its reply; `EAGAIN` and `EINTR` come only
from a request whose syscall, if any, did not succeed. Variants
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

A second model, `formal/reval.tla`, covers what dcfs reuses across a
change of the backing filesystem's state that should have invalidated it:
a file's shared backing descriptor and its access mode, the write
descriptor beside a read-only one, the cached mode the kernel's
`default_permissions` check reads, and cached negative entries and
complete listings. Its invariant: a dcfs open succeeds exactly when the
backing filesystem would allow it at that moment, nothing dcfs holds
grants more than that (an open descriptor keeps the rights it was granted
with, as in POSIX), and every write dcfs makes has a descriptor that can
carry it. Variants put back Phase 23's bugs of this kind (the shared
descriptor reused without the GETFLAGS re-check, `chattr +F` forwarded,
the missing write descriptor and the one handed to the last writer). The
fix that was declined, re-checking only after a flag change through dcfs,
holds under exclusive access and fails without it; with changes behind
dcfs's back the model also shows what does not survive whatever the fix
(the cached mode and the cached directory answers), which is the
exclusive-access assumption stated as a checked one. Files' traces from
the forged-request harness are validated against it (`formal/README.md`,
"The revalidation model").

A third model, `formal/lifetime.tla`, covers nodeids (see
[Row lifetime](#row-lifetime) and
[mmap after close](#mmap-after-close-held-fd-workaround)): the kernel's
lookup counts (entry replies minus `FORGET`s, batched or not), open files,
and what dcfs keeps for each nodeid (its row, the removed record, the
`written_` entry and its held descriptor), through unlinks and renames over
an object, `DESTROY`, crashes and the start's sweep of unnamed rows. Its
invariants: a nodeid the kernel holds resolves to the object it was handed
out for (never to another, and not to `ESTALE` while the kernel's reference
keeps the object alive); a row or removed record goes only when nothing
references it; a held descriptor lasts at most from a written file's last
close to its last `FORGET` (or `DESTROY`, or dcfs's removal of its last
link; the cap may leave none); dcfs's count is the kernel's, so no `FORGET` is for a lookup it did
not count and nothing it keeps outlives the last `FORGET`; and after a
crash the start sweeps every unnamed row. Variants put back a non-final
`FORGET` dropping the held descriptor or the removed record, the pre-23.7
crash that left an `O_TMPFILE` row behind, and a `FORGET_MULTI` counted as
one, and three gaps the model found, fixed in step 12.4b: a stub's nodeid
handed out again while the kernel still holds it, and a removed object's
row surviving a crash between an unlink's syscall and its phase 3, or a
`DESTROY` with the unlinked file still open for reading (a clean shutdown,
after which the start did not sweep). Nodeids' traces from the forged-request harness are validated
against it (`formal/README.md`, "The lifetime model").

A fourth model, `formal/ident.tla`, covers identity (see
[Identity model](#identity-model)): what a nodeid and its generation stand
for, how a request (`OpenNode`: the handle, then `VerifyBackingIdentity`)
and an NFS client's handle (the kernel's inode, or `LOOKUP(nodeid, ".")`
and the kernel's generation compare) resolve, through inode-number
recycling, renames, crashes, power losses, cache wipes, stubs and changes
behind dcfs's back, for today's identity and for Phase 14's (nodeid = inode
number), and with what the filesystem lets dcfs see (the handle's
generation, `FS_IOC_GETVERSION`, the birth time). Its invariants: one
`(nodeid, generation)` never stands for two objects; a nodeid the kernel
holds, or a handle it accepts, reaches its object or `ESTALE`, never
another and never `EIO` (dcfs never replies with a generation other than
the kernel's inode has, which would make `fuse_iget` mark it bad); a handle
of an object that still exists is served, and a row whose object is gone
is caught at every entry point. Variants put back an `OpenNode` without the
identity check, a probe reading by name instead of through one descriptor
(in either order of handle and generation), generations from a counter, a
stub generation reused and row ids from `MAX(id)`. What it shows does not
hold is checked too: today a cache wipe, and a power loss that rolls back
rows recorded since the last durable commit, lose the handles of objects
that still exist (`ESTALE`, never another object); a filesystem with
neither generations nor birth times can take a recovered row over for a
recycled inode; and Phase 14's identity, with a recycling behind dcfs's
back, makes the kernel mark the old inode bad, and, for a held nodeid
whose row went, would reach the new object unless it checks the generation
it handed out (only `LOOKUP(".")` has the kernel compare). Each reopen
reports its outcome (`IdentityResolved`), and nodeids' identity traces
from the forged-request harness are validated against it
(`formal/README.md`, "The identity model").
