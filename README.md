# dcfs — directory cache filesystem

dcfs is a FUSE daemon that mirrors a backing directory tree and serves every
read operation except file *contents* from a persistent SQLite cache on an
SSD: lookup, getattr, readdir, readlink, xattrs, access, statfs. Backing
disks spin up only for file-content I/O and for writes. It is safely
NFS-exportable: handles stay valid across daemon restarts, and handles that
cannot be honored fail with ESTALE, never by serving the wrong file.
Filesystems mounted below the source tree, and btrfs subvolumes, are not
supported (see Limitations).

## Write-through cache and nothing more

The backing filesystems are the only authority. Every mutation reaches the
backing filesystem before the cache reflects it. Deleting the cache
database, at any time, including mid-operation or after a crash, never
loses or corrupts data. The only consequences are a cold cache and stale
NFS handles.

## Status

Early development, mid-rewrite, not usable yet (as of September 2026). The
repo is descended from the 2023 "dfs" ublk block-device experiment, whose
git history this repo carries forward; "dfs" is the old name, "dcfs" is the
current one.

## Identity model

The FUSE nodeid is our own 64-bit row id, decoupled from backing identity.
Backing identity of an object is `(device_id, backing_ino, backing_gen)`,
where `device_id` identifies the filesystem and `backing_gen` comes from
`FS_IOC_GETVERSION`. Rows are unique on that triple, so hard links share a
row.

- `inodes.id` (64-bit, never reused) is our identity and the FUSE nodeid.
  Backing identifiers never serve as our keys; with several filesystems in
  play, backing inode numbers are not even unique.
- `st_ino` shown to users is the backing inode number, so hardlink-aware
  tools behave as they would on the backing filesystem.
- The FUSE generation returned on LOOKUP/GETATTR/SETATTR is a uniformly
  random, nonzero 32-bit value drawn for each new row (the root's is 0).
  Neither a rebuilt cache nor a power loss that rolls back recent inserts
  (letting `AUTOINCREMENT` hand an id out again) reissues an old
  `(nodeid, generation)` pair, except with probability 2^-32 per reissued
  id: the old handle gets ESTALE instead of the new object.
- A row is the same backing object only if, beyond `(device_id,
  backing_ino, backing_gen)`, its stored file handle bytes and its birth
  time (when both are known) match too. `backing_gen` is 0 for symlinks
  and special files and on filesystems without `FS_IOC_GETVERSION`, but
  the handle bytes still encode the real generation; and btrfs can reissue
  an identical handle after its own power loss, which only the birth time
  tells apart. Both are already in hand, so this costs no syscall.
- When the backing filesystem recycles an inode number, a new row is
  created with a new id and generation, and the old row is invalidated, so
  a patched kernel rejects stale handles against it instead of resolving
  them to the wrong file.
- Handles survive daemon restarts, since the rows persist. They do not
  survive a cache wipe: reconnection sends the kernel only the nodeid, and
  with multiple backing filesystems there is no way to encode
  `(device_id, ino, gen)` into a nodeid, and even for a single filesystem
  some filesystems (xfs) require the exact original generation. Fixing
  this would mean the daemon owning handle encoding in the kernel, which
  is not needed, so a wipe simply yields ESTALE rather than a wrong file.

## Fds and handles only, never paths

After startup the daemon holds no path strings. This is what makes
mounting dcfs over the directory it caches a supported configuration.

1. At startup, before mounting, the source directory and the cache
   database are opened by path once; after that, paths are never used
   again. (The submount policy check -- see Limitations -- also reads
   `--source`'s path and `/proc/self/mountinfo` once in this same window:
   it is a startup-only policy decision, not part of identity, and keeps
   no path afterwards either.)
2. Backing filesystems are reached through one mount fd per device id,
   never by opening a mount point by path. This must be a real
   (non-`O_PATH`) directory fd: `open_by_handle_at`'s mount fd argument is
   resolved via the kernel's non-raw fd class (`fs/fhandle.c
   get_path_from_fd()`), which rejects `O_PATH` descriptors with `EBADF`.
3. All backing operations use `*at` syscalls relative to fds derived from
   stored handles (`openat`, `fstatat` with `AT_EMPTY_PATH`, `readlinkat`,
   `linkat`, `renameat2`, and so on). Children are reached with
   `openat(dir_fd, name)`, never a joined path.
4. Handles are stored serialized, together with their device id, and are
   the only durable reference to a backing object.

## Filesystem identity and the startup purge

Filesystem identity is the filesystem UUID from `ioctl(fd,
FS_IOC_GETFSUUID)`, always: reboot-stable on ext4, xfs, and btrfs. This
ioctl has existed since Linux 6.9. ZFS returns ENOTTY until OpenZFS ships
the ioctl; that case is treated as unimplemented rather than falling back
to `f_fsid`. Btrfs subvolumes of one filesystem share a UUID but have
separate inode-number spaces, so filesystem identity there also carries the
subvolume id.

There is no `f_fsid` fallback and no libmount dependency; identity itself is
derived entirely from fds. (Startup does use `/proc/self/mountinfo` once,
by path, for the submount policy check below -- that check is deliberately
independent of the identity machinery, and never influences it.)

A `filesystems` table exists to record a backing filesystem below the
source and the dentry through which it was entered, for a future where
kernel-supported FUSE submounts (see Limitations) can be cached like the
source itself. Nothing populates it today: since submounts are refused
outright (below), the source is the only backing filesystem dcfs ever has.
The table, `MountFds`, and `StartupPurge` all stay in place for that future
rather than being deleted now.

At startup, the daemon compares the source filesystem's device id against
the one recorded when the cache was created and refuses to start on a
mismatch. It then purges all cached state -- dentries, inodes, xattrs,
symlinks, and descendants -- for every non-source row left in the
`filesystems` table. Today that only ever matters for a cache database
built before submounts were refused, when such a row could still be
created and its subtree cached; a fresh database's table holds only the
source.

## Crash robustness

Mutations are two-phase, so a crash between the backing change and the
cache update can only cost a repopulation, never wrong data:

1. transaction: mark the affected state unknown (delete the dentry rows
   involved, clear `children_complete` on the parent, clear `attrs_valid`
   on attribute targets);
2. perform the backing syscall;
3. transaction: write the new state.

Unknown state is repopulated from the backing filesystem on next access.

That alone covers a daemon crash, after which the database is exactly its
last committed state. A power loss (or kernel crash) is harder: SQLite's
WAL runs at `synchronous=NORMAL` (commits reach the WAL but are not
fsynced), the backing filesystem commits its journal on its own schedule,
and each comes back as some prefix of what was written, independently. So
the cache could come back *behind* the backing filesystem (phase 1 lost,
the syscall kept) or *ahead* of it (phase 3 kept, the syscall lost). dcfs
closes both with a durable dirty set, at the cost of one WAL fsync per
mutation and no backing flush per mutation:

- Phase 1 also records every inode the mutation changes (both parents and
  the object for a rename, the parent and the child for an unlink, the
  file for a setattr or writable open, ...) in a `dirty` table, and that
  transaction is committed with `synchronous=FULL` (a WAL fsync) before the
  backing syscall. Phase 3 commits normally and never removes dirty
  entries; a row phase 3 creates is dirty too. A phase 1 whose inodes are
  all already durably dirty skips the fsync, since recovery forgets their
  state anyway: a burst of creates in one directory costs one fsync, not
  one per file.
- A sync point `syncfs()`es the backing filesystems and then empties the
  dirty set (except files still open for writing). It runs when the kernel
  sends FSYNC or FSYNCDIR (after the fsync itself), at a clean shutdown,
  and at the start of the first request `--sync_interval_sec` (default 5)
  after the previous one while the set is non-empty. dcfs is
  single-threaded in libfuse's blocking loop and has no timer, so an idle
  daemon keeps a non-empty set until its next request, fsync or shutdown:
  that is safe, it only makes the re-read after a crash larger. (libfuse
  3.18.2 has no SYNCFS handler, and the kernel sends SYNCFS only to
  fuseblk servers anyway.)
- The single-row `cache_state` table (see `dcfs/schema.sql`) records,
  besides the schema version and the source filesystem's identity,
  whether the last run ended cleanly (`clean_shutdown`, set after its final
  sync point and WAL checkpoint) and which boot it ran in (`boot_id`). At startup after an unclean shutdown, dcfs marks everything
  in the dirty set unknown: attributes, xattrs, symlink target, a
  directory's whole listing, and the dentries pointing at each entry. Rows
  are kept, so NFS handles still resolve (and are verified when next
  opened). It logs a WARNING with the count and whether the machine
  rebooted (a crash or power loss) or only the daemon died.

What a power loss costs is therefore re-reading the entries mutated in the
last few seconds before it (up to `--sync_interval_sec`, or since the last
fsync), never serving state the backing filesystem did not keep.

Writes to file contents go from the kernel straight to the backing file
(FUSE passthrough), so dcfs never sees them. A writable open or create is
therefore phase 1 of a mutation: it marks the file's cached attributes
unknown before the open is replied to, and they stay unknown until the last
writable open of that file is released (phase 3), however often they are
read or refreshed meanwhile. While the file is open, attribute reads are
served by a `statx` of dcfs's already-open backing fd, so this costs no
extra open and no disk access. A crash while a file is open for writing
leaves its attributes unknown, never the pre-write size and mtime marked
current. Such a file stays in the dirty set until that last release, sync
points notwithstanding. Likewise, the attributes of an unlinked file dcfs
still holds open (link count 0) are never recorded as current: its row
only lives until the last close deletes it.

## Coherence

dcfs requires exclusive access to the backing trees: all access to them
must go through dcfs. There is no fanotify integration and no TTL-based
revalidation, and changes made to the backing trees behind dcfs's back
are unsupported.

They are, however, detected and logged when a syscall dcfs makes anyway
reveals them; dcfs never adds a syscall just to look. Whenever dcfs opens
an object by handle it already `statx`es it to verify its identity, and
when it lists a directory it already `statx`es every child. If the object
is no longer the one the cache describes (a recycled inode number or
generation), dcfs logs a WARNING and forgets the row (ESTALE). If its
cached attributes are marked current but disagree with the fresh `statx`
in mode, owner, group, link count, size, mtime or ctime, dcfs logs one
WARNING ("out-of-band change on the backing filesystem (unsupported)")
naming the fields that changed, then:

- adopts the fresh attributes;
- for a directory whose mtime or ctime changed, forgets its negative
  entries and marks its listing incomplete, so the next lookup or readdir
  lists it again;
- for any object whose ctime changed, marks its cached xattrs unknown.

The kernel's own attribute and dentry caches are not invalidated; they
pick up the change once they expire or are evicted. Anything served
purely from the cache (every lookup, getattr, readdir or xattr read that
needs no backing I/O) detects nothing.

## Limitations

- **No out-of-band access to the backing trees** (see Coherence above):
  dcfs requires exclusive access, detects what it stumbles onto for free,
  and never invalidates the kernel's own caches.
- **The kernel is not told about out-of-band changes.** Even when dcfs
  detects one and updates its own cache, the kernel keeps serving the
  attributes and dentries it already has until their timeouts expire or
  it evicts them; dcfs sends no invalidation notifications.
- **Writes through a shared writable mapping after the last `close()`
  are not reflected in metadata.** With passthrough the mapping keeps
  only the backing file, so the kernel releases the dcfs file on
  `close()` and later stores reach the backing file without dcfs (or the
  kernel's attribute cache) hearing of them: mtime, ctime and size stay as
  cached (and NFS clients, which derive change attributes from ctime, may
  keep stale data). While the file is still open for writing, attributes
  are served with a zero timeout from the open file, so they are current.
- **Filesystems mounted below the source, and btrfs subvolumes, are not
  supported.** One superblock means one `st_dev`, and `st_ino` (shown to
  users unchanged, so hardlink-aware tools such as `tar`/`rsync`/`cp -a`
  behave as they would on the backing filesystem) is only unambiguous
  within one `st_dev`; several backing filesystems under one source would
  let two different objects collide on the same `(st_dev, st_ino)` pair.
  So:
  - At startup, before mounting, dcfs looks for any mount whose mount
    point lies strictly below `--source` (`dcfs/mounts_below.h`, reading
    `/proc/self/mountinfo`) and refuses to start if it finds one, naming
    it in the error. A mount *on* `--source` itself is fine (that is what
    `--source` always is).
  - A boundary that only appears at runtime -- a filesystem mounted after
    dcfs started, or a btrfs subvolume, which the startup check cannot see
    since it is not a separate mount -- is refused when dcfs next lists
    the directory it sits in: the ERROR is logged once, the name is left
    out of the cached listing and out of readdir, and looking it up
    (`stat`, `open`, ...) fails with EXDEV instead of being served from a
    cache that would mix two filesystems' inode numbers. Unmounting it
    makes it visible again the next time its parent directory is
    repopulated.
  - The device id still lives in dcfs's identity model, the `filesystems`
    table, `MountFds`, and the boundary-detection code (`IsBoundary`); none
    of it is used to *serve* a boundary today, but it is what a future
    kernel-supported FUSE submount (`FUSE_ATTR_SUBMOUNT`, today
    virtiofs-only, and would need an INIT-time opt-in for `/dev/fuse`)
    would plug into, so that the kernel -- not dcfs's own `st_dev` -- keeps
    two filesystems' inode numbers apart.

## Design direction

The code today is deliberately simple: plain functions and small classes,
synchronous, single-threaded, no template trampolines, no threads, no async, so each
step is a small reviewable diff.

The eventual architecture is C++ coroutines over io_uring, including
FUSE-over-io_uring for requests and io_uring for backing I/O and
potentially SQLite's own I/O through a pluggable VFS. Today's code only
has to not preclude that future. A few rules keep the eventual rewrite
mechanical:

- every cache and backing operation takes an explicit `Context&`; no
  globals, no singletons, no `thread_local`;
- a transaction never spans a point that could later suspend: backing I/O
  happens first, then one short synchronous transaction;
- only `dcfs/backing.{h,cc}` touches the backing filesystems through
  syscalls; the cache and FUSE op layers never call the syscall wrapper
  directly, so the io_uring rewrite replaces one module;
- the SQLite wrapper opens connections through one factory that accepts a
  VFS name, so an io_uring `sqlite3_vfs` can be dropped in later.

## Requirements

- A Linux kernel carrying the `FUSE_ATTR_GENERATION` patch:
  https://lore.kernel.org/all/20260927141437.1432584-1-russ@har.mn/
- libfuse 3.18.2 plus the patch carried in `third_party/libfuse/` (added in
  a later step).
- Bazel, via bazelisk.
- FUSE passthrough requires Linux >= 6.9.

## Building and testing

dcfs requires root (real `open_by_handle_at`, `FS_IOC_GETFSUUID`, and so
on), so there is no host-side test execution: every test, unit tests
included, boots the project's own minimal Linux kernel under QEMU and runs
as root inside it. `bazel build //...` needs nothing beyond Bazel, but
`bazel test //...` additionally needs:

- KVM (`/dev/kvm`, and your user in the `kvm` group -- see
  `test/qemu/README.md` if you're not; QEMU falls back to software
  emulation otherwise, which is far slower);
- the test kernel built once with `test/qemu/scripts/build-kernel.sh` (see
  `test/qemu/README.md` for what it needs and how long it takes).

```
bazel build //...
bazel test //...                       # everything: unit + e2e, all in QEMU
bazel test --config=asan //dcfs:...
```

Unit-test VMs boot in well under a second (see `test/qemu/README.md`);
e2e tests (tagged `e2e`) are bigger and slower but run the same way, with
no separate `--config` needed.

Use `tools/format.sh` to format the source before sending a change.

## Further reading

- The kernel patch:
  https://lore.kernel.org/all/20260927141437.1432584-1-russ@har.mn/
- https://russ.har.mn/blog/2026-04-09/fuse-loopback-is-incomplete

## License

TBD.
