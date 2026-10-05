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

1. Extracts that tar into a scratch directory (`tar --no-same-owner`; see
   "Ownership" below for why `--no-same-owner`, not an unprivileged
   namespace dance like the old script's).
2. Recreates two things a complete Debian install's maintainer scripts
   would normally set up, which never run here (neither `rules_distroless`
   nor this project's assembly runs any package's postinst -- no `dpkg
   --configure`, matching `mmdebstrap --variant=apt`'s own behavior):
   - `/etc/mtab` -> `/proc/self/mounts` (also needed by
     `mkrootfs-debian.sh`; see "What this replaces") -- normally the
     `mount` package's postinst.
   - `/bin/sh` -> `/bin/bash` -- normally `dash`'s postinst via
     `update-alternatives` (this project installs `bash`, not `dash`; see
     the `bash` entry under "Packages").
3. Creates `/usr/local/bin` (`guest/init`'s `dcfs_rootfs=` branch copies
   `dcfs`/`fhtest`/`testutil` there; no package in this set creates it --
   it is normally `base-files`' job, and nothing here depends on
   `base-files` either directly or transitively).
4. Runs `mke2fs -q -t ext4 -d <dir> -F` to build the ext4 image directly
   from that directory, no mount or loop device involved.

### Ownership

Every file in the resulting image is owned by whatever uid/gid ran the
Bazel action, not the `root:root` (or other system-user) ownership the
`.deb` payloads themselves specify. This is a deliberate difference from
`mkrootfs-debian.sh`, not a shortcut:

- A real `chown(2)` to an arbitrary uid/gid needs a privilege this action
  does not have. The natural-looking fix -- wrap the extraction and
  `mke2fs` in `fakeroot`, so a plain `tar -x`'s `chown()` calls appear to
  succeed -- was tried and does not work inside a Bazel sandboxed action:
  the sandbox already runs the action inside its own single-entry user
  namespace (confirmed via that action's own `/proc/self/uid_map`:
  `"<uid> <uid> 1"`, i.e. only the invoking uid itself has a mapping, not
  even 0). `fakeroot` only fakes the *specific* failure a genuine
  non-root process gets from `chown(2)` (`EPERM`); inside this
  namespace the kernel instead returns `EINVAL` (the target uid/gid has
  no mapping at all), which `fakeroot` does not recognize as the expected
  case and passes straight through as a real, fatal `tar` error --
  confirmed by instrumenting a throwaway debug genrule and comparing
  `fakeroot`'s behavior with and without a pre-existing restrictive user
  namespace around it.
- It does not matter for `nfs_test` today. `guest/nfs.sh` chroots into
  this image and runs entirely as root (the guest kernel's PID 1 is
  already root; nothing here ever logs in or `su`s to another user), and
  root's own DAC bypass makes a file's nominal owner irrelevant to
  whether root can read, write or execute it. The one case where owner
  identity usually matters on its own -- a setuid/setgid binary
  escalating a non-root caller's privilege -- does not apply either,
  since every caller here already is root.

**This will matter for a later phase.** A systemd-booting guest (and
`dbus`, and any setuid binary systemd or PAM check the owner of) does care
about real `root:root` ownership, not just DAC bypass by an
already-privileged caller -- a future step landing that guest cannot reuse
this image unmodified. `mke2fs` (e2fsprogs) **1.47.1** added exactly the
fix for this: `-d <file.tar>` (a tarball, not a directory) builds the
image straight from the tar's own per-entry uid/gid/mode headers, with no
`chown(2)`, no namespace, no privilege of any kind needed -- the ownership
problem `fakeroot` couldn't solve above disappears entirely, because
nothing ever has to make a *real* directory tree with real inode
ownership in the first place.

Checked this host's version: `mke2fs -V` reports **1.47.0** (released
2023-02-05) -- one release before `-d <tarball>` landed. Confirmed
experimentally, not just by version number: `mke2fs -q -t ext4 -d
some.tar -F out.ext4` on this host fails outright --
`__populate_fs: Not a directory while changing working directory to
"some.tar"` -- this version's `-d` unconditionally `chdir()`s into its
argument, so it cannot take a tarball at all, with no separate flag to
opt in or out. Per this step's instructions: not building e2fsprogs from
source now (that is real, separate work -- a new `third_party/e2fsprogs/`,
the same pattern as `third_party/qemu`/`third_party/busybox`); stopping
here with this documented. When the systemd-booting guest phase lands:

1. Add `third_party/e2fsprogs/` (pinned source, built by Bazel,
   `>= 1.47.1`) -- or confirm a newer host `mke2fs` is available and
   update `test/qemu/README.md`'s prerequisites instead, if hermeticity
   isn't required for this one host tool by then.
2. Build the tar passed to `-d` with explicit root ownership in its
   headers instead of whatever `@debian//:flat` naturally carries post-
   `tar --no-same-owner` extraction -- e.g. `tar --owner=0 --group=0
   --numeric-owner` when re-packing, or (simpler, avoids an extract/
   repack round trip entirely) feed `mke2fs -d` the **original**
   `@debian//:flat` tar directly: it already carries each package's own
   `root:root`-or-whatever headers verbatim (see "Image assembly" above --
   this was true all along; `mkrootfs.sh`'s `tar --no-same-owner`
   extraction is what discards it, not `@debian//:flat` itself).
3. Add a check that root ownership actually took -- e.g. `debugfs -R
   'stat /usr/bin/mount' bazel-bin/third_party/debian/rootfs-debian.ext4`
   and assert `User: 0 Group: 0` in the output -- alongside
   `version_check_test`.

### Host tools

- **`mke2fs`** (e2fsprogs) is not yet built by Bazel; this step uses the
  host's. `-d <dir>` is a plain userspace `libext2fs` operation (no mount,
  no loop device, no root), so this does not violate the plan's "no root,
  no network, no loop mounts" constraint -- it is simply a host tool this
  step did not also hermeticize. A future step could fetch and build
  `e2fsprogs` the same way `third_party/qemu`/`third_party/busybox` do;
  out of scope here.
- Nothing else: `tar` is also the host's, but it was already an
  implicit host dependency of this project's Bazel actions before this
  step (e.g. `test/qemu/scripts/mkinitramfs.sh`'s `cpio`/`gzip`).

Measured on this host: `bazel build //third_party/debian:rootfs` from a
warm `@debian//:flat` is about 5s; the image is 768 MiB nominal
(`mkrootfs.sh`'s hard-coded size, matching the old script's headroom
ratio -- about 245 MiB of actual package content, leaving room for
`/cache/dcfs.db` and whatever `nfs_test` writes during a run, since
`run-qemu.sh` copies this cached image into each test's own `$TEST_TMPDIR`
rather than attaching it directly).

## Test

`//third_party/debian:version_check_test` (host-side, no root, no kernel --
same spirit as `third_party/qemu`/`third_party/busybox`'s own
`smoke_test.sh`) extracts `@debian//:dpkg_status`'s synthesized
`/var/lib/dpkg/status` (scoped to just the 14 explicitly requested
packages) and fails with a diff the moment any of their resolved versions
drifts from `packages.lock`. Before `third_party/debian/BUILD.bazel`
existed, this failed outright (`no such package 'third_party/debian'`);
see this step's commit log for the exact output.

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
  not needed, since this step does not attempt to preserve non-build-user
  ownership at all (see "Ownership" above) -- there is no id mapping
  problem to solve when no `chown` is ever attempted.
- Excluding `./dev/*` from the tar extraction (real device nodes can't be
  created unprivileged): moot, since no package in this set ships any
  (`tar -tvf` on `@debian//:flat` has zero `b`/`c`-type entries --
  confirmed, modern Debian packages don't ship static device nodes,
  `udev`/`devtmpfs` creates them at boot). `guest/init` still bind-mounts
  the real `/dev` over the chroot before using it, unchanged.
