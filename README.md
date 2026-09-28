# dcfs — directory cache filesystem

dcfs is a FUSE daemon that mirrors a backing directory tree, including any
filesystems mounted and btrfs subvolumes below it, and serves every read
operation except file *contents* from a persistent SQLite cache on an SSD:
lookup, getattr, readdir, readlink, xattrs, access, statfs. Backing disks
spin up only for file-content I/O and for writes. It is safely
NFS-exportable: handles stay valid across daemon restarts, and handles that
cannot be honored fail with ESTALE, never by serving the wrong file.

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
- The FUSE generation returned on LOOKUP/GETATTR/SETATTR is our own
  32-bit counter, seeded from a random value at cache-creation time, so a
  rebuilt cache cannot reissue an old `(nodeid, generation)` pair.
- When the backing filesystem recycles an inode number (a new
  `backing_gen`), a new row is created with a new id and generation, and
  the old row is marked bad, so a patched kernel rejects stale handles
  against it instead of resolving them to the wrong file.
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
   again.
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

There is no `f_fsid` fallback and no `/proc/self/mountinfo` or libmount
dependency; everything is derived from fds. A `filesystems` table records
each backing filesystem below the source and the dentry through which it
was entered.

At startup, the daemon compares the source filesystem's device id against
the one recorded when the cache was created and refuses to start on a
mismatch. It then walks the recorded filesystems from the source down,
reopens each boundary through its parent's mount fd, and purges all cached
state, dentries, inodes, xattrs, symlinks, and descendants, for any
filesystem that is not currently mounted where it was recorded. Mount
boundaries are rediscovered the next time their parent directory is
populated, so unmounting a sub-filesystem is equivalent to a cold cache for
that subtree, never wrong data.

## Crash robustness

Mutations are two-phase, so a crash between the backing change and the
cache update can only cost a repopulation, never wrong data:

1. transaction: mark the affected state unknown (delete the dentry rows
   involved, clear `children_complete` on the parent, clear `attrs_valid`
   on attribute targets);
2. perform the backing syscall;
3. transaction: write the new state.

Unknown state is repopulated from the backing filesystem on next access.

Writes to file contents go from the kernel straight to the backing file
(FUSE passthrough), so dcfs never sees them. A writable open or create is
therefore phase 1 of a mutation: it marks the file's cached attributes
unknown before the open is replied to, and they stay unknown until the last
writable open of that file is released (phase 3), however often they are
read or refreshed meanwhile. While the file is open, attribute reads are
served by a `statx` of dcfs's already-open backing fd, so this costs no
extra open and no disk access. A crash while a file is open for writing
leaves its attributes unknown, never the pre-write size and mtime marked
current.

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
