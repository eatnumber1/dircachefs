# xfstests (pinned): fsstress and fsx

Why: `fsstress` (random concurrent namespace and data operations) and `fsx`
(data, size and mmap correctness) are the standard file system stress tools;
the `stress_*` QEMU tests run them against dcfs and compare the cache with the
backing file system afterwards (`test/qemu/guest/stress.sh`, plan step 11.2b).
xfstests is GPL-2.0: it is fetched and built by Bazel as a test-only
dependency and its code is never copied into this repository.

What is here:

- `BUILD.xfstests`: the Bazel overlay for `@xfstests`. The autoconf build is
  not used; `config.h` is written by hand for Linux and glibc. Only
  `ltp/fsstress.c` and `ltp/fsx.c` are built, statically linked (dynamically
  under a sanitizer, like pjdfstest), both `testonly`.
- `shim/xfs/xfs.h` (`//third_party/xfstests:xfs_shim`): our own stand-in for
  xfsprogs' `<xfs/xfs.h>`, which `fsstress.c` and `fsx.c` need for the XFS
  ioctl structures, `xfsctl()` and `getopt_long`.

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
- **Integrity** `sha256-N//K3TdsbZ4SBR1gX1LJqXPzczKHjvdv8+zmNpnbLI8=`; the hex
  sha256 is `37ffcd8ae8f7a8ab788813708f67ae9fc5efccbcd93e0fb7ffb9e24b2d16e393`.

## Updating the pin

1. Pick a newer tag (`git ls-remote --tags https://github.com/kdave/xfstests`)
   and set `url` and `strip_prefix` in `MODULE.bazel`.
2. Download the archive, hash it (`sha256sum`, then base64 for `integrity`)
   and set `integrity`.
3. Build with `bazel build @xfstests//:fsstress @xfstests//:fsx`; a new tag
   may need more `HAVE_*` in `BUILD.xfstests`'s `config.h` or more
   declarations in the shim header.
4. Run `//test/qemu:stress_short_test_ext4` and the large tier.
