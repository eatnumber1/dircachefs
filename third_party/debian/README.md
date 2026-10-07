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
- Snapshot timestamp: **`20261006T082722Z`** (confirmed present via
  `curl -fsSL https://snapshot.debian.org/archive/debian/20261006T082722Z/dists/bookworm/InRelease`,
  picked from that day's listing at
  `https://snapshot.debian.org/archive/debian/?year=2026&month=10`
  -- not an arbitrary/rounded timestamp, an actual published snapshot run).
- Architecture: `amd64` only (the only architecture any dcfs test runs).
- 149 packages resolved in total (R3 recount; this README said 136 when
  the list had 14 named packages): the 20 named in MODULE.bazel's
  `apt.install` below, plus everything they pull in transitively
  (`include_transitive` defaults to `True`).

- Suites (step 5.3b): `bookworm` and `bookworm-updates` from
  `archive/debian/20261006T082722Z`, and `bookworm-security` from the separate
  `archive/debian-security/20261006T081244Z` (the 20261004T203145Z snapshot
  that first pinned this had the other two but not perl 5.36.0-7+deb12u4,
  which Debian's security tracker already required, so the pin moved to the
  newest runs of 2026-10-06). rules_distroless takes the newest version
  across the suites. Six packages moved: libevent, expat, xz-utils, pcre2,
  openssl and perl (3 binaries + libperl); `tools/sbom/debian_sources.tsv`
  must list every resolved package, so regenerate it with the pin
  (tools/sbom/README.md).

### Updating the pin

1. Pick a new timestamp from
   `https://snapshot.debian.org/archive/debian/?year=YYYY&month=MM`
   (pick an entry from the listing, not a guessed/rounded timestamp --
   `snapshot.debian.org` only serves the exact runs it published).
2. Update the `uris` value in MODULE.bazel's `apt.sources_list`.
3. `bazel mod deps --lockfile_mode=update` (any other Bazel command now
   fails with "MODULE.bazel.lock is no longer up-to-date": see "Lockfile"
   below), then `bazel test //third_party/debian:version_check_test` --
   this fails, with a diff, if any resolved package's version or any
   .deb's checksum changed; update `packages.lock` and `debs.lock` to
   match (see "Lockfile" below) once the new versions are intentional.
4. `bazel build //third_party/debian:rootfs` and re-run
   `bazel test //test/qemu:nfs_test`.
5. Re-run the full suite (`bazel test //...`).

## Packages

The 15 packages named in MODULE.bazel's `apt.install(dependency_set =
"debian", ...)`, with the reason each is there. (Their transitive
dependencies -- the other 129 of the 149 total -- are not individually
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
  14. `mkrootfs.py` symlinks `/bin/sh` to the `/bin/bash` this package
  provides (see "Image assembly" -- same reasoning as the `/etc/mtab`
  symlink: the `update-alternatives` postinst that would normally set
  `/bin/sh` up as part of installing `dash` never runs).
- **`strace`**: diagnostic tool kept from the original script -- it is
  exactly what first found the `/etc/mtab` `rpc.mountd` segfault this
  image's `mkrootfs.py` still works around (see below), and the same
  technique may be needed again for a future nfsd/mountd issue.
- **`util-linux`, `mount`**: `losetup`, `blkid`, `findmnt`, `fstab`
  handling, and (bookworm splits it out as its own binary package,
  built from the same source) `/bin/mount`/`/bin/umount` -- needed by
  this step's own package set and by future fstab tests (plan: "util-linux
  `mount` (fstab tests)").
- **`coreutils`**: a GNU userspace (`stat`, `find`, `cp`, ...) richer than
  the busybox-only initramfs every other test runs from -- a gap (`find -ls`) this sidesteps.
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
  `http_archive` pin in this project already is (`pjdfstest`, ...) -- there is nothing extra to check in for this to be
  reproducible and auditable; `git diff MODULE.bazel.lock` shows exactly
  what changed on a pin update.
- **`common --lockfile_mode=error`** (`.bazelrc`, R3/L9): Bazel never
  rewrites `MODULE.bazel.lock` silently. Verified by changing the snapshot
  timestamp, and separately by deleting `strace` from `apt.install`: each
  fails the next Bazel command with `MODULE.bazel.lock is no longer
  up-to-date because the usages of the extension
  '@@rules_distroless+//apt:extensions.bzl%apt' have changed`. The facts
  rules_distroless 0.9.4 keeps are part of the extension's recorded
  result in `MODULE.bazel.lock` (`generatedRepoSpecs`: one `deb_import`
  per package with its URL and **sha256**, verified when Bazel downloads
  the .deb), so no separate explicit lock file is needed for enforcement.
  To change a pin on purpose: edit `MODULE.bazel`, run
  `bazel mod deps --lockfile_mode=update`, and refresh the two checked-in
  lists below.
- `third_party/debian/packages.lock`: the 20 explicitly requested
  packages' resolved versions (name and full Debian version, epoch
  included), checked against the live resolution
  (`@debian//:dpkg_status`) by `//third_party/debian:version_check_test`.
- `third_party/debian/debs.lock` (R3/L9): **every** fetched .deb, the
  transitive ones too (149 lines of `<file name> <sha256>`), the human
  reviewable form of what `MODULE.bazel.lock` records, so a pin change shows
  up as a plain-text diff. `version_check_test` compares it with
  `MODULE.bazel.lock` (through `scripts/lock_debs.sh`); regenerate with
  `sh third_party/debian/scripts/lock_debs.sh MODULE.bazel.lock >
  third_party/debian/debs.lock`. (The live resolution cannot be queried for
  the transitive packages from Bazel: only the 20 named ones have targets
  visible from the root module, so for those versions are checked live and
  for the rest the lock file is what is compared.)

## Image assembly

`BUILD.bazel`'s `:rootfs` genrule runs `scripts/mkrootfs.py` against
`@debian//:flat` -- `rules_distroless`'s own `flatten` rule, which merges
every resolved package's `data.tar` (post-processed by its `deb_postfix`
rule; no `mergedusr` rewriting applied -- bookworm packages still ship
real files directly under `/bin`, `/sbin`, `/lib`, not only under their
`/usr` counterparts, confirmed by `tar -tvf` on `@debian//:flat`) into one
tar, deduplicating directory entries. `mkrootfs.py` (Phase 24; it replaced a
shell script around `mke2fs -d <tarball>`) then:

1. Unpacks the tar into a scratch directory, entry by entry and in order.
   Every parent directory is resolved through the symlinks the tar itself
   holds, lexically and inside the scratch directory (an absolute target
   means the image's root, never the host's), so an entry cannot reach
   outside it; a directory the tar lacks is created, as `tar -x` would
   (`@debian//:flat` has `./var/lib/dpkg/status` and no `./var/lib/dpkg`).
   Hard links are made as hard links (`perl`/`perl5.36.0`,
   `perlbug`/`perlthanks`).
2. Adds the five things a complete Debian install's maintainer scripts would
   normally set up, which never run here (neither `rules_distroless` nor this
   project's assembly runs any package's postinst, matching `mmdebstrap
   --variant=apt`):
   - `/etc/mtab` -> `/proc/self/mounts` -- normally the `mount` package's
     postinst; `rpc.mountd`'s crossmnt handling does
     `openat("/etc/mtab")` unconditionally and, when it is missing,
     dereferences NULL (a reproducible SIGSEGV).
   - `/bin/sh` -> `/bin/bash` -- normally `dash`'s postinst via
     `update-alternatives` (this project installs `bash`, not `dash`; see
     the `bash` entry under "Packages"). `guest/init` chroots in and runs
     `/bin/sh`.
   - `/usr/bin/awk` -> `mawk` -- normally `mawk`'s own postinst via
     `update-alternatives`.
   - `/etc/exports` (empty) -- `exportfs(8)` refuses outright if it's
     missing entirely; `guest/nfs.sh`'s own `exportfs -o ...` call adds
     the one export it needs at runtime.
   - `/usr/local/bin` -- `guest/init`'s `dcfs_rootfs=` branch copies
     `dcfs`/`fhtest`/`testutil` there; no package in this set creates it
     (normally `base-files`' job, and nothing here depends on
     `base-files` either directly or transitively).
3. Runs `mke2fs -q -t ext4 -d <directory>` (Alpine's, through musl's loader;
   fixed UUID, hash seed and clock, `E2FSPROGS_FAKE_TIME`). `MKE2FS_CONFIG`
   points at the checked-in `//third_party/e2fsprogs:mke2fs.conf` (R3/L5), so
   the image's ext4 features never depend on the host's `/etc/mke2fs.conf`;
   `ownership_test` also checks that `metadata_csum_seed` and `orphan_file`
   (which this machine's `/etc/mke2fs.conf` lacks) are present.
4. Runs `debugfs -w -f` with a `set_inode_field` line for the mode, uid and
   gid of every inode, taken from the tar (see "Ownership").

### Ownership

Every file in the resulting image has **real** `root:root` (or whatever
other owner/setuid/setgid bits the originating `.deb` payload set)
ownership -- not the uid/gid that ran the Bazel action that assembled it.
`//third_party/debian:ownership_test` checks this directly on the real image
(`debugfs stat` on `/`, `/etc/os-release`, `/usr/sbin/rpc.nfsd` and
`/bin/mount`, plus `/bin/mount`'s setuid bit), and
`//third_party/debian:mkrootfs_test` checks ownership, modes, hard links,
symlinked parents, the fixed-up paths and reproducibility on a small tar.

A Bazel action has no privilege to `chown(2)` to an arbitrary uid/gid (and
`fakeroot` does not work inside the sandbox's single-entry user namespace:
the kernel rejects the `chown(2)` with `EINVAL`, not the `EPERM` fakeroot
knows how to fake). An extracted tree therefore carries the builder's
ownership, and `mke2fs -d <directory>` copies it. Until Phase 24 the image
was made with `mke2fs -d <tarball>` (e2fsprogs 1.47.1 and later, through
libarchive), which reads each entry's uid/gid/mode from the tar header and
needs no privilege; Alpine's mke2fs is built without libarchive, so
`mkrootfs.py` makes the filesystem from a directory and then sets every
inode's owner, group and mode with `debugfs`, which writes the image file
directly. In `@debian//:flat` all but four entries are `root:root`
(`/var/local` is `root:staff`; `chage`, `expiry` and `unix_chkpwd` are
`root:shadow`), but the extracted directory has the builder's owner on all
of them, so every inode is set. On the pin of 2026-10-06 the new image listed
8715 paths with the same type, mode, owner, group and size and the same hard
link groups as the one `mke2fs -d <tarball>` made, and a `debugfs rdump` of
both trees was identical.

`debugfs` exits 0 after a command fails, so `mkrootfs.py` treats any output
besides the echoed commands as an error.

### Host tools

`tar` is not needed: Python reads the tar. `mke2fs` and `debugfs` are
Alpine's (`@alpine_fstools`), run through wrappers (`third_party/alpine`).
Nothing here needs root, network access or a loop mount.

The image is 768 MiB nominal (`mkrootfs.py`'s `--size`, matching the old
script's headroom ratio -- about 245 MiB of actual package content,
leaving room for `/cache/dcfs.db` and whatever `nfs_test` writes during a
run, since `run-qemu.sh` copies this cached image into each test's own
`$TEST_TMPDIR` rather than attaching it directly).

## Test

`//third_party/debian:version_check_test` (host-side, no root, no kernel --
same spirit as the Alpine repositories' `tools_test`) extracts `@debian//:dpkg_status`'s synthesized
`/var/lib/dpkg/status` (scoped to just the 20 explicitly requested
packages) and fails with a diff the moment any of their resolved versions
drifts from `packages.lock`. Before `third_party/debian/BUILD.bazel`
existed, this failed outright (`no such package 'third_party/debian'`);
see this step's commit log for the exact output.

`//third_party/debian:ownership_test` (host-side, no root, no kernel --
`debugfs stat`) is the real regression test for this step's "Ownership"
section: before the image had real ownership, this
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
  postinst ever runs), so `mkrootfs.py` still symlinks it by hand -- see
  "Image assembly" above.

Two of its findings do not apply any more:

- The unprivileged tar-extract-to-ext4 uid-mapping dance
  (`/etc/subuid`/`/etc/subgid`, `unshare --map-user=0 --map-users=...`):
  not needed, even though this image now does end up with real
  `root:root` ownership (see "Ownership" above) -- `mkrootfs.py` sets each inode's
  uid/gid with `debugfs` (and, before Phase 24, `mke2fs -d <tarball>` read
  them from the tar header), with no `chown(2)` call and therefore no
  id-mapping problem to solve in the first place; the old script's unshare/subuid
  dance existed specifically to make an unprivileged `chown(2)` to an
  arbitrary uid succeed, a step this approach never needs.
- Excluding `./dev/*` from the tar extraction (real device nodes can't be
  created unprivileged): moot, since no package in this set ships any
  (`tar -tvf` on `@debian//:flat` has zero `b`/`c`-type entries --
  confirmed, modern Debian packages don't ship static device nodes,
  `udev`/`devtmpfs` creates them at boot). `guest/init` still bind-mounts
  the real `/dev` over the chroot before using it, unchanged.
