# dcfs: directory cache filesystem

dcfs is a FUSE filesystem that mirrors one backing directory tree and
answers every metadata operation from a persistent SQLite cache, so that
the disks holding the tree only have to spin up when someone reads or
writes file contents.

The intended setup is a large tree on spinning disks that are allowed to
spin down, with the cache database on an SSD. Once a directory has been
listed through dcfs, `lookup`, `getattr`, `readdir`, `readlink`,
`getxattr`, `listxattr`, `access` and so on are served from the database
without touching the backing disks. File contents are not cached: reads
and writes go from the kernel straight to the backing file through FUSE
passthrough, at native speed and without copying through the daemon.

dcfs is a write-through cache and nothing more. The backing filesystem is
the only authority: every change made through dcfs reaches the backing
filesystem before the cache records it, and deleting the cache database at
any time (even mid-operation or after a crash) never loses or corrupts
data. The cost of losing the database is a cold cache and stale NFS
handles.

dcfs is designed to be exported over NFS. Its file handles stay valid
across daemon restarts, and a handle that can no longer be honoured fails
with `ESTALE` rather than resolving to the wrong file.

Contents:

- [Status](#status)
- [Requirements](#requirements)
- [Building](#building)
- [Usage](#usage)
- [Testing](#testing)
- [Design overview](#design-overview)
- [Limitations](#limitations)
- [Further reading](#further-reading)
- [License](#license)

## Status

Pre-release (October 2026). The single-threaded daemon is complete for its
planned scope: read-only operations from the cache, write-through for every
mutation, crash and power-loss recovery, caller credentials, and NFS
export. The full test suite passes against a stock kernel and stock
libfuse, including pjdfstest with zero dcfs-specific failures against
ext4. It has not yet seen production use.

The repository carries forward the history of a 2023 experiment called
"dfs" (a ublk block device); "dcfs" is the current project.

## Requirements

- **Root.** dcfs reopens cached objects with `open_by_handle_at(2)`
  (`CAP_DAC_READ_SEARCH`), mounts FUSE and registers passthrough files
  (`CAP_SYS_ADMIN`), and switches to each caller's filesystem credentials.
  There is no unprivileged mode.
- **Linux 6.9 or later** for `FS_IOC_GETFSUUID` (filesystem identity) and
  FUSE passthrough (`CONFIG_FUSE_PASSTHROUGH`).
- **A backing filesystem that supports file handles
  (`name_to_handle_at`) and a stable filesystem identity.** ext4, xfs and
  btrfs are all tested (step 5.2: every e2e test runs on all three). ext4
  and xfs report that identity through `FS_IOC_GETFSUUID` directly; btrfs
  does not implement `FS_IOC_GETFSUUID` at all (no `fs/btrfs/*.c` file
  calls the kernel's `super_set_uuid()`, on any kernel version, so the
  generic ioctl always returns `ENOTTY` for it), so dcfs falls back to
  `BTRFS_IOC_FS_INFO`'s `fsid` field there instead -- the same UUID
  `btrfs filesystem show`/`blkid` report, reached through a btrfs-specific
  ioctl rather than the generic one. ZFS is not supported until OpenZFS
  implements `FS_IOC_GETFSUUID`; dcfs refuses to start on it.
- **To build:** Bazel through
  [bazelisk](https://github.com/bazelbuild/bazelisk) (the repository pins
  Bazel 9.2.0 in `.bazelversion`) and a C++20 compiler. Every library
  dependency (Abseil, SQLite, stock libfuse 3.18.2) is fetched and built
  by Bazel; no system libfuse is needed.
- **To test:** KVM and `mkfs.ext4`/`mkfs.btrfs`/`mkfs.xfs`. QEMU, its
  qboot firmware, the guest's busybox and (for `nfs_test`) `mke2fs` are
  all pinned and built by Bazel, not installed on the host. See
  [Testing](#testing).

## Building

```
bazel build //...                 # everything, including the test binaries
bazel build //dcfs:main           # the daemon
bazel build //dcfs:main_static    # the daemon, fully statically linked
```

The daemon is `bazel-bin/dcfs/main` (or `bazel-bin/dcfs/main_static`).
The static build is what the test initramfs uses and is the easiest one to
install on another machine:

```
sudo install -m 0755 bazel-bin/dcfs/main_static /usr/local/bin/dcfs
```

`bazel build //...` works without the test kernel: until it is built, the
kernel is a placeholder that makes any test fail fast with a pointer to
the build script.

Format changes with `tools/format.sh` (clang-format and buildifier, if
installed) before sending them.

## Usage

```
dcfs --source=<dir> --cache_db=<path> [flags] <mountpoint>
```

### Flags

| Flag | Default | Meaning |
|---|---|---|
| `--source` | (required) | The directory to cache. Opened once at startup; dcfs never uses the path again. |
| `--cache_db` | (required) | The SQLite cache database. Created if missing, mode 0600 (its `-wal`/`-shm` files inherit that mode too), since it holds metadata as sensitive as `--source`'s: every cached name, attribute, xattr and symlink target, including those of directories a reader cannot list. Its directory is created mode 0700 if missing; an existing one that is group- or world-accessible logs a warning but does not stop dcfs from starting. Put it on an SSD, not on the backing disks, on a local filesystem: dcfs refuses to start if SQLite cannot use WAL mode there. |
| `<mountpoint>` | (required) | Where to mount dcfs. May be the same path as `--source`. |
| `--allow_other` | `false` | Mount with `-o allow_other`, so users other than root can use the mount. Needed for almost any real deployment, and for NFS export. |
| `--attr_timeout_sec` | `3600` | How long the kernel may cache an inode's attributes. Long by design, since dcfs has exclusive access. While a file is open for writing, its attributes are always returned with a timeout of 0. |
| `--entry_timeout_sec` | `3600` | How long the kernel may cache a lookup result, including a negative one. |
| `--sync_interval_sec` | `5` | While mutations have left dirty cache entries, the first request this many seconds after the last sync point runs a new one (`syncfs` of the backing filesystem, then the dirty set is cleared). Bounds how much is re-read after a power loss. |
| `--foreground` | `true` | Stay in the foreground. With `false`, dcfs daemonizes after mounting and its standard error goes to `/dev/null`, so its log is lost. |
| `--fuse_opt` | (empty) | Extra mount options passed to libfuse as `-o <opts>`, comma-separated, e.g. `--fuse_opt=max_read=65536`. Repeating the flag replaces the previous value, so combine options in one flag. `default_permissions` is always added. |

dcfs uses Abseil logging, so Abseil's logging flags work too: `--v=1`
enables per-request debug logging, and `--stderrthreshold` (default
`WARNING` in dcfs) controls what reaches standard error. At the default
level the daemon only logs warnings and errors, such as out-of-band
changes it noticed or recovery after an unclean shutdown.

libfuse mounts with `nosuid,nodev` by default. Pass `--fuse_opt=suid,dev`
if setuid binaries or device nodes on the backing tree must work through
the mount.

### Example

```
sudo mkdir -p /var/lib/dcfs /mnt/media
sudo dcfs --source=/srv/media --cache_db=/var/lib/dcfs/media.db \
    --allow_other /mnt/media
```

The first listing of each directory reads it from the backing disk and
records everything about its entries (attributes, file handle, symlink
target, xattrs). After that, `find /mnt/media -ls` or `ls -lR` reads
nothing from the backing disk, also after dcfs is restarted. Opening a
file for reading or writing does spin the disk up.

### Mounting over the source directory

Mounting dcfs on the directory it caches is supported:

```
sudo dcfs --source=/srv/media --cache_db=/var/lib/dcfs/media.db \
    --allow_other /srv/media
```

This works because dcfs opens `--source` before mounting and never uses a
path again: every later access goes through that descriptor or through
file handles stored in the cache. Everything that used `/srv/media` before
now goes through dcfs and cannot bypass it by accident, which makes this
the easiest way to honour the exclusive-access requirement (see
[Limitations](#limitations)).

### Running under systemd

`packaging/dcfs.service` and `packaging/dcfs.env.example` are a unit and
its environment file:

```
sudo install -m 0644 packaging/dcfs.service /etc/systemd/system/
sudo install -D -m 0644 packaging/dcfs.env.example /etc/dcfs/dcfs.env
sudoedit /etc/dcfs/dcfs.env    # set SOURCE, CACHE_DB, MOUNTPOINT, EXTRA_ARGS
sudo systemctl daemon-reload
sudo systemctl enable --now dcfs
```

The unit runs dcfs as root in the foreground and restarts it if it exits
with an error. `EXTRA_ARGS` in the environment file holds further flags,
split at whitespace; the example sets `--allow_other`, which users other
than root, and nfsd, need. Before each start the unit lazily unmounts a
dead FUSE mount left on the mount point by a crash (see below), and
leaves a mount point that can be accessed alone.

### Exporting over NFS

nfsd needs `--allow_other`, and a FUSE filesystem needs an explicit `fsid=`
in its export. An `/etc/exports` line:

```
/mnt/media  192.168.1.0/24(rw,fsid=1,no_subtree_check)
```

NFS clients keep their handles across dcfs restarts. After restarting
dcfs, though, run

```
sudo exportfs -f
```

nfsd's export cache holds a reference to the old mount and answers `EIO`
until it is flushed. This is ordinary NFS administration for any
filesystem that is unmounted and remounted under an export.

If the cache database is deleted, handles held by NFS clients become stale
(`ESTALE`); they never resolve to a different file.

### Shutdown, crashes and restarts

`SIGTERM`, `SIGINT` or `SIGHUP` (and so `systemctl stop`) shut dcfs down
cleanly: it unmounts, syncs the backing filesystem, empties the dirty set,
checkpoints the SQLite WAL, records a clean shutdown and exits 0. If a
file was still open for writing (possible after a lazy unmount), dcfs
leaves the clean-shutdown flag unset so that the next start re-reads what
that file's writes may have changed.

If dcfs stops without a clean shutdown (a crash, `SIGKILL`, a kernel crash
or a power loss), the next start notices, logs a warning with the number
of affected entries, and forgets everything cached about the entries
changed since the last sync point, so that they are re-read from the
backing filesystem. Inode rows are kept, so NFS handles keep working.

After a crash the dead FUSE mount stays in place, and accessing it fails
with `ENOTCONN`. Unmount it (`umount -l <mountpoint>`) before starting dcfs
again (the systemd unit does this itself). When dcfs is mounted over its
own source this is mandatory, since dcfs would otherwise try to open the
dead mount as its source.

Only one dcfs may use a cache database at a time: dcfs takes an exclusive
lock on it at startup and refuses to start if another process holds it.
dcfs also refuses a database that was built for a different filesystem or
a different source directory; delete it to start with a cold cache.

## Testing

Every test, unit tests included, runs as root inside a QEMU guest booted
from the project's own minimal kernel. There is no host-side test
execution: dcfs needs root, `open_by_handle_at`, `FS_IOC_GETFSUUID` and
FUSE passthrough, none of which a development host can be assumed to have.
Guests boot in about a second with KVM (QEMU `microvm`, direct kernel
boot), so `bazel test //...` is dominated by compilation, not booting.
`test/qemu/README.md` has the full details.

### One-time setup

1. Install `mkfs.ext4`, `mkfs.btrfs`, `mkfs.xfs` (for tests with a scratch
   disk). QEMU, its qboot firmware, busybox and the test kernel are all
   pinned and fetched/built by Bazel (`//third_party/qemu`,
   `//third_party/busybox`, `//third_party/linux`); there is no host QEMU,
   qboot, busybox or manual kernel build step any more -- see
   `test/qemu/README.md`.
2. Get write access to `/dev/kvm`: `sudo usermod -aG kvm "$USER"`, then log
   in again. Without KVM, QEMU falls back to software emulation, which is
   more than ten times slower and can make tests time out.
3. For `nfs_test` only, the small Debian root image it chroots into is
   built by Bazel from a pinned package set, using the pinned, Bazel-built
   `//third_party/e2fsprogs:mke2fs` (Phase 4c; not a host tool):

   ```
   bazel build //third_party/debian:rootfs
   ```

   See `third_party/debian/README.md` for the pin and package list.

### The `kvm` group and the Bazel server

Until you log in again after joining the `kvm` group, run tests under
`sg kvm -c '...'`. Bazel keeps a long-lived server process, and the tests'
QEMU processes inherit that server's groups, not your shell's. If the
server was first started without the `kvm` group, `sg kvm -c 'bazel test
...'` is not enough: run `bazel shutdown` first so that the next invocation
starts a server that has the group.

### Running tests

```
sg kvm -c 'bazel test //...'                         # everything
sg kvm -c 'bazel test //dcfs:metadata_cache_test'    # one unit test
sg kvm -c 'bazel test //test/qemu:write_test --test_output=streamed'
sg kvm -c 'bazel test --config=asan //dcfs:all'      # under ASan
```

`--test_output=streamed` shows a guest's console as it runs. Afterwards,
each test's output is in `bazel-testlogs/<package>/<target>/test.log`, and
the guest's serial console (including dcfs's own log, for the end-to-end
tests) in `bazel-testlogs/<package>/<target>/test.outputs/serial.log`.

### What is tested

- **Unit tests** (`dcfs/*_test.cc`, Bazel macro `qemu_cc_test`): the
  SQLite wrapper, schema migration, the metadata cache, the backing layer,
  file handles, filesystem identity, the submount check, the syscall
  wrappers (including fault injection through link-time wrappers), and
  status and errno handling. Tests that need real filesystem semantics get
  a scratch ext4 disk.
- **End-to-end tests** (`test/qemu/guest/*.sh`, macro `qemu_test`) run the
  real daemon against a scratch ext4 disk and check every result both
  through the mount and directly on the backing filesystem. Many of them
  also read `/sys/block/<dev>/stat` to prove that a warm metadata pass
  reads zero sectors from the backing disk, including after a restart:
  `readonly_test`, `passthrough_test`, `lifecycle_test`, `handles_test`,
  `setattr_test`, `create_test`, `rename_test`, `write_test`,
  `credentials_test`, `crash_test`, `power_test`, `release_leak_test`,
  `readdir_boundary_test`, `removed_test`, `nfs_test` and `boot_test`.
  [`docs/design.md`](docs/design.md#test-strategy) says what each one
  proves.
- **POSIX conformance**: `pjdfstest_test` runs all of pjdfstest (about 8800
  checks, as root and as unprivileged users), once through dcfs and once
  directly on the same ext4 filesystem, and fails on any dcfs-specific
  failure. Today both runs fail the same 28 checks. See
  `docs/conformance.md`. This test is slow (tens of minutes).

Bugs get a regression test first: the test is shown to fail on the
unfixed code, then the fix makes it pass.

The GitHub Actions workflow (`.github/workflows/ci.yml`) only builds
(`bazel build //...`): hosted runners have no KVM, so the tests need a
KVM-capable machine set up as above.

## Design overview

dcfs is layered so that exactly one module touches the backing filesystem
for request work:

```
kernel FUSE <-> fuse_ops.cc -> DirCacheFS (dir_cache_fs.cc)
                                  |                    |
                                  v                    v
                        backing:: (backing.cc)   cache:: (metadata_cache.cc)
                        syscalls on the          pure SQLite, no syscalls
                        backing filesystem
```

**Identity.** Every backing object dcfs has seen has a row in the cache.
The row id is the FUSE node id, is never reused, and carries a random
32-bit generation; `st_ino` shown to users is the backing inode number.
A row is tied to its backing object by the filesystem's UUID, the inode
number and generation, the file handle bytes and the birth time. dcfs
holds no paths after startup: it reaches objects by reopening stored file
handles and reaches children with `openat` relative to a directory
descriptor. That is what allows mounting dcfs over its own source.

**Explicit states.** Every cached fact is present, absent or unknown, and
a directory listing is either complete (a name without a row is absent) or
not. A lookup the cache cannot answer probes just that name, or lists the
whole directory once; after that, the directory is served from the cache.

**Write-through in three phases.** Every mutation (1) marks what it is
about to change as unknown and records the affected inodes in a durable
dirty set, committed with an fsync of the WAL; (2) performs the backing
syscall, as the calling user where that matters; (3) records the new
state. A crash at any point leaves at worst unknown entries, which are
re-read on demand. Sync points (`syncfs`, then clearing the dirty set) run
on `fsync`, every few seconds while there is dirty state, and at shutdown;
after an unclean shutdown, everything still in the dirty set is forgotten.
This bounds what a power loss costs to re-reading the entries changed in
the last few seconds.

**File contents** go through FUSE passthrough on one shared backing file
per inode. While a file is open for writing, its cached attributes stay
unknown and are answered from a `statx` of the open file.

**Future.** The code is deliberately synchronous and single-threaded
today, but follows rules (an explicit context everywhere, no transaction
spanning a backing syscall, guarded cache fills) so that it can move to
C++ coroutines over io_uring.

[`docs/design.md`](docs/design.md) covers all of this in depth: goals and
assumptions, the identity model, the full schema, the write-through and
recovery protocol, concurrency, and the test strategy.

## Limitations

- **Exclusive access to the backing tree is required.** Everything that
  changes the backing tree must go through dcfs. There is no fanotify
  watch and no time-based revalidation. Changes made behind dcfs's back
  are unsupported: dcfs notices them only when a syscall it makes anyway
  reveals them (when it reopens an object by handle, or lists a
  directory), then logs a warning and adopts what it sees. Anything
  answered purely from the cache notices nothing and stays stale.
- **The kernel's caches are not invalidated** when dcfs does notice an
  out-of-band change. The kernel keeps serving attributes and dentries it
  already has until their timeouts (an hour by default) expire or it
  evicts them. This is a deliberate decision: sending invalidations safely
  needs a separate notifier thread, and out-of-band changes are
  unsupported anyway.
- **No filesystems mounted below the source, and no btrfs subvolumes.**
  All objects in a FUSE mount share one `st_dev`, and dcfs shows backing
  inode numbers as `st_ino`, so two filesystems could produce colliding
  `(st_dev, st_ino)` pairs and confuse hard-link detection in `tar`,
  `rsync` and `cp -a`. dcfs refuses to start if anything is mounted below
  `--source`, checked against `/proc/self/mountinfo` -- which, because a
  btrfs subvolume is not a separate mount, cannot catch a subvolume that
  already exists under `--source` before dcfs starts: this is a real gap,
  not an oversight, and there is no way to detect it at startup short of
  walking the whole tree first (which dcfs deliberately never does; see
  "Coherence" below). What dcfs *does* catch, for a subvolume exactly as
  for a real mount, is the boundary appearing at runtime -- a new mount, or
  a btrfs subvolume (pre-existing or freshly created), the first time dcfs
  lists the directory it lives in: logged as an error, left out of
  directory listings, and `EXDEV` when looked up (step 5.2's
  `create_test_btrfs` covers this). Kernel support for FUSE submounts
  (`FUSE_ATTR_SUBMOUNT`, today used only by virtiofs) would allow lifting
  this.
- **Writes through a shared writable `mmap` after the last `close()` are
  not tracked.** With passthrough, the mapping holds only the backing file,
  so the kernel releases the dcfs file at `close()` and later stores reach
  the backing file without dcfs hearing of them. The file's cached size,
  mtime and ctime stay as they were at `close()`, and NFS clients, which
  detect changes through ctime, may keep serving stale data. While the
  file is still open for writing, attributes are current. Fixing this
  needs a kernel change.
- **NFS handles do not survive deleting the cache database.** They fail
  with `ESTALE`, never by resolving to a different file. Handles do survive
  restarts of dcfs.
- **Single-threaded.** dcfs serves one request at a time, so a request that
  has to wait for a disk to spin up delays every other request, including
  ones the cache could answer. The coroutine and io_uring design that
  lifts this is future work.
- **Power loss re-reads recent changes.** After a power loss or kernel
  crash, everything cached about entries changed in the last
  `--sync_interval_sec` seconds (or since the last `fsync`) is forgotten
  and re-read from the backing filesystem, which spins it up. An idle dcfs
  has no timer, so a dirty set left by the last burst of activity is only
  cleared by the next request, `fsync` or shutdown; that is safe but makes
  the re-read larger.
- **Generation 0 objects.** dcfs reads the backing inode generation
  (`FS_IOC_GETVERSION`) only for regular files and directories; symlinks,
  device nodes, FIFOs and sockets, and every object on a filesystem without
  generations, have generation 0. For them, detecting a recycled inode
  number relies on the stored file handle and the birth time. ext4, xfs
  and btrfs encode the generation in their handles, so this is covered
  there; on a filesystem whose handles carry no generation and which
  reports no birth time, a stale row could match a new object.
- **Removed objects that are still referenced can be read, not changed.**
  A process whose working directory was removed, or an `O_PATH`
  descriptor on an unlinked file, sees what a local filesystem shows
  (`stat` works and reports `nlink` 0, listing a removed directory finds
  nothing). Changing such an object (`chmod` of a removed working
  directory, say), or reopening an unlinked file through
  `/proc/<pid>/fd/<n>` of an `O_PATH` descriptor, fails with `ESTALE`,
  where a local filesystem would allow it.
- **atime is not maintained.** Reads through passthrough update the
  backing file's access time, but dcfs keeps serving the one it last
  recorded. `st_blocks` can also lag behind delayed allocation until the
  file's attributes are next refreshed.
- **Not implemented:** `O_TMPFILE`, `copy_file_range`, reflinks
  (`FICLONE`) and other ioctls; tools fall back to plain reads and writes.
  File locks are handled by the kernel, locally within the mount.
- **Filesystem coverage.** ext4, xfs and btrfs are all exercised by the
  test suite (step 5.2). ZFS is refused until it supports
  `FS_IOC_GETFSUUID`.

## Further reading

- [`docs/design.md`](docs/design.md): the detailed design.
- [`docs/conformance.md`](docs/conformance.md): pjdfstest results.
- [`test/qemu/README.md`](test/qemu/README.md): the QEMU test
  infrastructure.
- Background: https://russ.har.mn/blog/2026-04-09/fuse-loopback-is-incomplete

## License

To be decided.
