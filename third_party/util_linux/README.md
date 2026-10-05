# third_party/util_linux: private static libuuid and libblkid

R3 (`docs/plan/review-fixes.md`). util-linux is built only for its two
libraries, which `third_party/xfsprogs` and `third_party/btrfs-progs`
link statically into `mkfs.xfs`/`xfs_io` and `mkfs.btrfs`/`btrfs`. None of
util-linux's programs are built or used.

e2fsprogs's own bundled `lib/uuid` and `lib/blkid` (which
`third_party/e2fsprogs` uses) are not an alternative: both tools call
libblkid's probe and topology API, which that old private copy lacks.

## Pin

- Version: **2.42.4** (latest release in the `v2.42` series as of
  2026-10-05, from <https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.42/>).
- URL: `https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.4.tar.xz`
- sha256: `fbd62a100ab7bb8746ba0661255c3c48185b1e9021507c624da01fbc696330ec`
  (computed locally; equal to the entry in `sha256sums.asc` in the same
  directory).

### Updating the pin

1. Pick the new release from the directory above.
2. `curl -LO ... && sha256sum ...`, compare with `sha256sums.asc`.
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s
   `util_linux_src`, and `version` of both shims in `BUILD.bazel`.
4. `bazel test //third_party/xfsprogs/... //third_party/btrfs-progs/...`
   (plain and `--config=asan`).

## Configure flags

`BUILD.util_linux`, `UTIL_LINUX_CONFIGURE_OPTIONS`: `--disable-all-programs
--enable-libuuid --enable-libblkid` and every other library
(`libmount`, `libsmartcols`, `libfdisk`, `liblastlog2`) off;
`--disable-shared --enable-static --disable-rpath`; and every optional
integration configure would probe the host for explicitly off (`--disable-nls`,
`--disable-asciidoc`, `--disable-poman`, `--disable-bash-completion`,
`--without-{python,systemd,udev,ncursesw,tinfo,readline,cryptsetup,selinux,audit}`).
Environment: `PKG_CONFIG_LIBDIR=/nonexistent`, `CFLAGS=-Wno-error -g -O2
-fno-sanitize=address,undefined`, `LDFLAGS=-fno-sanitize=address,undefined`
(`-Wno-error` last; the sanitizer cancel as ONE comma-separated word;
reasons in `third_party/xfsprogs/README.md`).

A manual configure and `make` outside Bazel with these flags built both
libraries in about a minute and installed `libuuid.a`, `libblkid.a`, the
two headers and the two `.pc` files; under Bazel only the two archives and
`include/` are exported (rules_foreign_cc does not export `lib/pkgconfig`),
hence the shims.

## pkgconfig shims

`:blkid_pc` and `:uuid_pc` are `pkgconfig_shim`s
(`third_party/qemu/pkgconfig_shim.bzl`) for btrfs-progs, whose configure
reads blkid and uuid only through pkg-config. See
`third_party/btrfs-progs/README.md` ("Dependencies") for how they are used
and the real-execroot caveat. Each shim names both archives; harmless.
xfsprogs does not use pkg-config for them (plain `AC_CHECK_LIB`, found
through the include and library directories rules_foreign_cc derives from
the `deps` CcInfo).

## Test

No test of its own: `//third_party/xfsprogs:smoke_test` and
`//third_party/btrfs-progs:smoke_test` exercise both libraries (mkfs on a
file image needs libblkid's probing and libuuid's UUID generation).
