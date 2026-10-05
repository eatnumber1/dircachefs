# third_party/debian: pinned, Bazel-built Debian NFS-test rootfs

Phase 4, part c (`docs/plan/phases/04-pinned-host-tools.md`). The Debian
chroot `test/qemu:nfs_test` runs from (see `test/qemu/guest/nfs.sh` and
`guest/init`'s `dcfs_rootfs=` branch) is resolved, fetched and assembled by
Bazel from a pinned `snapshot.debian.org` timestamp -- not installed on the
host, not checked in, and no longer built once by hand with `mmdebstrap`
into `~/.cache/dcfs` (see "What this replaces" below).

## Pin

- Suite: `bookworm` (Debian 12), matching the suite
  `test/qemu/scripts/mkrootfs-debian.sh` used.
- Snapshot timestamp: **`20261004T203145Z`** (confirmed present via
  `curl -fsSL https://snapshot.debian.org/archive/debian/20261004T203145Z/dists/bookworm/InRelease`,
  picked from that day's listing at
  `https://snapshot.debian.org/archive/debian/?year=2026&month=10`
  -- not an arbitrary/rounded timestamp, an actual published snapshot run).
- Architecture: `amd64` only (the only architecture any dcfs test runs).
- 136 packages resolved in total: the 14 named in MODULE.bazel's
  `apt.install` below, plus everything they pull in transitively
  (`include_transitive` defaults to `True`).

### Updating the pin

1. Pick a new timestamp from
   `https://snapshot.debian.org/archive/debian/?year=YYYY&month=MM`
   (pick an entry from the listing, not a guessed/rounded timestamp --
   `snapshot.debian.org` only serves the exact runs it published).
2. Update the `uris` value in MODULE.bazel's `apt.sources_list`.
3. `bazel test //third_party/debian:version_check_test` -- this fails
   immediately, with a diff, if any resolved package's version changed;
   update `packages.lock` to match (see "Lockfile" below) once the new
   versions are intentional.
4. `bazel build //third_party/debian:rootfs` and re-run
   `bazel test //test/qemu:nfs_test`.
5. Re-run the full suite (`bazel test //...`).

## Packages

The 15 packages named in MODULE.bazel's `apt.install(dependency_set =
"debian", ...)`, with the reason each is there. (Their transitive
dependencies -- the other 121 of the 136 total -- are not individually
justified here, same as this project does not list every library QEMU or
busybox links against beyond what their own READMEs call out as a direct,
deliberate choice.)

Same package set `mkrootfs-debian.sh` installed (`nfs-kernel-server`,
`nfs-common`, `attr`, `strace`, `util-linux`+`mount`, `coreutils`,
`e2fsprogs`, `procps`), plus `bash` (see below) and what later phases need
so this image is not re-pinned again soon (`systemd`/`systemd-sysv`,
`xfsprogs`, `btrfs-progs`, `quota`):

- **`nfs-kernel-server`**: `rpc.nfsd`, `rpc.mountd`, `exportfs` -- the
  NFSv4 server side `guest/nfs.sh` exercises. This is also where
  `exportfs` itself comes from (the plan calls it out by name).
- **`nfs-common`**: the client side -- `mount.nfs4`, `rpc.statd`,
  `rpc.idmapd` -- `guest/nfs.sh` uses to loopback-mount dcfs's own NFS
  export.
- **`attr`**: `getfattr`/`setfattr`, for test scripts exercising extended
  attributes through the chroot/NFS path.
- **`bash`**: not in the original package set -- added because
  `guest/init`'s `dcfs_rootfs=` branch does `chroot /newroot /bin/sh
  "/tests/$TEST"`, and this package set otherwise resolves to *no shell at
  all* (confirmed: `tar -tvf` on `@debian//:flat` has no `bin/sh`,
  `bin/bash` or `bin/dash` without it). `mmdebstrap`'s bootstrap always
  included one because Debian's "essential" package set does (`dash`, via
  `apt`'s own `Priority: required` handling); `rules_distroless` resolves
  a pure `Depends`/`Pre-Depends` graph from the packages actually named,
  which turns out not to reach a shell at all from this project's chosen
  14. `mkrootfs.sh` symlinks `/bin/sh` to the `/bin/bash` this package
  provides (see "Image assembly" -- same reasoning as the `/etc/mtab`
  symlink: the `update-alternatives` postinst that would normally set
  `/bin/sh` up as part of installing `dash` never runs).
- **`strace`**: diagnostic tool kept from the original script -- it is
  exactly what first found the `/etc/mtab` `rpc.mountd` segfault this
  image's `mkrootfs.sh` still works around (see below), and the same
  technique may be needed again for a future nfsd/mountd issue.
- **`util-linux`, `mount`**: `losetup`, `blkid`, `findmnt`, `fstab`
  handling, and (bookworm splits it out as its own binary package,
  built from the same source) `/bin/mount`/`/bin/umount` -- needed by
  this step's own package set and by future fstab tests (plan: "util-linux
  `mount` (fstab tests)").
- **`coreutils`**: a GNU userspace (`stat`, `find`, `cp`, ...) richer than
  the busybox-only initramfs every other test runs from -- see
  `third_party/busybox/README.md`'s "`mountpoint` and `find`" section for
  a concrete gap (`find -ls`) this sidesteps.
- **`e2fsprogs`**: `mkfs.ext4`/`e2fsck`/`dumpe2fs`, for anything this
  chroot needs to create or inspect an ext4 filesystem at test time.
- **`procps`**: `ps`, `free`, `pkill` -- process inspection inside the
  chroot (`guest/nfs.sh`'s cleanup trap already uses `pkill`).
- **`systemd`, `systemd-sysv`**: not used by `nfs_test` today -- added so
  a later phase's systemd-booting guest and `systemd-analyze verify` can
  use this same pinned image instead of this project re-pinning a second
  Debian snapshot just for that. `systemd-sysv` provides `/sbin/init`.
- **`xfsprogs`**: `mkfs.xfs` and friends, for the same reason.
- **`btrfs-progs`**: `mkfs.btrfs` and friends, for the same reason.
- **`quota`**: `quotacheck`/`repquota`/`edquota`, for the same reason.

## Lockfile

`rules_distroless` 0.9.4 (the latest version on the BCR as of 2026-10-05;
the plan asked for "BCR, latest") resolves packages through Bazel's
module-extension **facts** cache (`module_ctx.facts`, available since
Bazel 9 -- this repository pins Bazel 9.2.0, `.bazelversion`), not through
the older, separately-checked-in `<name>.lock.json` file the
`rules_distroless` README's "Lockfiles" section still documents (that path
-- `apt.lock(into = ...)` plus `bazel run @debian//:lock` -- is explicitly
only taken `if not use_facts` in `apt/extensions.bzl`; confirmed by
reading the extension source at the pinned version). Concretely:

- Every resolved package (name, version, architecture, URL, **sha256**) is
  recorded by Bazel itself in this repository's own already-checked-in
  `MODULE.bazel.lock`, exactly the same way every other `bazel_dep`/
  `http_archive` pin in this project already is (`qemu`, `busybox`,
  `pjdfstest`, ...) -- there is nothing extra to check in for this to be
  reproducible and auditable; `git diff MODULE.bazel.lock` shows exactly
  what changed on a pin update.
- `third_party/debian/packages.lock` is this project's own, additional,
  deliberately small checked-in lock: just the 14 explicitly requested
  packages' resolved versions (not the other 122 transitive packages),
  verified against the live resolution by
  `//third_party/debian:version_check_test` on every `bazel test //...`.
  This is the "a checked-in lock file (sha256 per package)" the plan asks
  for in spirit -- human-reviewable at a glance -- while the actual
  per-package sha256 enforcement (what Bazel will refuse to silently
  change under you) lives in `MODULE.bazel.lock`, which already serves
  exactly that role for every other pinned dependency in this project.

## Image assembly

`BUILD.bazel`'s `:rootfs` genrule runs `scripts/mkrootfs.sh` against
`@debian//:flat` -- `rules_distroless`'s own `flatten` rule, which merges
every resolved package's `data.tar` (post-processed by its `deb_postfix`
rule; no `mergedusr` rewriting applied -- bookworm packages still ship
real files directly under `/bin`, `/sbin`, `/lib`, not only under their
`/usr` counterparts, confirmed by `tar -tvf` on `@debian//:flat`) into one
tar, deduplicating directory entries. `mkrootfs.sh` then:

1. Derives the complete, top-down-ordered set of directory paths every
   entry in `@debian//:flat` implies (every ancestor of every path, not
   just the ones the tar happens to declare explicitly -- see "Ownership"
   below, "The directory-completeness gap", for why this step exists at
   all) and builds a real scratch directory tree from that list, so a
   recursive `tar -c` over it naturally emits each parent before any of
   its children.
2. Tars that skeleton (`--owner=0 --group=0 --numeric-owner`) and
   concatenates `@debian//:flat` onto it (`tar --concatenate`) -- the
   combined tar is then strictly more complete than `@debian//:flat`
   alone, and changes no path `@debian//:flat` already declares (mke2fs
   treats a later duplicate directory entry as a no-op).
3. Recreates, the same way, five things a complete Debian install's
   maintainer scripts would normally set up, which never run here
   (neither `rules_distroless` nor this project's assembly runs any
   package's postinst -- no `dpkg --configure`, matching `mmdebstrap
   --variant=apt`'s own behavior) -- each built as a real symlink/file/
   directory in a second scratch tree, tarred the same `--owner=0
   --group=0` way and concatenated onto the combined tar:
   - `/etc/mtab` -> `/proc/self/mounts` (also needed by
     `mkrootfs-debian.sh`; see "What this replaces") -- normally the
     `mount` package's postinst.
   - `/bin/sh` -> `/bin/bash` -- normally `dash`'s postinst via
     `update-alternatives` (this project installs `bash`, not `dash`; see
     the `bash` entry under "Packages").
   - `/usr/bin/awk` -> `mawk` -- normally `mawk`'s own postinst via
     `update-alternatives`.
   - `/etc/exports` (empty) -- `exportfs(8)` refuses outright if it's
     missing entirely; `guest/nfs.sh`'s own `exportfs -o ...` call adds
     the one export it needs at runtime.
   - `/usr/local/bin` -- `guest/init`'s `dcfs_rootfs=` branch copies
     `dcfs`/`fhtest`/`testutil` there; no package in this set creates it
     (normally `base-files`' job, and nothing here depends on
     `base-files` either directly or transitively).
4. Runs `mke2fs -q -t ext4 -d <combined tar> -F` (the Bazel-built, static
   `//third_party/e2fsprogs:mke2fs` -- see that package's README.md) to
   build the ext4 image directly from that tar's own per-entry
   uid/gid/mode headers, no extraction, mount or loop device involved at
   any point.

### Ownership

Every file in the resulting image has **real** `root:root` (or whatever
other owner/setuid/setgid bits the originating `.deb` payload set)
ownership -- not the uid/gid that ran the Bazel action that assembled it.
`//third_party/debian:ownership_test` checks this directly (`debugfs
stat` on `/`, `/etc/os-release`, `/usr/sbin/rpc.nfsd` and `/bin/mount`,
plus `/bin/mount`'s setuid bit).

This was not always true. Earlier revisions of this file extracted
`@debian//:flat` with `tar --no-same-owner` into a scratch directory
before handing it to `mke2fs -d <dir>`, discarding every file's real
owner (a real `chown(2)` to an arbitrary uid/gid needs a privilege a
sandboxed Bazel action does not have, and `fakeroot`'s `LD_PRELOAD` trick
does not work inside the sandbox's own single-entry user namespace --
confirmed by instrumenting a throwaway debug genrule: the kernel rejects
the `chown(2)` with `EINVAL`, not the `EPERM` `fakeroot` knows how to
fake). That didn't matter for `nfs_test` at the time -- `guest/nfs.sh`
chroots in and runs entirely as root, and root's own DAC bypass makes a
file's nominal owner irrelevant -- but would have mattered for a later,
systemd-booting guest (systemd/PAM/dbus do care about real ownership, not
just an already-privileged caller's DAC bypass).

`mke2fs` (e2fsprogs) **1.47.1** added the fix this step now uses: `-d
<file.tar>` (a tarball, not a directory) builds the image straight from
the tar's own per-entry uid/gid/mode headers, with no `chown(2)`, no
namespace, no privilege of any kind needed, and no real directory tree
with real inode ownership ever gets materialized in the first place. The
host's `mke2fs` was one release too old for this (**1.47.0**, confirmed
experimentally: `-d some.tar` unconditionally `chdir()`s into its
argument and fails outright, `__populate_fs: Not a directory`), so this
step also adds `third_party/e2fsprogs/` -- a pinned, static,
Bazel-built `mke2fs`/`debugfs` -- see that package's README.md.

#### The directory-completeness gap

Feeding `@debian//:flat` to `mke2fs -d` directly (no fixups) fails
outright, reproducibly, with two distinct, unrelated errors found by
hand before `mkrootfs.sh`'s directory-skeleton step (above) was added:

```
__populate_fs_from_tar: File not found by ext2_lookup cannot find directory "./var/lib/dpkg" to create "status"
mke2fs: File not found by ext2_lookup while populating file system
```

and, after prepending just that one missing directory:

```
__populate_fs_from_tar: File not found by ext2_lookup cannot find directory "./etc" to create "ld.so.conf.d"
```

e2fsprogs's own tar-import code (`misc/create_inode_libarchive.c`'s
`__populate_fs_from_tar`) is not a general-purpose tar extractor: it
walks entries in stream order and does a plain `ext2fs_namei` lookup for
each entry's parent directory, which must already exist in the
filesystem being built by that point -- unlike a real `tar -x`
extraction (what every earlier version of this script did), it never
auto-creates a missing intermediate directory. `@debian//:flat` does not
reliably provide one: `./var/lib/dpkg/status` (`rules_distroless`'s own
synthesized dpkg status file) has no `./var/lib/dpkg` directory entry
anywhere in the tar at all, and `./etc/ld.so.conf.d/<file>` appears
before any `./etc/ld.so.conf.d` directory entry survives the `flatten`
rule's own directory-entry deduplication (merging many packages'
individual `data.tar`s can reorder which package's copy of a shared
directory entry "wins"). `mkrootfs.sh`'s fix is general, not a
case-by-case patch for these two: it derives *every* ancestor directory
every entry in the tar implies and prepends the complete set, so it does
not matter which specific paths a future snapshot/package-list update
turns out to be missing.

### Host tools

None. `mke2fs` (e2fsprogs) is now `//third_party/e2fsprogs:mke2fs`, a
pinned, static, Bazel-built binary -- see that package's README.md for
the version, configure flags and how its own `-d <tarball>` dependency on
libarchive was hermeticized. `tar` remains the host's, but it was already
an implicit host dependency of this project's Bazel actions before this
step (e.g. `test/qemu/scripts/mkinitramfs.sh`'s `cpio`/`gzip`), and
nothing here needs root, network access or a loop mount: `mke2fs -d`
never `chown()`s, mounts or attaches anything, it just copies tar header
fields into inodes it already has full access to create.

Measured on this host: `bazel build //third_party/debian:rootfs` from a
warm `@debian//:flat` (and a warm `//third_party/e2fsprogs:mke2fs`) is
about 2s; the image is 768 MiB nominal (`mkrootfs.sh`'s hard-coded size,
matching the old script's headroom ratio -- about 245 MiB of actual
package content, leaving room for `/cache/dcfs.db` and whatever
`nfs_test` writes during a run, since `run-qemu.sh` copies this cached
image into each test's own `$TEST_TMPDIR` rather than attaching it
directly).

## Test

`//third_party/debian:version_check_test` (host-side, no root, no kernel --
same spirit as `third_party/qemu`/`third_party/busybox`'s own
`smoke_test.sh`) extracts `@debian//:dpkg_status`'s synthesized
`/var/lib/dpkg/status` (scoped to just the 14 explicitly requested
packages) and fails with a diff the moment any of their resolved versions
drifts from `packages.lock`. Before `third_party/debian/BUILD.bazel`
existed, this failed outright (`no such package 'third_party/debian'`);
see this step's commit log for the exact output.

`//third_party/debian:ownership_test` (host-side, no root, no kernel --
`debugfs stat`, same as `//third_party/e2fsprogs:smoke_test`'s own
end-to-end check) is the real regression test for this step's "Ownership"
section: before `mkrootfs.sh` switched to `mke2fs -d <tarball>`, this
failed with `/etc/os-release` (and every other file) reporting `User:
1000 Group: 120` -- the build uid/gid, not root (see this step's commit
log for the exact failing output); it now confirms `/`, `/etc/os-release`
and `/usr/sbin/rpc.nfsd` are `root:root` and `/bin/mount` is both
`root:root` and setuid.

`//test/qemu:nfs_test` (`test/qemu/BUILD.bazel`) is the real end-to-end
check: it now takes its `rootfs` from `//third_party/debian:rootfs`
instead of the old `@kernel_image//:rootfs_debian.ext4` symlink-to-
`~/.cache` target (see `test/qemu/kernel.bzl`, `test/qemu/README.md`).

## What this replaces

`test/qemu/scripts/mkrootfs-debian.sh` (deleted by this step): a
`mmdebstrap --mode=unshare` run against the live `bookworm` mirror,
invoked once by hand into `~/.cache/dcfs/rootfs-debian.ext4` and exposed
to Bazel by `test/qemu/kernel.bzl`'s `kernel_image` repository rule
symlinking it in (a `DCFS-ROOTFS-MISSING` placeholder otherwise). Two of
its real findings are still true and still handled, just in a different
place now:

- The stale `debian-archive-keyring`/`NO_PUBKEY` problem: moot here --
  `rules_distroless` fetches each package directly, by URL and `sha256`,
  from the pinned snapshot; nothing here ever verifies an `InRelease`/
  `Release` GPG signature, since the per-package hash already pins exactly
  the bytes used (the same trust model this project's other
  `http_archive`-pinned dependencies already use).
- `/etc/mtab`: still missing for the same underlying reason (no package's
  postinst ever runs), so `mkrootfs.sh` still symlinks it by hand -- see
  "Image assembly" above.

Two of its findings do not apply any more:

- The unprivileged tar-extract-to-ext4 uid-mapping dance
  (`/etc/subuid`/`/etc/subgid`, `unshare --map-user=0 --map-users=...`):
  not needed, even though this image now does end up with real
  `root:root` ownership (see "Ownership" above) -- `mke2fs -d <tarball>`
  reads each entry's uid/gid straight out of the tar header into the
  inode it creates, with no `chown(2)` call and therefore no id-mapping
  problem to solve in the first place; the old script's unshare/subuid
  dance existed specifically to make an unprivileged `chown(2)` to an
  arbitrary uid succeed, a step this approach never needs.
- Excluding `./dev/*` from the tar extraction (real device nodes can't be
  created unprivileged): moot, since no package in this set ships any
  (`tar -tvf` on `@debian//:flat` has zero `b`/`c`-type entries --
  confirmed, modern Debian packages don't ship static device nodes,
  `udev`/`devtmpfs` creates them at boot). `guest/init` still bind-mounts
  the real `/dev` over the chroot before using it, unchanged.
