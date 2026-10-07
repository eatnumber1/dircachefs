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
| `build-essential` (gcc, g++, binutils, make) | every C/C++ compile, the kernel, QEMU and the mkfs tools | Phase 7 pins an LLVM toolchain |
| `flex`, `bison` | the kernel build (kconfig's lexer and parser) | the BCR builds fail on them, `third_party/linux/README.md` |
| `ninja-build` | QEMU's build (`third_party/qemu`) | found by `act` (Phase 5.2); QEMU's configure fails with "Cannot find Ninja" |
| `libelf-dev` | the kernel build: objtool includes `<gelf.h>` | found by `act` (Phase 5.2); the BCR's `elfutils` is the hermetic candidate, not pursued (`third_party/linux/README.md`) |
| `python3`, `perl` | QEMU's configure and meson, the kernel's scripts | universal on build hosts |
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
dcfs --source=<dir> --cache_db=<path> [flags] <mountpoint>
```

### Flags

`dcfs --help` lists all flags and `dcfs --version` prints the version.

| Flag | Default | Meaning |
|---|---|---|
| `--source` | (required) | The directory to cache. Opened once at startup; dcfs never uses the path again. |
| `--cache_db` | (required) | The SQLite cache database. Created if missing, mode 0600 (its `-wal`/`-shm` files inherit that mode too), since it holds metadata as sensitive as `--source`'s: every cached name, attribute, xattr and symlink target, including those of directories a reader cannot list. Its directory is created mode 0700 if missing; an existing one that is group- or world-accessible logs a warning but does not stop dcfs from starting. dcfs refuses to start if the database is a symlink or not a regular file, or if it (or an existing `-wal`/`-shm`) grants more access than `--source`'s root directory does (owner not root or that directory's owner; group or other read/write that directory does not grant); the error names both sets of permissions. A database that passes but is looser than 0600 is tightened to 0600 with a warning. Put it on an SSD, not on the backing disks, on a local filesystem: dcfs refuses to start if SQLite cannot use WAL mode there. |
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

1. Install `truncate` (coreutils; creates the scratch-disk images). QEMU,
   the mkfs tools, its qboot firmware, busybox and the test kernel are all
   pinned and fetched/built by Bazel (`//third_party/qemu`,
   `//third_party/busybox`, `//third_party/linux`); there is no host QEMU,
   qboot, busybox or manual kernel build step any more -- see
   `test/qemu/README.md`.
2. Get write access to `/dev/kvm`: `sudo usermod -aG kvm "$USER"`, then log
   in again. KVM is optional: without it QEMU falls back to software
   emulation (TCG), 2-9x slower (`DCFS_FORCE_TCG=1` forces this).
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

## Continuous integration

`.github/workflows/ci.yml` runs the whole suite on GitHub Actions
(`ubuntu-24.04` runners) in three jobs, the tiers of
`test/qemu/README.md`:

| Job | Runs | Needs |
|---|---|---|
| `fast` | `bazel test --config=fast //...` (small tests) | |
| `presubmit` | `bazel test --config=presubmit //...` (small and medium) | `fast` |
| `full` | `bazel test //...` (every tier, pjdfstest on all three filesystems), then `bazel test --config=asan //...` | `presubmit` |

- **Caches.** Bazel's disk cache, repository cache and Bazelisk's download
  are restored and saved with `actions/cache`, even when tests fail (the
  kernel build takes half an hour). The disk cache is content addressed, so
  a pin change invalidates only the actions whose inputs changed; the keys
  end in the commit and fall back to the newest entry of the same job.
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
- **Kernel matrix: not yet.** Every test boots the one pinned kernel
  (`//third_party/linux:bzImage`, the latest stable release when pinned).
  Testing the minimum supported kernel (6.9) too needs a second pinned
  kernel, a config fragment without the options newer than 6.9
  (`FUSE_IO_URING` is 6.14) and a Bazel flag choosing the kernel in the
  `qemu_test` macros: `third_party/linux/README.md`, "Kernel matrix".
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
still points at that commit). The compiler toolchain joins this list when
Phase 7 pins it. The job fails on any finding that `osv-scanner.toml` does
not ignore; an ignore needs a reason and an expiry date, and an expired or
unexplained ignore fails the job. A self-check step scans a deliberately old
libfuse (3.2.0, CVE-2018-10906; `tools/sbom/testdata/`) with the very same
invocation and fails the job if the scanner reports nothing.

**Informational, never gating: the test-only dependencies** (the Debian
test image, kernel, QEMU, busybox, e2fsprogs, xfsprogs, btrfs-progs,
util-linux, urcu, inih, bc, dtc, pjdfstest, googletest, google_benchmark,
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
  (plan phase 15). The paths given on the command line (`--source`, the
  mount point, `--cache_db`) are not escaped in startup messages yet.
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
  first. The stub keeps its inode number across restarts (until its
  directory is relisted). Its NFS handle stops working after a cache wipe.
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
  at startup for these, and holds at most half of it, leaving at least
  65,536 for everything else; past that, or if the limit cannot be raised
  from the usual 1,024, the re-read waits for the next access and reads the
  disk). Until then the file's cached mtime and ctime stay
  as they were at `close()`, and NFS clients, which detect changes through
  ctime, may serve stale data. A mapping that is still writing when dcfs
  is unmounted keeps writing to the backing file afterwards, and the next
  run does not know. While the file is still open for writing, attributes
  are current. Fixing this fully needs a kernel change (docs/design.md,
  "mmap after close").
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
- **Removed objects that are still referenced cannot be linked back.**
  A process whose working directory was removed, or an `O_PATH`
  descriptor on an unlinked file, sees and can change what a local
  filesystem allows (`stat` reports `nlink` 0, `chmod`, `truncate` and
  xattrs work, an unlinked file can be reopened through
  `/proc/<pid>/fd/<n>`), but a hard link to it fails with `ESTALE`.
- **Access times are predicted.** Reads go through passthrough, so dcfs
  never sees them: when a file is opened for reading it records the access
  time the backing filesystem's mount option gives a read (relatime, the
  default: if the old one is not after the modification or change time,
  or is a day old; strictatime: always; noatime: never), without touching
  the disk. The backing filesystem stamps the read itself, so the two can
  differ by the moment between the open and the read, and an open that
  reads nothing still moves dcfs's (so does the private open behind
  `lsattr` and `chattr`). A file's own `noatime` flag (`chattr +A`) is not
  taken into account, and after a power loss dcfs may keep an access time
  the backing filesystem lost. Directories' access times are not
  maintained. `st_blocks` can lag behind delayed allocation until the
  file's attributes are next refreshed.
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
  `FS_IOC_FSSETXATTR` on a FUSE file, and dcfs is not asked. `O_TMPFILE`, and linking such a file into a name, work. File
  locks are handled by the kernel, locally within the mount.
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
