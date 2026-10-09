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
- [Operations](#operations)
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
  There is no unprivileged mode: `mount.dcfs` refuses to run as another user
  and says why (see [Responsibilities](#responsibilities-of-the-administrator)).
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
  Bazel 9.2.0 in `.bazelversion`). Every library dependency (Abseil,
  SQLite, stock libfuse 3.18.2) is fetched and built by Bazel; no system
  libfuse is needed. Nor is a host compiler, linker, or C library header or
  static library: the compiler is LLVM's release and the glibc and kernel
  headers are a pinned Debian sysroot (`third_party/llvm/README.md`).
- **To test:** KVM (optional, but tests are 2-9x slower under TCG), network
  access once per pin, and the host tools below. QEMU, its qboot firmware,
  the guest's busybox and the mkfs tools (`mke2fs`, `mkfs.xfs`,
  `mkfs.btrfs`) are all pinned and built by Bazel, not installed on the
  host. See [Testing](#testing).

### Host requirements

Bazel builds everything else, but a few build and test steps still use the
host's tools. This is the complete list, kept honest by running the whole
suite in a fresh GitHub-runner-like container (`act`, see
[CI](#continuous-integration)); `.github/ci/prepare.sh` installs exactly
these packages on a CI runner.

| Package (Debian/Ubuntu) | Used by | Why it is not hermetic yet |
|---|---|---|
| `libc6` 2.36 or newer, `bash`, `coreutils` (`mktemp`, `realpath`, `rm`) | running the pinned clang, lld and llvm-nm (their ELF interpreter is the host's), and the toolchain's `cc_wrapper.sh` | the release's binaries are glibc programs and Bazel's own actions run on the host's shell and libc; everything else they need (libstdc++, libgcc_s, zlib, libxml2, ICU, liblzma; the glibc headers and static libraries and the Linux UAPI headers the targets build against) is pinned and fetched by Bazel (`third_party/llvm/README.md`, step 7.1b) |
| `python3` | the `osv` CI job's SBOM generator (`.github/ci/osv.sh`); Bazel's own Python is hermetic | universal on build hosts |
| `coreutils` (`truncate`), `curl`, `xz-utils`, `git` | scratch-disk images, fetching, archives | universal on build hosts |

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
sudo ln -s /usr/local/bin/dcfs /sbin/mount.dcfs    # mount -t dcfs runs this
sudo ln -s /usr/local/bin/dcfs /sbin/mount.fuse.dcfs    # and this, on a remount
```

`bazel build //...` works without the test kernel: until it is built, the
kernel is a placeholder that makes any test fail fast with a pointer to
the build script.

The `dcfs(8)` man page is generated from this README: `bazel build
//man:dcfs.8` produces `bazel-bin/man/dcfs.8`; copy it to
`/usr/local/share/man/man8/`.

Format changes with `tools/format.sh` (clang-format and buildifier, if
installed) before sending them.

## Usage

```
mount.dcfs SOURCE MOUNTPOINT [-sfnv] [-N ns] -o OPTIONS
```

dcfs is the `mount.dcfs` mount helper (the dcfs binary installed under that
name: it dispatches on its own name, and run as `dcfs` it only has `--help`
and `--version`): `mount(8)` runs it for a mount of type `dcfs`, as in
`mount -t dcfs -o dcfs.fstype=none,dcfs.cache_db=/var/lib/dcfs/media.db
/srv/media /mnt/media`. SOURCE is the directory, device or export to cache,
as `mount` would take it; MOUNTPOINT is where dcfs appears (a directory: a
file is refused). `mount.dcfs -V` prints the version. `-f` checks the
options and mounts nothing; `-s` and `-v` are passed to the underlying
mount; `-N` is not supported.

`mount.dcfs` returns once dcfs answers the kernel's first request (it is
serving) or with the failure's message on its standard error and a non-zero
exit status, in `mount(8)`'s terms: 1 for a usage mistake or a refusal to run
(an unknown option, not root), 32 for a start that failed, and the native
mount's own status when mounting SOURCE failed. Relative paths (SOURCE,
MOUNTPOINT, `dcfs.cache_db`) are resolved against the working directory at
that moment. It must run as root: FUSE passthrough, the private mount
namespace and `open_tree` need `CAP_SYS_ADMIN`, `open_by_handle_at` needs
`CAP_DAC_READ_SEARCH`, and acting with each caller's credentials needs
`setfsuid`, `setfsgid` and `setgroups`; so fstab's `user` option cannot work.

A daemonized dcfs (the default) logs to syslog; see [Logging](#logging). With
`dcfs.foreground` dcfs stays in the foreground and logs to standard error
only. A comma ends an option, so values of `dcfs.fuse_opt` and
`dcfs.vmodule` cannot contain one (give `dcfs.fuse_opt` once per libfuse
option). `mount.dcfs` is also installed as `mount.fuse.dcfs`, the name
`mount(8)` looks for when it remounts a mount of type `fuse.dcfs`.

### Options

Options are not shared: every option goes to the underlying mount except
those prefixed `dcfs.`, which go to dcfs (`ro` makes the underlying mount
read-only; `dcfs.ro` makes the dcfs mount read-only). An unknown `dcfs.`
option is an error. For a type of `none`, the underlying mount is the
administrator's own.

| Option | Default | Meaning |
|---|---|---|
| `dcfs.fstype` | (autodetect) | How SOURCE is reached. `none`: SOURCE is a directory in the caller's mount namespace and dcfs serves it live (the administrator owns that mount; it stays in place and is kept busy). `bind`: SOURCE is captured with a non-recursive bind. Any other value, or none: the type of a native mount of SOURCE (`ext4`, `xfs`, `btrfs`, `nfs`, ...; absent: `mount(8)` autodetects it), made in a private mount namespace that exists only for the capture, so nothing is mounted in the caller's namespace and the backing filesystem is released when dcfs exits. The native error text and exit status pass through on failure and nothing is left mounted. |
| `dcfs.cache_db` | (required) | The SQLite cache database. Created if missing, mode 0600 (its `-wal`/`-shm` files inherit that mode too), since it holds metadata as sensitive as the source's: every cached name, attribute, xattr and symlink target, including those of directories a reader cannot list. Its directory is created mode 0700 if missing; an existing one that is group- or world-accessible logs a warning but does not stop dcfs from starting. dcfs refuses to start if the database is a symlink or not a regular file, or if it (or an existing `-wal`/`-shm`) grants more access than the source's root directory does (owner not root or that directory's owner; group or other read/write that directory does not grant); the error names both sets of permissions. A database that passes but is looser than 0600 is tightened to 0600 with a warning. Put it on an SSD, not on the backing disks, on a local filesystem: dcfs refuses to start if SQLite cannot use WAL mode there. |
| `dcfs.cache_dir` | (not yet) | Refused for now: a directory of cache databases named after the instance identity arrives with plan step 15.3; name the database with `dcfs.cache_db`. |
| `dcfs.ro` | off | Mount dcfs read-only. `mount -o remount,dcfs.ro` toggles it, and `-o remount` alone makes it read-write again, without touching the underlying mount. |
| `dcfs.foreground` | off | Stay in the foreground (debugging, tests). |
| `dcfs.fuse_opt` | (empty) | An extra mount option passed to libfuse, e.g. `dcfs.fuse_opt=max_read=65536`; give the option once per libfuse option. `default_permissions` is always added, and required: dcfs makes no permission checks of its own and relies on the kernel's, from the attributes it caches (docs/design.md, "Caller credentials"), so naming it here is an error. |

The FUSE mount's source, as `mount`, `df` and `findmnt` show it, is SOURCE as
`mount.dcfs` receives it, and its type is `fuse.dcfs`. That is SOURCE as
written (`/srv/media`, `nas:/export`), except that `mount(8)` resolves a
`UUID=` or `LABEL=` source to the device before it runs a helper: an fstab
line `UUID=aaaa /data dcfs ...` shows `/dev/vdb` (what step 15.6's systemd
guest observed), not `UUID=aaaa`.

### Flags

`dcfs --help` lists the flags and `dcfs --version` prints the version. A
flag is set as the option `dcfs.<flag>`, e.g. `dcfs.sync_interval_sec=2`.

| Flag | Default | Meaning |
|---|---|---|
| `dcfs.allow_other` | `false` | Mount with `-o allow_other`, so users other than root can use the mount. Needed for almost any real deployment, and for NFS export. |
| `dcfs.attr_timeout_sec` | `3600` | How long the kernel may cache an inode's attributes. Long by design, since dcfs has exclusive access. While a file is open for writing, its attributes are always returned with a timeout of 0. |
| `dcfs.entry_timeout_sec` | `3600` | How long the kernel may cache a lookup result, including a negative one. |
| `dcfs.sync_interval_sec` | `5` | While mutations have left dirty cache entries, the first request this many seconds after the last sync point runs a new one (`syncfs` of the backing filesystem, then the dirty set is cleared). Bounds how much is re-read after a power loss. |

dcfs uses Abseil logging, so Abseil's logging flags work too, as options with
Abseil's semantics:

- `dcfs.stderrthreshold` (default `WARNING`): Log lines at this level or above go to standard error (in a daemon: to syslog). `dcfs.stderrthreshold=0` (or `INFO`) adds the lifecycle lines: start with the source, cache and mount point, the recovery summary, each sync point with the rows it cleared and its duration, shutdown clean or unclean and why, and the first backing access after an idle period. `ERROR` hides warnings. (dcfs's default is `WARNING` where Abseil's is `ERROR`.)
- `dcfs.minloglevel` (default `0`): Lines below this level (0 INFO, 1 WARNING, 2 ERROR, 3 FATAL) are dropped everywhere, whatever the threshold.
- `dcfs.v` (default `0`): Enables verbose lines up to this level: `1` is one line per request that reached the backing filesystem, and why; `2` is every request with its reply; `3` adds the SQL statements. Verbose lines are INFO lines: also pass `dcfs.stderrthreshold=0`.
- `dcfs.vmodule` (default (empty)): Per source file verbosity, e.g. `dcfs.vmodule=backing=2`, overriding `dcfs.v` for those files.

At the default level the daemon only logs warnings and errors: errors are
what dcfs itself failed at (a request that failed inside dcfs, such as its
cache database or running out of descriptors, a backing change it could not
record, a refused start, a failed recovery probe; an errno answer such as
`ENOENT`, `ESTALE` or `EINTR` is not logged); warnings are what it noticed or survived (out-of-band changes,
recovery after an unclean shutdown, a loose cache mode).

libfuse mounts with `nosuid,nodev` by default. Pass `dcfs.fuse_opt=suid`
and `dcfs.fuse_opt=dev` if setuid binaries or device nodes on the backing
tree must work through the mount.

### Example

```
sudo mkdir -p /var/lib/dcfs /mnt/media
sudo mount -t dcfs -o dcfs.fstype=none,dcfs.cache_db=/var/lib/dcfs/media.db,dcfs.allow_other \
    /srv/media /mnt/media
```

(Where `mount` does not run mount helpers, run `mount.dcfs` itself with the
same arguments, e.g. `sudo mount.dcfs -o ... /srv/media /mnt/media`.)

The first listing of each directory reads it from the backing disk and
records everything about its entries (attributes, file handle, symlink
target, xattrs). After that, `find /mnt/media -ls` or `ls -lR` reads
nothing from the backing disk, also after dcfs is restarted. Opening a
file for reading or writing does spin the disk up.

### Remounting

`mount -o remount,dcfs.ro /data` makes the dcfs mount read-only, and a
remount without `dcfs.ro` makes it read-write again. That is all a remount
does: it never reaches the underlying filesystem and it does not restart or
reconfigure the daemon, so it keeps the mount's `nosuid`, `nodev`, `noexec`,
`noatime` and `nodiratime` as they are and ignores the rest.

- Native options in a remount (`ro`, `noatime`, `vers=4.2`, ...) are
  ignored with a WARNING that names them (`a remount of dcfs changes only the
  dcfs mount (dcfs.ro); ignoring the underlying mount's options ro,
  noatime`), because mount(8) may merge the options of the matching fstab
  line into the ones you gave. What mount(8) itself consumes (`rw`,
  `defaults`, `nofail`, `_netdev`, `noauto`, `auto`, the `user` family and
  `x-*` options) is not reported.
- The other `dcfs.` options (`dcfs.allow_other`, `dcfs.cache_db`, the
  timeouts) are checked and ignored without a message: to change them,
  unmount and mount again.
- A mount point that is not a dcfs mount is refused with an error and left
  alone.

To change the underlying filesystem's options, unmount and mount again with
the new options. With `dcfs.fstype=none` the underlying mount is the
administrator's own and can be remounted directly (`mount -o remount,ro
/srv/media`): dcfs then sees the filesystem go read-only and its writes fail
with `EROFS`. That needs a mount that is not over-mounted by dcfs itself, so
that it stays reachable.

### Logging

A daemonized dcfs has no standard input, output or error (all `/dev/null`)
and logs to syslog under the identity `dcfs`, facility `daemon`, with its
pid. Abseil's severities map to syslog's `info`, `warning`, `err` and
`crit`. Without a syslog daemon the messages are dropped. Under systemd the
journal collects them: `journalctl -t dcfs` shows them.

`dcfs.stderrthreshold` is the one knob for both destinations: in a daemon it
is the level at or above which a message goes to syslog (default `WARNING`,
so only warnings and errors), and `dcfs.stderrthreshold=0` adds the INFO
lifecycle lines. `dcfs.minloglevel` still drops everything below its level.
`dcfs.v` and `dcfs.vmodule` only enable verbose lines, which are INFO lines,
so they need `dcfs.stderrthreshold=0` too to be seen; at `dcfs.v=2` or more
the volume is that of every request, which is a lot for syslog.

Mistakes found before the daemon forks (a bad option, not root) and a
failure to start after it are printed by `mount.dcfs` itself on its standard
error, in the terminal or in the journal of whatever ran `mount`, and
reported in its exit status; they are not repeated in syslog. With
`dcfs.foreground` there is no syslog at all: the messages go to standard
error.

### Mounting over the source directory

Mounting dcfs on the directory it caches is supported, with `none` and with
`bind`:

```
sudo mount -t dcfs -o dcfs.fstype=none,dcfs.cache_db=/var/lib/dcfs/media.db,dcfs.allow_other \
    /srv/media /srv/media
```

This works because dcfs opens SOURCE before mounting and never uses a
path again: every later access goes through that descriptor or through
file handles stored in the cache. Everything that used `/srv/media` before
now goes through dcfs and cannot bypass it by accident, which makes this
the easiest way to honour the exclusive-access requirement (see
[Limitations](#limitations)). With `dcfs.fstype=bind` the same holds for a
captured bind of the directory. Unmounting dcfs brings the original
directory back, and from then on it is reachable without dcfs.

With `none`, the mount that holds SOURCE belongs to the administrator and
dcfs keeps it busy: it cannot be unmounted until dcfs is.

With a native type (`UUID=...`, `nas:/export`, a device) there is nothing to
cover: the filesystem is mounted in a private mount namespace that exists
only while dcfs starts, so it never appears at any path in the caller's
namespace, and the dcfs mount point can be where you would otherwise have
mounted the filesystem itself. The filesystem is released when dcfs exits,
however it exits.

### Instances, trees and boundaries

Run one dcfs per backing filesystem: each ext4 or xfs filesystem, each
btrfs subvolume, each NFS export. One filesystem's cache can then be wiped,
or its daemon restarted, without touching the others. There is no recursive
bind and no instance that serves a second filesystem under its source.

A tree is a set of instances mounted over each other's directories: mount
the parent, then mount the child on an (empty) directory inside the
parent's mount, naming the child's own device or export, not a path inside
the parent:

```text
UUID=aaaa  /data      dcfs  dcfs.cache_db=/var/lib/dcfs/data.db      0 0
UUID=bbbb  /data/sub  dcfs  dcfs.cache_db=/var/lib/dcfs/data-sub.db  0 0
```

The parent never sees the child, because the filesystem it serves is a
private mount of its own. The parent must be mounted before the child (an
earlier fstab line does that for `mount -a`; systemd orders mount units by
path) and `umount -R /data` unmounts the child first.

`dcfs.fstype=none` and `dcfs.fstype=bind` serve a directory of the caller's
mount namespace, so a filesystem mounted below SOURCE would put two
filesystems under one `st_dev`. dcfs refuses to start in that case, whether
the mount is a directory or a file, and the error names the mount points
found: unmount them, or choose a SOURCE below them.

A mount or btrfs subvolume that appears below SOURCE after dcfs started is
a **boundary**: dcfs shows it as an empty stub directory, usable as a mount
point, logs an ERROR once for it, and answers anything inside it with
`ENOTSUP` (rename or link across it with `EXDEV`). See
[Limitations](#limitations).

### Running under systemd

dcfs mounts from `/etc/fstab` like any other filesystem, with or without
systemd: the filesystem type is `dcfs`, SOURCE is the first field, and the
options are mount options. There is no unit file to install and no service
that runs dcfs in the foreground; the daemon forks away from `mount.dcfs`
and `umount` ends it.

```text
# SOURCE        MOUNTPOINT  TYPE  OPTIONS                                    DUMP PASS
UUID=aaaa-aaaa  /data       dcfs  noatime,dcfs.cache_db=/var/lib/dcfs/data.db,dcfs.allow_other                   0 0
UUID=bbbb-bbbb  /data/sub   dcfs  dcfs.fstype=xfs,dcfs.cache_db=/var/lib/dcfs/sub.db,dcfs.allow_other            0 0
UUID=cccc-cccc  /data/vol   dcfs  dcfs.fstype=btrfs,subvol=vol,dcfs.cache_db=/var/lib/dcfs/vol.db                0 0
nas:/export     /srv/nas    dcfs  dcfs.fstype=nfs,vers=4.2,_netdev,nofail,dcfs.cache_db=/var/lib/dcfs/nas.db     0 0
/srv/raw        /cache/raw  dcfs  dcfs.fstype=bind,dcfs.ro,dcfs.cache_db=/var/lib/dcfs/raw.db                    0 0
/mnt/disk       /mnt/fast   dcfs  dcfs.fstype=none,dcfs.cache_db=/var/lib/dcfs/fast.db                           0 0
```

- **`dcfs.fstype`** says how SOURCE is reached. Absent: `mount(8)` finds the
  type of the device itself, as it does for a plain `mount`. A native type
  (`ext4`, `xfs`, `btrfs`, `nfs`, `fuse.sshfs`, ...): that type, with the
  other options handed to the native mount (`subvol=vol`, `vers=4.2`).
  `bind`: SOURCE is a directory of the running system, captured with a
  non-recursive bind. `none`: SOURCE is a directory dcfs serves live,
  usually the mount point of a filesystem mounted by an earlier fstab line.
  See the [Options](#options) table.
- **`dcfs.cache_db` is required** on every line, one database per mount
  (never shared: dcfs refuses a second instance on a database that is in
  use). The database's directory is created mode 0700 if it does not exist,
  but only that one level, and on whatever filesystem holds it at that
  moment: if the cache lives on a filesystem of its own, mount that first.
- **Options are not shared**: everything not prefixed `dcfs.` goes to the
  underlying mount (`noatime`, `subvol=vol`, `vers=4.2`), and `ro` makes
  that mount read-only while `dcfs.ro` makes the dcfs mount read-only.
  `defaults`, `rw`, `noauto`, `nofail`, `_netdev` and `x-*` options are
  accepted on every form, `none` included, since mount(8) adds or reads them
  itself; any other native option on a `none` line is an error, because dcfs
  makes no underlying mount to give it to.
- **The sixth field** (the fsck pass) is `0`: dcfs provides no `fsck.dcfs`,
  so nothing checks the filesystem at boot. To check the filesystem under a
  dcfs mount, run `fsck` on its device while it is unmounted.
- **A line is a mount**: `mount /data`, `umount /data`, `mount -a` and
  `umount -R /data` work as for any fstab entry, and `mount /data` takes
  the line's options. Write absolute paths in the line.

Network backings need `_netdev`. systemd does not know that a `dcfs` mount
of an NFS export is a network mount: `_netdev` makes it wait for the network
and keeps it out of local-fs.target. Add `nofail` to a mount the machine can
boot without, and `noauto` to one that should not mount at boot.

**Without systemd**, `mount -a` (from the init scripts, or by hand) mounts
the lines in file order, so put parents before the children mounted inside
them. Restart one instance with `umount /data/sub && mount /data/sub`, and
unmount a tree with `umount -R /data`.

**With systemd**, `systemd-fstab-generator` turns each line into a mount
unit at boot and at `systemctl daemon-reload`; `systemd-escape -p
--suffix=mount /data/sub` prints a unit's name (`data-sub.mount`). The unit
is finished when `mount.dcfs` exits, which it does once dcfs answers the
kernel's first request, so units that depend on the mount start when dcfs is
serving. A mount unit requires and orders after the mount units of the
directories above its mount point, so parents mount first and stopping a
parent stops its children. systemd does not see what a line needs besides its
mount point: a `none` or `bind` SOURCE that is itself a mount, and a cache
database on another filesystem, need `x-systemd.requires-mounts-for=` on the
line (for example `x-systemd.requires-mounts-for=/var/lib/dcfs`).
Restart one instance with `systemctl restart data-sub.mount`.

A mount that fails prints its message on the standard error of whatever ran
`mount` (the journal, under systemd) and exits with a status from the list
under [Usage](#usage); the mount unit then fails with it, and nothing is left
mounted.

### Responsibilities of the administrator

dcfs trusts what it has cached, so the setup has to keep it true.

- **Each instance has exclusive access to what it serves.** Nothing else
  writes to the backing tree: no other writer on the machine, nothing writing
  to a network export from elsewhere, and no second dcfs over the same
  objects (instances on different filesystems are fine; two on one
  directory, or on overlapping directories of one filesystem, are not).
  dcfs does not try to detect overlapping instances, except that it will
  not open a cache database that another dcfs holds. Mounting dcfs over its
  source (see above) keeps local writers honest.
- **Out-of-band changes are unsupported.** If one happens anyway, see
  [Operations](#operations).
- **The cache database is yours to place and protect**: on a local SSD
  filesystem that supports SQLite's WAL mode, not on the backing disks, with
  a directory only root can read (dcfs checks the permissions and warns).
  A database belongs to one instance: dcfs refuses one built for a different
  filesystem or source directory.
- **Mount order and dependencies**: parents before children, the cache's
  filesystem before the instance (above).
- **Why dcfs runs as root and refuses otherwise**: FUSE passthrough, the
  private mount namespace, `open_tree` and the filesystem-identity ioctls
  need `CAP_SYS_ADMIN`, `open_by_handle_at` needs `CAP_DAC_READ_SEARCH`, and
  serving each caller with that caller's credentials needs `setfsuid`,
  `setfsgid` and `setgroups`. `mount.dcfs` as a normal user exits with
  status 1 and says so; fstab's `user` option cannot work.
- **NFS**: see [Exporting over NFS](#exporting-over-nfs).

### Exporting over NFS

nfsd needs `dcfs.allow_other`, and a FUSE filesystem needs an explicit `fsid=`
in its export; each exported dcfs mount needs its own. An `/etc/exports`
line:

```
/mnt/media  192.168.1.0/24(rw,fsid=1,no_subtree_check)
```

NFS clients keep their handles across dcfs restarts. After restarting an
instance (unmounting and mounting it again), though, run

```
sudo exportfs -f
```

nfsd's export cache holds a reference to the old mount and answers `EIO`
until it is flushed. This is ordinary NFS administration for any
filesystem that is unmounted and remounted under an export.

If the cache database is deleted, handles held by NFS clients become stale
(`ESTALE`); they never resolve to a different file.

### Shutdown, crashes and restarts

`SIGTERM`, `SIGINT` or `SIGHUP`, and an unmount of its mount point (so
`umount` and `systemctl stop` of its mount unit), shut dcfs down
cleanly: it unmounts, syncs the backing filesystem, empties the dirty set,
checkpoints the SQLite WAL, records a clean shutdown and exits 0. If a
file was still open for writing (possible after a lazy unmount), dcfs
leaves the clean-shutdown flag unset so that the next start re-reads what
that file's writes may have changed.

If dcfs stops without a clean shutdown (a crash, `SIGKILL`, a kernel crash
or a power loss), the next start notices, logs a warning with the number
of affected entries, and forgets everything cached about the entries
changed since the last sync point, so that they are re-read from the
backing filesystem. Inode rows are kept, so NFS handles keep working,
except after a power loss or kernel crash: the cache database makes a
durable commit at the first change through dcfs since the last sync point,
at a WAL checkpoint and at a start, and rows recorded after it (by
lookups, listings, and the objects a change creates) are lost, with their
NFS handles. Those fail with `ESTALE`; a nodeid handed out again gets a
fresh random generation, so an old handle resolves to a different file
only with probability 2^-32 (docs/design.md, "Generations").

After a crash the dead FUSE mount stays in place, and accessing it fails
with `ENOTCONN`. Unmount it (`umount -l <mountpoint>`) before mounting dcfs
there again; `mount.dcfs` does not do this for you. When dcfs is mounted over
its own source this is mandatory, since dcfs would otherwise try to open the
dead mount as its source. A filesystem that dcfs captured itself (any
`dcfs.fstype` but `none`) needs no cleanup: it is released when the daemon
dies, however it dies.

Only one dcfs may use a cache database at a time: dcfs takes an exclusive
lock on it at startup and refuses to start if another process holds it.
dcfs also refuses a database that was built for a different filesystem or
a different source directory; delete it to start with a cold cache.

## Operations

### Wiping the cache of one instance

Each instance has its own database, so wiping one touches nothing else.
Unmount the instance (and anything mounted inside it: `umount -R /data/sub`),
delete the database with its `-wal` and `-shm` files, and mount it again:

```bash
sudo umount -R /data/sub
sudo rm -f /var/lib/dcfs/sub.db{,-wal,-shm}
sudo mount /data/sub
```

The next start has a cold cache: the first listing of each directory reads
the backing disk again. Nothing is lost on the backing filesystem. NFS
clients holding handles from before the wipe get `ESTALE`, never a
different file; after the mount run `sudo exportfs -f` if the instance is
exported. A running instance holds its database open and locked, so wipe it
only while it is unmounted.

### After an out-of-band change

dcfs assumes nothing else changes the backing tree. If something did (a
restore, an administrator working on the raw filesystem, another machine
writing to an NFS export), dcfs notices only what a request it makes anyway
reveals, logs a WARNING saying `out-of-band change on the backing filesystem
(unsupported)` and adopts what it sees. Anything it answers from the cache
alone stays stale, and the kernel keeps serving the attributes and names it
already holds until their timeouts (an hour by default) expire. A restart
does not make dcfs re-read: a cleanly stopped instance trusts its database.

To be sure, stop the change from recurring, then wipe that instance's cache
as above. Unmounting also drops the kernel's caches for it. If the change
was one you can name (a few files), the warnings in the log list the inodes
dcfs adopted; wiping is still the only way to know that it saw everything.

### Upgrading dcfs

A running dcfs keeps running the binary it started from, so installing a
new one changes nothing for the mounts that exist: the upgrade takes effect
for an instance when that instance is unmounted and mounted again.
`mount -o remount` is not that: it only toggles the dcfs mount's read-only
state (see [Remounting](#remounting)) and leaves the daemon in place.

```bash
sudo install -m 0755 bazel-bin/dcfs/main_static /usr/local/bin/dcfs.new
sudo mv /usr/local/bin/dcfs.new /usr/local/bin/dcfs
sudo umount -R /data && sudo mount -a      # or per instance, parents first
```

`/sbin/mount.dcfs` and `/sbin/mount.fuse.dcfs` are symbolic links to the
binary (see [Building](#building)) and need no change. Unmounting shuts the
daemon down cleanly (see "Shutdown, crashes and restarts"), so the cache is
kept; a new version upgrades the database's schema at its first start, and
refuses a database whose schema is newer than it knows (after a downgrade,
delete the database). Exported instances need `sudo exportfs
-f` afterwards (see "Exporting over NFS"). Programs using the mount have to
be stopped, or the unmount fails with `EBUSY`.

### What the log messages mean

The messages an administrator can see at the default level
(`dcfs.stderrthreshold=WARNING`), by their opening words. Errors are what
dcfs itself failed at; warnings are what it noticed or survived.

| Message | What it means and what to do |
|---|---|
| WARNING `inode N: out-of-band change on the backing filesystem (unsupported): ...; adopting the new attributes` | Something changed the backing tree behind dcfs and dcfs read the new state. Find the writer and stop it; to resynchronise everything, see "After an out-of-band change". |
| WARNING `... its handle now reaches a different object ...; forgetting it (ESTALE)` | The object a cached row names was replaced behind dcfs (an inode number reused). Clients holding it get `ESTALE`. Same cause and remedy as above. |
| ERROR `refusing to cache NAME under inode N: it is a mount point or subvolume boundary; it is shown as an empty stub directory...` | A filesystem was mounted, or a btrfs subvolume exists, below the source of a running instance. The name is a stub that returns `ENOTSUP` inside. Mount a second instance on the stub, or remove the mount. Once per stub. |
| ERROR `refusing OP on or inside the boundary stub ...` | A process used a stub (listing or creating inside it). Same remedy. |
| WARNING `the last run did not shut down cleanly (... a crash or power loss / the daemon died; same boot); recovered N dirty cache entries` | At start: the previous run ended without its clean-shutdown record. dcfs forgot what it had cached about the entries changed since the last sync point; they are re-read as needed. Nothing to do unless it recurs; then find out why the daemon dies (the kernel log, the syslog before the line). |
| ERROR `could not probe recovered inode N ...` | During that recovery the backing filesystem could not be read for an entry (printed for the first ten). The entry stays dirty and is re-checked on access. Check the backing filesystem's health. |
| WARNING `forgot N rows of objects removed by mutations the last run's end cut short` | Recovery housekeeping after a crash; informational. |
| WARNING `periodic sync of the backing filesystems failed, keeping the dirty set: ...` (also `fsync sync ...` and `fsyncdir sync ...`) | `syncfs` of the backing filesystem failed: the filesystem has errors, went read-only or is gone. Nothing is lost in dcfs (the entries stay dirty and cost a larger re-read after a crash). Look at the kernel log; it is retried at the next request past the interval. |
| WARNING `clean shutdown incomplete, the next start will recover the dirty set: ...` | The last sync at shutdown failed (same causes as the line above). The next start recovers by itself. |
| ERROR `OP: the backing change happened, but recording it in the cache failed; leaving it unknown: ...` | The change is on the backing filesystem but the cache database could not record it (disk full or I/O error on the cache's disk). The entry is re-read later. Free space or fix the cache's disk. |
| ERROR lines that are a bare status (`INTERNAL: ...`, `RESOURCE_EXHAUSTED: ...`), logged once by the request that failed | A request failed inside dcfs, such as the cache database or running out of descriptors; the caller got an error. Errno answers like `ENOENT`, `ESTALE` and `EINTR` are not logged. The message carries the cause. |
| WARNING `could not raise RLIMIT_NOFILE ...` or `holding no descriptors on written files ...` | dcfs could not raise its open-file limit, or the limit is 16,384 or less, so it holds no descriptors on written files. It works, but the re-read of a written file's attributes waits for the next access and reads the disk (see the shared writable `mmap` limitation). Raise the hard open-file limit of the environment that runs `mount`. |
| WARNING `cache database directory ... is group- or world-accessible` / `... was mode 0644; tightened to 0600` | The cache holds metadata as sensitive as the source. `chmod 700` the directory; the file is fixed by dcfs. |
| WARNING `a remount of dcfs changes only the dcfs mount (dcfs.ro); ignoring the underlying mount's options ...` | A remount named native options; they were ignored. See [Remounting](#remounting). |
| WARNING `pid N: supplementary groups unreadable ...; using none` | A request came from a process whose groups dcfs could not read; it was checked without them and may have got `EACCES`. At most once a minute. |

A start that fails prints its reason on the standard error of `mount`, with
exit status 32 (or the native mount's own, or 1 for a usage mistake). The
common reasons:

- `Cache database X is in use by another dcfs process`: another instance has
  that database; two daemons cannot share one.
- `Cache database X was created for filesystem A, but SOURCE is on B; delete
  the database to start a cold cache`, and `... for a different source
  directory ...`: the database belongs to another filesystem or directory
  (wrong `dcfs.cache_db`, or the source was replaced); delete it only if the
  new source is the intended one.
- `dcfs does not yet support filesystems mounted below SOURCE`: unmount them
  or choose another SOURCE.
- `... is on a filesystem whose superblock is read-only under a read-write
  mount`: the filesystem went read-only after an error; unmount, check and
  mount it again.
- `dcfs.cache_db is required`.

## Testing

Every test, unit tests included, runs as root inside a QEMU guest booted
from the project's own minimal kernel. There is no host-side test
execution: dcfs needs root, `open_by_handle_at`, `FS_IOC_GETFSUUID` and
FUSE passthrough, none of which a development host can be assumed to have.
Guests boot in about a second with KVM (QEMU `microvm`, direct kernel
boot), so `bazel test //...` is dominated by compilation, not booting.
`test/qemu/README.md` has the full details.

### One-time setup

1. Install `truncate` (coreutils; creates the scratch-disk images). QEMU,
   the mkfs tools, its qboot firmware, busybox and the test kernel are all
   Alpine packages of one pinned release branch, fetched by Bazel and checked
   against Alpine's signing keys (`//third_party/alpine`,
   `//third_party/linux`); there is no host QEMU,
   qboot, busybox or manual kernel build step any more -- see
   `test/qemu/README.md`.
2. Get write access to `/dev/kvm`: `sudo usermod -aG kvm "$USER"`, then log
   in again. KVM is optional: without it QEMU falls back to software
   emulation (TCG), 2-9x slower (`DCFS_FORCE_TCG=1` forces this).
3. For `nfs_test` only, the small Debian root image it chroots into is
   built by Bazel from a pinned package set (`third_party/debian/scripts/
   mkrootfs.py`, with Alpine's `mke2fs` and `debugfs`; no host tool, no
   root):

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
sg kvm -c 'bazel test --config=ubsan //dcfs:all'     # under UBSan
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
  `readdir_boundary_test`, `removed_test`, `boundary_test`, `names_test`,
  `nfs_test` and `boot_test`.
  [`docs/design.md`](docs/design.md#test-strategy) says what each one
  proves.
- **POSIX conformance**: `pjdfstest_test` runs all of pjdfstest (about 8800
  checks, as root and as unprivileged users), once through dcfs and once
  directly on the same ext4 filesystem, and fails on any dcfs-specific
  failure. Today both runs fail the same 28 checks. See
  `docs/conformance.md`. This test is slow: it runs as three guests per
  filesystem (shards by test directory), each a few minutes.

Bugs get a regression test first: the test is shown to fail on the
unfixed code, then the fix makes it pass.

### Reproducible build

`bazel run //tools:reproducible_build` builds `//dcfs:main`,
`//dcfs:main_static` and `//man:dcfs.8` from two copies of the repository at
different paths, in two Bazel output bases (one with its own repository
contents cache) and without a disk cache, and fails unless the outputs are
byte-identical (it prints the strings that differ). CI runs it as the
`reproducible` job. The binaries link a pinned glibc and Linux headers (the
Debian sysroot of `@dcfs_llvm`, step 7.1b) and the compiler runs on pinned
libraries; the host's glibc, which runs clang and lld, is what is left of the
host, so the comparison should hold across hosts as well (only one host has
been measured).
They are identical with nothing special: the
toolchain redacts `__DATE__`/`__TIME__`, compiles with paths relative to the
execroot, and the man page carries no date.

### Coverage

```sh
bazel coverage --config=presubmit //...      # or a single test: bazel coverage //dcfs:status_test
```

Clang's source-based coverage (the pinned toolchain's `coverage` feature) of
our code (`dcfs/`, `bench/`, `tools/`; not the tests and not external
repositories), unit and end-to-end tests alike. The combined lcov report is
`bazel-out/_coverage/_coverage_report.dat` (`$(bazel info output_path)/_coverage/`);
a test's own is `bazel-testlogs/<package>/<test>/coverage.dat`. Render one
with `genhtml` if you want a browsable view. How it works: the guest runs the
instrumented binaries, which write `.profraw` files to `/cov`; `guest/init`
tars them onto an extra virtio disk, `run-qemu.sh` merges them
(`llvm-profdata`) and exports lcov (`llvm-cov`) into Bazel's `COVERAGE_DIR`
(`test/qemu/README.md`, "Coverage"). The profiles are written in LLVM's
continuous mode (`%c`, counters mapped into the profile file), so a daemon
that a test SIGTERMs or SIGKILLs still leaves a complete profile; only a guest
that is itself cut off (a power-cut test ending the VM) loses its processes'.
The report covers the small and medium tiers only (`--config=presubmit`):
the large and enormous tests, and the trace-validation tests, are not in it.
CI publishes it as the `coverage-lcov` artifact (dcfs, bench and tools only);
the gate below runs only there. The first baseline (small and medium tiers): all
of `dcfs/*.cc` 92.4% of lines and 73.3% of branches.

The gate (step 8.1, `tools/coverage_gate.sh`, run by `.github/ci/coverage.sh`
in the CI coverage job only, not in the local tiers): `dcfs/*.cc` line and
branch coverage of the published report must not fall below the committed
baseline `dcfs/coverage_baseline.txt` (`lines 92.40`, `branches 73.30`, two
decimals). Below it on either, the job fails with the numbers. Above it by 0.1
or more on either, the job passes and prints (and adds to the job summary) a
note, `raise dcfs/coverage_baseline.txt to X / Y`: the baseline only moves up,
in a later commit, and a rise never turns a push red (0.1 is the tolerance for
noise). `bench/` and `tools/` (`fhtest.c`
and `testutil.c` are test helpers) are printed, not gated. Self-check:
`//tools:coverage_gate_self_check_test` runs the gate over canned lcovs below,
equal to, within tolerance of and above a canned baseline (above passes with
the note).

## Continuous integration

`.github/workflows/ci.yml` runs the whole suite on GitHub Actions
(`ubuntu-24.04` runners) in the jobs below, the tiers of `test/qemu/README.md` and
the two sanitizer suites:

| Job | Runs | Needs |
|---|---|---|
| `subjects` | every commit subject of the push or pull request starts with a plan step (`.github/ci/commit_subjects.sh`); seconds, no cache, gates nothing | |
| `fast` | `bazel test --config=fast //...` (small tests) | |
| `presubmit` | `bazel test --config=presubmit //...` (small and medium) | `fast` |
| `coverage` | `bazel coverage` of the small and medium tests, the lcov artifact and the coverage gate | `fast` |
| `reproducible` | two builds of the shipped outputs in two output bases are byte-identical | `fast` |
| `mutation-changed` | mutation testing of the protocol functions the push touched (at most 30 mutants); fails on a survivor | `fast` |
| `full` | the large and enormous tests (pjdfstest on all three filesystems), in 3 shards | `presubmit` |
| `asan` | `bazel test --config=asan` over every tier, in 3 shards | `presubmit` |
| `ubsan` | `bazel test --config=ubsan` over every tier, in 3 shards | `presubmit` |

`full`, `asan` and `ubsan` run in parallel, nine runners in all, each with its
own cache key and a time limit (180 minutes; `asan` 240), so no suite's length bounds the others.
A shard is a deterministic partition of the suite's test targets
(`.github/ci/test.sh --shard=I/N`, dealt out by `.github/ci/shard.sh` over
the sorted list, size by size, so each shard gets its share of the slow
tests; `//tools:shard_test`), passed to Bazel as explicit targets, so the
test-result cache applies per shard. The sanitizer shards skip the host-only
tests (`HOST_ONLY_COMPATIBLE`, `test/qemu/README.md`), which leaves 156 test
targets, 52 per shard. `full`'s 34 large and enormous targets are 12, 11 and
11 per shard (small and medium are `presubmit`'s). The targets of step 11.2
fall as follows (`.github/ci/test.sh --shard=I/3 --list` prints a shard):

| Shard | `full` (large, enormous) | `asan` (every tier) |
|---|---|---|
| 0 | `fault_ace_fs_test_btrfs`, `fault_power_kill_test_xfs`, destroy, nfs, pjdfstest on xfs | `fault_ace_a_test`, `fault_ace_fs_test_xfs`, the ext4/xfs/btrfs fault tests (some) |
| 1 | `fault_ace_a_test`, `fault_ace_fs_test_xfs`, idle_long, pjdfstest on btrfs | `fault_ace_b_test`, `fault_power_kill_test_btrfs`, `fault_power_kill_test_ext4` |
| 2 | `fault_ace_b_test`, `fault_power_kill_test_btrfs`, bench_full, formal large, pjdfstest on ext4 | `fault_ace_fs_test_btrfs`, `fault_power_kill_test_xfs` |

Expected wall time per shard (2026-10-08: the serial sum of the targets'
measured times on a shared 4-core machine at two test jobs, divided by 1.5 for
the runner running two guests at a time; the sanitizer builds are cold in every
run: fetch 24 min and build 20 min, measured on GitHub): `full` 46, 52 and
55 min of tests serial, so 31, 35 and 37 min, 80 min with the cold part;
`asan` 61, 62 and 46 min serial, 2.5 to 3 times under ASan except the ACE and kill tests of step 11.2, which take the same under ASan as plain (`fault_ace_fs_test_xfs`: 227 s under ASan, 199-247 s plain), so 89-105, 96-114 and 71-84 min, and 116-159 min with the cold part, against a limit of 240 min; `ubsan` about 1.5 times plain, 60 min and 105 with
the cold part, limit 180. ACE target a (568 s plain, 182 sequences) is the
longest single test of its shard after `bench_full` (761 s) and `idle_long`
(691 s). Each shard uploads `test-logs-<job>-<shard>` on failure. Dealing by
index modulo 3 balances the counts, not the time: a fourth `asan` shard would
cut its wall to about 120 min at the price of another cold 45 min of runner
time, but a cost-weighted deal (a file of measured times) would balance better
than another shard; neither is done until a `cold` run measures the shards.

- **Caches.** Bazel's disk cache, repository cache and Bazelisk's download
  are restored and saved with `actions/cache`, even when tests fail (the
  kernel build takes half an hour). The disk cache is content addressed, so
  a pin change invalidates only the actions whose inputs changed; the keys
  end in the commit and fall back to the newest entry of the same job. The
  repository cache (the downloaded archives) has its own key, the hash of
  `MODULE.bazel.lock`; the extracted repositories (13 GB) are never saved.
  The coverage, reproducible and mutation jobs save no disk cache.
- **Cold runs.** Caches are evicted (10 GB per repository, seven days idle)
  and a lock-file change starts from nothing, so every job's limit covers
  the cold path. Measured on this 4-core machine at load 18 (2026-10-07):
  fetching every repository with empty caches took 24 minutes (2.4 GB
  downloaded, 13 GB extracted, the LLVM tarball being 1.9 GB of it), our C++
  build about 20 minutes, a `//dcfs:main_static` build alone 6 to 10; the
  limits in `ci.yml` are twice those estimates, with the estimate and its
  date beside each. `workflow_dispatch` has a `cold` input: with it every
  job restores and saves no cache, to run the cold path on purpose (the
  `mutation-changed` job, which needs a push range, is skipped on dispatch).
- **Where the time goes.** Every job that runs Bazel at length writes its JSON
  trace profile (`--profile`) and uploads it, success or failure, as the
  `bazel-profile-<job>[-<shard>]` artifact: `fast`, `presubmit`, `coverage`,
  `osv` (its fetch), `reproducible` (one profile per build) and each shard of
  `full`, `asan` and `ubsan`. The test jobs add Bazel's compact execution log
  (`<job>.execlog.zst`; 0.9 MB for a warm fast-tier run, not measured cold).
  `mutation-changed` and the weekly mutation run start a Bazel per mutant and
  write none. `tools/ci_profile.py` (`bazel run //tools:ci_profile --
  <profiles>`; unit tested) sums a profile into total wall, repository
  fetches, the `@dcfs_llvm` extraction, third-party builds, our compile and
  link, test execution and the critical path, and `.github/ci/profile.sh`
  appends that table to the job's summary, so a run shows the breakdown
  without a download. To measure the cold path, dispatch the workflow with
  `cold` set (above) and read the tables.
- **KVM.** `.github/ci/prepare.sh` makes `/dev/kvm` usable if the runner has
  one (public repositories' standard Linux runners do; private ones do
  not) and the tests log which accelerator they used. Without KVM the tests
  run under TCG: `bazel test` gets longer timeouts (`--test_timeout`: 300,
  1800, 3600, 7200 s for short, moderate, long, eternal; Phase 5.1 measured
  pjdfstest on ext4 at 3502 s of the default 3600 s) and pjdfstest runs on
  ext4 only (about 3500 s per filesystem under TCG, and xfs and btrfs have
  the other tests' variants); `.github/ci/test.sh` does both.
- **Failures** upload every test's `test.log`, `test.xml` and `test.outputs/`
  (the guest's serial console) as the `test-logs-<job>` artifact.
- **Kernel matrix: not yet.** Every test boots Alpine's `linux-virt` of the
  one pinned Alpine branch (`//third_party/linux:vmlinuz`, series 6.18 on
  v3.24; CI fetches the branch's current build on each run). Testing the
  minimum supported kernel (6.9) too needs a second kernel (its options
  newer than 6.9, `FUSE_IO_URING` is 6.14, must be absent) and a Bazel flag
  choosing the kernel in the `qemu_test` macros: `third_party/linux/README.md`.
- **Host tools.** `.github/ci/prepare.sh` installs the packages of
  [Host requirements](#host-requirements) explicitly and the pinned
  Bazelisk (sha256-checked).

### Running CI locally with act

The workflow is developed with [nektos/act](https://github.com/nektos/act),
which runs its jobs in containers that resemble GitHub's runners, so it can
be iterated on before anything is pushed. `act` and the runner image are
pinned (`third_party/act/README.md`):

```
bazel run //third_party/act -- -j fast        # also presubmit, full
```

The job container gets `/dev/kvm`, so the QEMU tests run there as on a
hosted runner. Because the container has only what a fresh runner has, a
build or test that quietly depends on this machine's tools fails there:
running the `full` job under `act` is how the host requirements above were
found. Run it again after adding a tool or a test.

### Dependency vulnerability scanning (OSV-Scanner)

The `osv` CI job (every push to `main`, every pull request, and weekly on
Mondays, so new advisories reach unchanged pins) scans with the pinned
`osv-scanner-action` (`tools/sbom/README.md`). Only what the dcfs binaries
**ship** gates the job; everything else is scanned for information.

**Gated: the shipped dependencies**, the external repositories of the Bazel
dependency graph of `//dcfs:main` and `//dcfs:main_static`
(`//dcfs:linked_deps`; `bazel test //tools/sbom:sbom_test` fails when a
linked repository has no entry in `tools/sbom/pins.json` or a test-only one
is linked): abseil-cpp, gloop (a dependency of abseil-cpp), SQLite, libfuse,
liburing and numactl (libfuse's), each pinned with the upstream git commit of
its release tag (`sbom.py verify-commits`, run by the job, checks the tag
still points at that commit). The binaries also contain the pinned
toolchain's static C++ runtime (libc++, libc++abi, libunwind, compiler-rt's
builtins from llvm-project 22.1.8), listed under `toolchain_runtime` in
`pins.json`, and glibc, linked statically from the toolchain's Debian sysroot
(`libc6-dev`, under `shipped_debs`): OSV matches it by Debian package, so the
job also scans `shipped-debs.cdx.json`, and that scan gates too. The
sysroot's glibc is Debian 13's (trixie, 2.41-12+deb13u4, step 7.1c); OSV holds
21 records against it, none fixed in trixie (10 are fixed in Debian 14 only,
11 in no release), and `osv-scanner.toml` ignores each with a reason and an
expiry 90 days out (2027-01-06), so a new record fails the job. The job
fails on any finding in the git-commit scan that `osv-scanner.toml` does not
ignore; an ignore needs a reason and an expiry date, and an expired or
unexplained ignore fails the job. A self-check step scans a deliberately old
libfuse (3.2.0, CVE-2018-10906; `tools/sbom/testdata/`) with the very same
invocation and fails the job if the scanner reports nothing.

**Informational, never gating: the test-only dependencies** (the Debian
test image, kernel, QEMU, busybox, e2fsprogs, xfsprogs, btrfs-progs,
util-linux, urcu, inih, dtc, pjdfstest, googletest, google_benchmark,
TLA+ tools, act, Bazelisk, the Bazel rule sets). Their SBOM
(`testonly.cdx.json`) is scanned and the findings are printed in the job log,
but the step cannot fail the job.

Run it with `bazel run //third_party/act -- -j osv`.

How OSV matches each shipped project (observed with osv-scanner v2.6.0 and
`api.osv.dev`, 2026-10-06). OSV has no package ecosystem for C/C++
libraries: a `pkg:github/...` or `pkg:generic/...` purl in an SBOM, even with
a commit as its version, reports nothing. What OSV does hold for these
projects are advisories with `GIT` ranges (a repository URL and the commits
that introduced and fixed the bug), matched against a commit. osv-scanner
takes a commit only from a git root, so the job writes one detached git root
(`.git/HEAD` naming the commit, nothing else) per shipped project and scans
those with `scan source --include-git-root`. Proof that this path works:
libfuse 3.2.0 (commit `cfdca8c6...`) is reported as CVE-2018-10906, and
libfuse 3.18.2, the pinned one, is not. What OSV holds per project:

| Shipped project | Matched by | OSV advisories for it today |
|---|---|---|
| libfuse 3.18.2 | git commit | yes (e.g. CVE-2018-10906, fixed long before 3.18.2) |
| SQLite 3.53.4 | git commit of the `sqlite/sqlite` GitHub mirror | yes, many (19 for 3.30.0); OSV's ranges name the `github.com/sqlite/sqlite` mirror of the fossil repository |
| abseil-cpp 20260817.0 | git commit | one (CVE-2025-0838). It is matched for commits on the main branch's history only: the release tags of older LTS branches (`20240116.0`, `20240722.0`) were *not* reported although CVE-2025-0838 affects them, so a vulnerable LTS pin would be missed; the pinned release is a newer one |
| gloop 20260708.rc1 | git commit | none (the repository is new; the scan matches when an advisory appears) |
| liburing 2.14 | git commit | none for `axboe/liburing` |
| numactl 2.0.19 | git commit | none for `numactl/numactl` |
| glibc 2.41-12+deb13u4 (static, from the sysroot) | Debian package `glibc`, release trixie | the Debian tracker's advisories, fixed ones by the `+deb13uN` revision (21 today, all ignored with expiries) |

Not covered, by construction: a vulnerability OSV does not hold with a `GIT`
range (the scan reports what OSV holds; nothing is checked against NVD or
CPEs here).

The test-only dependencies are matched as before: the **Debian packages**
(`pkg:deb/debian/<source>@<version>`) against Debian's security tracker; the
other test-only pins (kernel, QEMU, busybox, ...) carry `pkg:github` or
`pkg:generic` purls that OSV does not match, and are listed in the SBOM for
completeness only. Several of those programs are also in the Debian rootfs,
whose copies are scanned.

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
on `fsync`, every few seconds while a mutation has left dirty state, and at
shutdown; after an unclean shutdown, everything still in the dirty set is
forgotten. This bounds what a power loss costs to re-reading the entries
changed in the last few seconds. A file that was only read is in the dirty
set too, for its access time only: that alone makes no sync point run
until the kernel's own dirtytime expiry (12 hours by default), and after a
crash it costs only that file's attributes.

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

- **`systemctl restart` of a dcfs mount can fail, and for a required mount
  can end in emergency mode.** `systemd` calls a mount unit stopped as soon
  as the mount is gone, while the daemon is still finishing (syncing the
  backing filesystem, closing the cache database: up to two seconds), so the
  start that a restart follows with finds the cache database in use by the
  old daemon and fails with `Cache database ... is in use by another dcfs
  process` (exit status 32). For a mount that `local-fs.target` requires
  (any fstab line without `nofail`) that failure sends the machine to
  `emergency.target`. Stop the unit, wait for the daemon to be gone, then
  start it (`pidof mount.dcfs` shows the daemons). Found by
  step 15.6's systemd guest, which keeps the failing check as
  `DISABLED_systemd-restart-parent-restarts-child`
  (`test/qemu/README.md`, "The systemd guest"); a fix is a start that waits
  for the cache lock, or a `umount.dcfs` that returns when the daemon has
  exited.
- **File names are bytes, but only the logs show them escaped.** dcfs
  treats names, symlink targets and xattr names as unmodified bytes (any
  byte but NUL, and `/` in a name; no normalization, no case folding, no
  text decoding), and whatever the backing filesystem accepts or rejects
  (255-byte names, 4095-byte symlink targets, 1023 on xfs) it accepts or
  rejects the same way. Wherever dcfs prints one in a log line or error
  message it escapes it (`\n`, `\xff`; see `dcfs/escape.h`), so a name
  with a newline cannot forge a log line. The escaping of mount points and
  sources in `mountinfo`, `/etc/fstab` and `exports(5)` text (octal
  escapes) does not exist yet: it arrives with the NFS export tooling
  (plan phase 15). The paths given on the command line (SOURCE, the
  mount point, `dcfs.cache_db`) are not escaped in startup messages yet.
- **A backing filesystem that stops taking writes holds the whole daemon.**
  With the backing filesystem frozen (`fsfreeze -f`, an LVM or storage
  snapshot, a hung network mount), a change through dcfs blocks in its
  backing syscall, and since dcfs serves one request at a time, so does
  every request behind it, a read of cached state included: nothing is
  served until the thaw, and a signal to the waiting process cannot help (the
  daemon is inside the syscall; the checkpoints before it have passed).
  Measured (`//test/qemu:fault_freeze_test`, `test/qemu/README.md`): while
  nothing is held, a frozen backing filesystem does not stop reads: stat,
  listings of cached directories, lookups, opens and reads (including
  passthrough reads) are answered, and a write through passthrough blocks
  its client in the kernel, not the daemon. A change made during the freeze
  completes after the thaw and what dcfs then serves is the backing
  filesystem's. `SIGTERM` while frozen shuts the daemon down at once (its
  last `syncfs` of a frozen filesystem returns immediately); with a change
  held it waits behind it, and the next start recovers the change's dirty
  entry. A periodic sync point during a freeze runs and clears the dirty set.
- **A backing device that fails is met, not hidden, but a lost write can
  leave a stale size until a restart.** When the backing device fails reads
  or writes, a change through dcfs returns the filesystem's error (EIO, or
  EROFS once ext4's journal has aborted), nothing is recorded as having
  succeeded, and after the device recovers the same operations work
  (`//test/qemu:fault_recover_test`, `test/qemu/README.md`, "Fault
  injection"). On xfs and btrfs an inode the device cannot read makes the
  backing filesystem answer "stale handle"; dcfs then looks the name up
  in its directory and replies EIO while the name is still there, keeping
  what it cached (step 11.3b). One limit: a write through passthrough whose
  write-back later fails loses its pages in the backing filesystem, which
  then reports its old size once it re-reads the inode, while dcfs keeps
  serving the size it read earlier (the file's inode stays in the dirty set)
  until the next start of the daemon recovers it. The writer is told by
  its `fsync` (EIO); a writer that never calls it is not told by anything,
  as on any filesystem. On btrfs the kernel itself warns when it cannot
  read a cold inode (`DISABLED_BTRFS_FAILED_INODE_READ_WARNS`,
  `//test/qemu:fault_recover_btrfs_unpinned_test`; a kernel bug, the test
  tolerates exactly that warning).
- **Interrupting a request is prompt only between backing syscalls.** A
  signal to a process waiting on dcfs (Ctrl+C, `timeout`, even `kill -9`)
  ends the wait with `EINTR` at dcfs's next checkpoint, just before its
  next backing syscall: within about a second while dcfs lists a large
  uncached directory on a slow disk. A backing syscall already blocked in
  the kernel (a disk spinning up, a hung network mount, a long `fsync`)
  is waited out first. A change that already reached the backing
  filesystem is completed and reported as done, never as `EINTR`. The
  exception is `fsync`: interrupted after its backing `fsync` and before
  the sync point that makes it durable in dcfs's cache, it replies `EINTR`
  although the file's data was synced; repeating it is harmless. See
  docs/design.md, "Cancellation".
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
  SOURCE, checked against `/proc/self/mountinfo` -- which, because a
  btrfs subvolume is not a separate mount, cannot catch a subvolume that
  already exists under SOURCE before dcfs starts: this is a real gap,
  not an oversight, and there is no way to detect it at startup short of
  walking the whole tree first (which dcfs deliberately never does; see
  "Coherence" below). What dcfs *does* catch, for a subvolume exactly as
  for a real mount, is the boundary appearing at runtime -- a new mount, or
  a btrfs subvolume (pre-existing or freshly created), the first time dcfs
  lists the directory it lives in: logged as an error and shown as an
  empty **stub directory** with the boundary root's mode, owner and times
  as of the last time dcfs probed the name (they are not refreshed when the
  other filesystem's root changes through its own mount, until the
  directory is listed again) and an inode number at or above 2^63 (a range
  no backing inode number may use; dcfs refuses, with `ENOTSUP`, any object
  whose backing inode number is in it). A 32-bit program built without
  large-file support cannot list a directory that holds a stub: the
  kernel's compat `getdents` returns `EOVERFLOW` for its inode number. The stub can be looked up, `stat`ed and used as a
  mount point; anything inside it (listing it, looking up, creating,
  opening) fails with `ENOTSUP`, logged once per stub, removing it with
  `EBUSY` (as a mount point), and renaming it
  with `EXDEV`. A link or rename *into* a stub fails with `ENOTSUP` rather
  than `EXDEV`, because the kernel looks the target name up in the stub
  first. The stub keeps its inode number across restarts and relistings
  (until the name is found to be no boundary any more), and no other stub
  ever gets it. Its NFS handle stops working after a cache wipe.
  Mount another dcfs (or the native filesystem) on the stub to reach what
  is behind it (`boundary_test` covers mounts on all three filesystems and
  btrfs subvolumes). Kernel support for FUSE submounts
  (`FUSE_ATTR_SUBMOUNT`, today used only by virtiofs) would allow lifting
  this.
- **Writes through a shared writable `mmap` after the last `close()` are
  seen late.** With passthrough, the mapping holds only the backing file,
  so the kernel releases the dcfs file at `close()` and later stores reach
  the backing file without dcfs hearing of them. dcfs re-reads the
  attributes of every file that was open for writing when the kernel
  finally lets go of its inode (which cannot happen before the mapping is
  gone) and at unmount, through a descriptor it keeps on each such file
  until then, so that costs no disk access (dcfs raises its open-file limit
  at startup for these, and leaves half of it, at least 16,384 and at most
  65,536, for everything else; past that, or if the limit cannot be raised
  from the usual 1,024 (dcfs warns at startup then), the re-read waits for
  the next access and reads the disk). Until then the file's cached mtime and ctime stay
  as they were at `close()`, and NFS clients, which detect changes through
  ctime, may serve stale data. A mapping that is still writing when dcfs
  is unmounted keeps writing to the backing file afterwards, and the next
  run does not know. While the file is still open for writing, attributes
  are current. Fixing this fully needs a kernel change (docs/design.md,
  "mmap after close").
- **NFS handles do not survive deleting the cache database.** They fail
  with `ESTALE`, never by resolving to a different file. Handles do survive
  restarts of dcfs, and crashes, but a power loss can lose those of rows
  recorded since the database's last durable commit (see "Shutdown,
  crashes and restarts"; the same `ESTALE`).
- **Single-threaded.** dcfs serves one request at a time, so a request that
  has to wait for a disk to spin up delays every other request, including
  ones the cache could answer. The coroutine and io_uring design that
  lifts this is future work.
- **A backing filesystem that went read-only by itself is refused.** After
  an error, ext4 (`errors=remount-ro`) and btrfs (a transaction abort) go
  read-only while their mount stays read-write, and still show changes
  their disk never got. dcfs keeps everything changed since that moment
  dirty (its sync points fail, so the run does not end clean) and refuses
  to start over such a filesystem until it is unmounted, checked and
  mounted again; the next start then re-reads those entries. A filesystem
  mounted read-only on purpose is fine. `mount -o remount,ro` of another
  mount of the same filesystem (a bind mount, another btrfs subvolume)
  looks the same to dcfs, since it makes the shared superblock read-only:
  dcfs refuses to start over SOURCE then, and a running dcfs keeps
  every change dirty (its sync points fail); remount it read-write, or
  mount SOURCE read-only too.
- **A full disk fails requests.** With the backing filesystem full, a
  change that needs space fails with `ENOSPC`, as on the backing
  filesystem itself, and nothing about it is cached (on btrfs, which
  keeps metadata space apart, only writes of data may fail). With the
  cache database's disk full, a request that has to record something in
  the cache fails with `ENOSPC`, including reads that would fill it; what
  is cached already is still served. A create that reached the backing
  filesystem but could not be recorded fails with `EEXIST`: the file
  exists, and the kernel then asks dcfs about the name again instead of
  remembering it as absent. A program that creates names until one is
  new (`mkstemp`, `mkdtemp`) takes `EEXIST` as "try another name", so
  while the cache cannot record anything but its first write still
  succeeds, each retry can leave a new empty file or directory behind on
  the backing filesystem (in practice the next create's first, durable
  write fails with `ENOSPC` first and ends the loop). Free space and
  everything works again; the entries changed meanwhile are re-read after
  the next restart.
- **Power loss re-reads recent changes.** After a power loss or kernel
  crash, everything cached about entries changed in the last
  `dcfs.sync_interval_sec` seconds (or since the last `fsync`) is forgotten
  and re-read from the backing filesystem, which spins it up. An idle dcfs
  has no timer, so a dirty set left by the last burst of activity is only
  cleared by the next request, `fsync` or shutdown; that is safe but makes
  the re-read larger.
- **xfs writes to the disk by itself for about a minute after the last
  write.** Not dcfs: after the last change to an xfs filesystem, its log
  worker writes two small log records (+4 block writes in all), one per
  30-second tick (`fs.xfs.xfssyncd_centisecs`), to mark the log clean. A
  disk that spins down after less than about a minute of inactivity can
  spin up once more because of it; after that an idle xfs and an idle dcfs
  on top of it do no I/O (measured in the test guest: 240 s with
  dcfs mounted, 100 s with dcfs stopped; ext4 and btrfs did not show it in
  the 60 s idle test). Syncing and freezing/thawing the filesystem
  (`fsfreeze -f` / `-u`) does that work immediately, which is what the idle
  test does before its measurement. No mount option was needed or tested.
- **Generation 0 objects.** dcfs reads the backing inode generation
  (`FS_IOC_GETVERSION`) only for regular files and directories; symlinks,
  device nodes, FIFOs and sockets, and every object on a filesystem without
  generations, have generation 0. For them, detecting a recycled inode
  number relies on the stored file handle and the birth time. ext4, xfs
  and btrfs encode the generation in their handles, so this is covered
  there; on a filesystem whose handles carry no generation and which
  reports no birth time, a stale row could match a new object.
- **Removed objects that are still referenced behave as on the backing
  filesystem.** A process whose working directory was removed, or an
  `O_PATH` descriptor on an unlinked file, sees and can change what a local
  filesystem allows (`stat` reports `nlink` 0, `chmod`, `truncate` and
  xattrs work, an unlinked file can be reopened through
  `/proc/<pid>/fd/<n>`), and linking one back gets the local filesystem's
  answer: `ENOENT` for a file with no link left (Linux links such a file
  only if `O_TMPFILE` made it, which works through dcfs), `EPERM` for a
  directory.
- **Access times of directories and symlinks are dcfs's own.** A regular
  file's access time is the backing filesystem's: reads go through
  passthrough and the backing filesystem stamps them by its mount's rule
  (relatime, strictatime, noatime) and the file's own flags (`chattr +A`,
  `O_NOATIME`), and while dcfs has the file open it reads that back from
  the open descriptor (at each close, and for a stat while the file is
  open), so a stat shows exactly what the backing filesystem has. A
  directory's listing and a symlink's target are served from the cache and
  never read on the backing filesystem, so dcfs stamps the access time the
  backing mount's rule gives in its own database, never on the backing
  filesystem: it survives a restart of dcfs and is lost when the cache is
  wiped (the backing filesystem's older one comes back), and a directory's
  own `noatime` flag is not taken into account. After a power loss while a
  file was open and had been read, dcfs may serve the access time from
  before those reads until the file is next opened and closed (the open's
  record of it may not have reached the cache's disk, as with the handles
  recorded since the database's last durable commit). Reads the kernel
  still makes through a file that was open when dcfs stopped (passthrough
  reads go on without the daemon) are not seen: after a clean shutdown and
  restart dcfs may serve that file's access time from before them, until
  it is next opened and closed. `st_blocks` can lag behind delayed
  allocation until the file's attributes are next refreshed (not while the
  file is open).
- **Reflinks fail with `EOPNOTSUPP`; most ioctls with `ENOTTY`.** The
  kernel answers `FICLONE`, `FICLONERANGE` and `FIDEDUPERANGE` itself and
  FUSE has no way to forward them, so `cp --reflink=always` fails on every
  backing filesystem. `copy_file_range` does reach the backing filesystem,
  which on btrfs and xfs shares the extents as a reflink would:
  `cp --reflink=auto` (coreutils' default) gets that. Of the other
  ioctls only `FS_IOC_GETFLAGS`/`FS_IOC_SETFLAGS` and
  `FS_IOC_FSGETXATTR`/`FS_IOC_FSSETXATTR` (`chattr`, `lsattr`) and
  `FS_IOC_GETVERSION` reach the backing file (but `chattr +F`, which
  would make a directory case-insensitive, fails with `EOPNOTSUPP`); any
  other fails with `ENOTTY`. After `chattr` through dcfs the kernel may
  serve the previous ctime until its attribute timeout: it does not drop
  its cached attributes after a successful `FS_IOC_SETFLAGS` or
  `FS_IOC_FSSETXATTR` on a FUSE file, and dcfs is not asked. The guest check
  `immutable-ctime` (`DISABLED_immutable-ctime`) reproduces it and is kept
  disabled until the kernel changes. `O_TMPFILE`, and linking such a file into a name, work. File
  locks are handled by the kernel, locally within the mount.
- **A Linux bug can oops the kernel when casefold is enabled online.**
  `EXT4_IOC_SET_TUNE_SB_PARAM` turns the casefold feature on under a
  mounted ext4 without loading the filesystem's encoding, so the next
  `readdir` of a `chattr +F` directory dereferences NULL (Linux 6.18 to
  7.3-rc). It needs no dcfs; the guest check
  `casefold-tune-online-oops` (`DISABLED_casefold-tune-online-oops`)
  reproduces it and is kept disabled.
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
