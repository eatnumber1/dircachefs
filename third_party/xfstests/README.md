# xfstests (pinned): fsstress, fsx, replay-log and the generic tests

Why (step 17.1): the generic tests are the standard file system conformance
suite; `test/qemu/guest/xfstests.sh` runs them against dcfs on each backing
file system (the `xfstests_*` QEMU tests, "The runtime" below).

Why (steps 11.2b and 12.14): `fsstress` (random concurrent namespace and data operations) and `fsx`
(data, size and mmap correctness) are the standard file system stress tools;
the `stress_*` QEMU tests run them against dcfs and compare the cache with the
backing file system afterwards (`test/qemu/guest/stress.sh`, plan step 11.2b).
`replay-log` (from `src/log-writes/`) replays a dm-log-writes log onto a
device; `sqlite_durability_test` uses it to bring its replay device to each
FLUSH of the cache disk's log (`test/qemu/guest/sqlite_durability.sh`, plan
step 12.14).
xfstests is GPL-2.0: it is fetched and built by Bazel as a test-only
dependency and its code is never copied into this repository.

What is here:

- `helpers.bzl` and `BUILD.xfstests`: the programs of `src/` and `ltp/` the
  generic tests run, built from the pinned sources (`XFSTESTS_HELPERS`; the
  comment there says which were left out and why). `:scripts` is `check`,
  `common/`, `tests/generic/` and the shell and awk helpers of `src/`.
- `0001-dcfs-guest.patch`: changes to the pinned xfstests, each with the reason
  in a comment: `_fs_type` reports a FUSE file system with a subtype
  (`fuse.dcfs`) as `fuse`, which is what `init_rc` compares with `FSTYP`; and
  `check` writes the OOM score, in its two places, with `printf`, not `echo`
  (the guest's bash is musl's, whose `echo` makes two `write` calls, the
  second of a newline, which `/proc/<pid>/oom_score_adj` answers with EINVAL
  and bash reports on the test's stderr).
- `shim/glibc_strerror.c`: an `LD_PRELOAD` library giving the guest's musl
  programs glibc's error messages (`strerror`, `strerror_r`, `perror`) for the
  eleven errors on which the two differ. xfstests matches the glibc text ("Operation not supported" decides
  whether `_require_xfs_io_command` says "not run"; golden outputs contain
  "Disk quota exceeded" and the like), and without it a test that glibc
  systems skip fails here.

- `BUILD.xfstests`: the Bazel overlay for `@xfstests`. The autoconf build is
  not used; `config.h` is written by hand for Linux and glibc. Only
  `ltp/fsstress.c` and `ltp/fsx.c` are built, statically linked (dynamically
  under a sanitizer, like pjdfstest), both `testonly`.
- `shim/xfs/xfs.h` and `shim/xfs/xqm.h` (`//third_party/xfstests:xfs_shim`):
  our own stand-ins for xfsprogs' `<xfs/xfs.h>`, which `fsstress.c` and
  `fsx.c` need for the XFS ioctl structures, `xfsctl()` and `getopt_long`, and
  `<xfs/xqm.h>`, which `src/feature.c` takes the quota flags from.

## The runtime (step 17.1)

`test/qemu/guest/xfstests.sh` runs the generic tests against dcfs in a QEMU
guest. The pinned xfstests needs bash, GNU userland, util-linux's `mount` and
`findmnt`, perl and `xfs_io`, and about 100 MB of helper programs, which is
far more than the busybox initramfs every other test boots (it would sit in
the guest's tmpfs). The runtime is therefore a root file system image,
`//test/qemu:xfstests_rootfs` (`scripts/mkxfstests_rootfs.py`), that
`guest/init` mounts and chroots into, the way the NFS test runs in its Debian
image (`qemu_test`'s `rootfs`). Every part comes through Bazel:

- the **Alpine packages** of `@alpine_xfstests_tools` (`MODULE.bazel`, pinned
  to the branch like the others, `third_party/alpine/README.md`): bash,
  coreutils, util-linux (`mount`, `umount`, `findmnt`, `mkswap`, `fallocate`,
  ...), grep, sed, gawk, findutils, diffutils, attr, acl, e2fsprogs' `chattr`
  and `lsattr`, bc, perl, fio, xfsprogs (`xfs_io` and the `mkfs`/`xfs_db`
  family), btrfs-progs and the libraries they link. They are listed by name,
  not resolved with their dependencies, because `xfsprogs-extra` depends on
  python3 (24 MB, for `xfs_scrub_all`); `//test/qemu:xfstests_runtime_test`
  checks that every library the programs need is in the list;
- **xfstests' own files** from the `@xfstests` pin: `check`, `common/`,
  `tests/generic/` (with the `group.list` that `tools/mkgroupfile` would write,
  made from each test's `_begin_fstest` line), the scripts of `src/`, and the
  programs of `src/`, `src/vfs/` and `ltp/` that `BUILD.xfstests` builds
  (`helpers.bzl` lists them);
- busybox (for the applets no Alpine package supplies, `sh` among them), a
  `mount` that gives every FUSE mount of the guest its `dcfs.cache_db`
  (named after the device), `dcfs.allow_other`, and `dcfs.fuse_opt=suid` and
  `=dev` unless the test asks for `nosuid` (`mount -t fuse.dcfs` is what
  xfstests runs for `FSTYP=fuse`, `FUSE_SUBTYP=.dcfs`; a few tests mount with
  `-t fuse`, or with options of their own); a `umount` that waits for the dcfs
  daemons of the unmounted file systems to exit (the daemon holds its cache
  database until then, and xfstests mounts the device again at once); and
  `shim/glibc_strerror.c` preloaded.

The guest mounts `/dev/vdb` as `TEST_DEV` and `/dev/vdc` as `SCRATCH_DEV`
through dcfs (so the backing file system, ext4, xfs or btrfs, is the matrix
variant's), puts the cache databases and `/tmp` on tmpfs (dcfs commits to its
database durably; on the image's disk each commit waited for the host), and
makes a new file system on the scratch device before each batch of tests (for
`FSTYP=fuse` xfstests only deletes the scratch files). `check` costs several
seconds of CPU before its first test in this guest, as much as most "not run"
tests take, so it is given ten tests at a time; the script watches its output
and kills the test that has run for 200 s. The tests are those of group
`auto` less `test/qemu/guest/xfstests.excluded`, in six shards by position
(`guest/xfstests_<n>.sh`).

## What is left out, and the tests it turns into "not run"

The first column is how many of the 737 tests that run were "not run" for the
reason, on the xfs run (the other backing file systems differ by a few). Most
are xfstests' own checks that the file system does not support something
(reflinks and dedupe: FUSE cannot forward the ioctls; fiemap; fcollapse and
the other fallocate modes; encryption, verity, shutdown, freezing, swap files,
quotas, idmapped mounts: README.md's limitations); the rest is the runtime:

| Not run | Reason | What would bring them back |
|---|---|---|
| 76 | `dmsetup`: the dm-flakey, dm-error and dm-thin tests put device-mapper targets under the scratch device, which a FUSE scratch file system cannot sit on | nothing: they cannot apply |
| 32 | `src/godown` is not built (it uses XFS ioctls our shim does not declare) | nothing: dcfs has no shutdown ioctl |
| 31 | quota tools (`quota`, `setquota`) | `quota-tools` from Alpine; dcfs has no quotas, so they would be "not run" anyway |
| 19 | xfstests cannot make a sized scratch file system for `FSTYP=fuse` (`_scratch_mkfs_sized`) | nothing in this repository |
| 26 | libaio and liburing are not built: 15 programs of `src/aio-dio-regress/` ("not built"), `src/feature -A` (7 tests: "kernel does not support asynchronous I/O") and `-R` (4: io_uring) | the two libraries as Bazel builds |
| 11 | `fsverity` | the tool; dcfs has no verity |
| 3 | `duperemove` | the tool; dcfs has no dedupe |
| 4 | `t_open_tmpfiles`, `t_immutable` (libacl and libhandle), `dbtest` (gdbm): not built; `dbench` (1) | the libraries and the tool as Bazel builds |

python3 (24 MB) is left out: only `xfs_scrub_all` and generic/746 (which is not
run on a FUSE file system anyway) use it.

`src/vfs/vfstest` is built without libcap, so the tests of capabilities in its
`--test-core` are skipped.

## What is turned off, and the coverage it loses

- AIO and io_uring (libaio, liburing, not in the guest): fsstress's `aread`,
  `awrite`, `afsync` and `uring_*` operations, fsx's `-A` and `-U` modes.
- libbtrfsutil: fsstress's btrfs subvolume and snapshot operations
  (`subvol_create`, `subvol_delete`, `snapshot`).
- xfsprogs' real headers: the XFS-only ioctls (bulkstat, resvsp, unresvsp,
  direct-I/O alignment query, XFS error injection) are issued through our
  shim and fail on dcfs, which implements no ioctl; the test sets the
  frequency of bulkstat, bulkstat1, resvsp and unresvsp to 0.

## Pin

Declared in `MODULE.bazel` (`http_archive` `xfstests`).

- **Tag** `v2026.05.17` of `github.com/kdave/xfstests` (a mirror of
  `git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git`; the tag's commit is
  `ffc8bad17e5b2f56e48dbac43f7c5ae8ac368fe5`).
- **URL** `https://github.com/kdave/xfstests/archive/refs/tags/v2026.05.17.tar.gz`
- **Integrity** `sha256-N//Niuj3qKt4iBNwj2eun8XvzLzZPg+3/7niSy0W45M=` (as in
  `MODULE.bazel`); the hex sha256 is
  `37ffcd8ae8f7a8ab788813708f67ae9fc5efccbcd93e0fb7ffb9e24b2d16e393`.
- **How the sha256 was obtained** `curl -L` of the URL above and `sha256sum`
  of the file, on 2026-10-07 (step 11.2b); Bazel verifies it on every fetch.
  GitHub's archives of a tag are not guaranteed byte-stable forever: if a
  fetch fails the hash check, re-download, compare the extracted tree with
  the tag's commit, and update the hash.

## Updating the pin

1. Pick a newer tag (`git ls-remote --tags https://github.com/kdave/xfstests`)
   and set `url` and `strip_prefix` in `MODULE.bazel`, and the comment above
   the `http_archive` there.
2. Download the archive, hash it (`sha256sum`, then base64 for `integrity`)
   and set `integrity`.
3. In this file's "Pin" section, set the tag, the tag's commit id
   (`git ls-remote` above, the `^{}` line), the URL, both forms of the hash
   and the date.
4. Build with `bazel build @xfstests//:fsstress @xfstests//:fsx
   @xfstests//:replay-log`; a new tag
   may need more `HAVE_*` in `BUILD.xfstests`'s `config.h` or more
   declarations in the shim header.
5. Run `//test/qemu:stress_short_test_ext4` and the large tier; the set of
   features fsx disables (`guest/stress.sh`) may change with the new tools.
   Run `//test/qemu:sqlite_durability_test` too (replay-log's options).
6. Update the version in `tools/sbom/pins.json` only if its extraction rule
   needs it (it reads `strip_prefix`).
