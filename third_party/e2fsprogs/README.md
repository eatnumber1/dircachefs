# third_party/e2fsprogs: pinned, minimal, Bazel-built static mke2fs/debugfs

Phase 4, part c follow-up (`docs/plan/phases/04-pinned-host-tools.md`).
`third_party/debian/README.md`'s "Host tools" section flagged the host's
`mke2fs` as this project's one remaining non-hermetic build tool, and its
"Ownership" section explains why: this host's `mke2fs` is 1.47.0, one
release before `-d <tarball>` (build an ext4 image directly from a tar's
own per-entry uid/gid/mode, no `chown(2)`, no privilege) landed in 1.47.1.
This step fetches and builds e2fsprogs >= 1.47.1 with Bazel instead --
not installed on the host, not checked in -- so
`third_party/debian/scripts/mkrootfs.sh` no longer needs the host's
`mke2fs` at all.

## Pin

- Version: **1.47.4** (released 2026-03-06; the latest stable release as
  of 2026-10-05, confirmed from the upstream release directory
  <https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/> and
  cross-checked against `git tags` on
  <https://github.com/tytso/e2fsprogs> -- no `v1.47.5`/`v1.48.0` exists
  yet).
- URL: `https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4/e2fsprogs-1.47.4.tar.xz`
- sha256: `fd5bf388cbdbe006a3d3b318d983b2948382440acc85a87f1e7d108653e8db0b`
  (computed locally from the downloaded tarball; cross-checked against the
  GPG-signed `sha256sums.asc` published next to it in the same directory
  -- this repository does not verify GPG signatures at fetch time, only
  the sha256 Bazel itself enforces, same trust model as every other
  `http_archive`-pinned dependency here).
- `0001-debugfs-static-link-libarchive.patch` (applied via `patch_args =
  ["-p1"]`, same convention as `third_party/pjdfstest`'s patch): see
  "The debugfs.static gap" below.

### Updating the pin

1. Pick the new release from
   <https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/>.
2. `curl -LO .../v<version>/e2fsprogs-<version>.tar.xz && sha256sum e2fsprogs-<version>.tar.xz`
   and cross-check against that same directory's `sha256sums.asc`.
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s
   `e2fsprogs_src` `http_archive`.
4. `bazel test //third_party/e2fsprogs/...` -- the smoke test will fail
   immediately if the new release's `debugfs/Makefile.in` already fixes
   the `debugfs.static` gap the patch works around (re-check "The
   debugfs.static gap" below and drop the patch if so) or if a Kconfig/
   configure option this README documents was renamed or removed.
5. Re-run `bazel test //...` (the full suite, including
   `//third_party/debian:rootfs`'s build and
   `//third_party/debian:ownership_test`).

## Bundled libarchive

`mke2fs -d <tarball>` is implemented entirely in `misc/
create_inode_libarchive.c` -- there is no fallback tar reader; without
libarchive it is `ENOTSUP` (confirmed by reading the file's own `#if
(!(CONFIG_DLOPEN_LIBARCHIVE || HAVE_ARCHIVE_H) || CONFIG_DISABLE_LIBARCHIVE)`
stub). The host has no `libarchive-dev`, and even if it did, depending on
a host-installed libarchive would reintroduce exactly the non-hermeticity
this step exists to remove. `third_party/e2fsprogs/BUILD.libarchive`
therefore builds a private, minimal, fully static libarchive from
upstream source, used only to link `mke2fs`/`debugfs` -- not exposed as a
general-purpose dependency (hence living here, not in its own
`third_party/libarchive/`).

- Version: **3.8.1** (latest stable release as of 2026-10-05, per
  <https://github.com/libarchive/libarchive/releases>; also the version
  the Bazel Central Registry's own `libarchive` module pins, confirmed via
  `https://bcr.bazel.build/modules/libarchive/metadata.json` -- the BCR
  module itself was considered first and rejected, see "Why not the BCR
  module" below).
- URL: `https://github.com/libarchive/libarchive/releases/download/v3.8.1/libarchive-3.8.1.tar.xz`
- sha256: `19f917d42d530f98815ac824d90c7eaf648e9d9a50e4f309c812457ffa5496b5`
  (computed locally; cross-checked against the BCR module's own
  `source.json` `integrity` field for the same tarball:
  `sha256-GfkX1C1TD5iBWsgk2Qx+r2SOnZpQ5PMJyBJFf/pUlrU=` decodes to the
  same 32 bytes).

### Why not the BCR module

The Bazel Central Registry's `libarchive` module (`3.8.1.bcr.3`) is a real
Bazel-native `cc_library` -- no `configure_make` needed, no transitive
link-order concerns. It was tried first and rejected because its single
`archive` target unconditionally compiles in every format and filter,
with hard `bazel_dep`s on `bzip2`, `lz4`, `xz`, `zstd` and (optionally)
`mbedtls` -- four more pinned third-party C libraries this project would
otherwise have no use for, just to read a plain uncompressed POSIX tar.
Worse, linking that full build into e2fsprogs's `mke2fs.static`/
`debugfs.static` turned out to need more than `--with-libarchive=direct`
can express: e2fsprogs's `configure.ac` only ever sets `$(LIBARCHIVE)` to
the literal string `-larchive` (confirmed by reading it -- there is no
mechanism to make it also emit `-lbz2 -llz4 ... -lzstd`), so the final
static link would need those transitive libraries bundled into one
"fat" `libarchive.a` by hand (a `genrule` combining several `.a` files'
object members with `ar`) just to make a single `-larchive` resolve
everything. Building our own minimal libarchive from upstream source
instead -- disabling every optional backend dcfs never uses (see
`LIBARCHIVE_CONFIGURE_OPTIONS` in `BUILD.libarchive`) -- produces a
`libarchive.a` whose only undefined symbols are glibc's (confirmed with
`nm -u` on the built `.a`, see "Build" below), so a plain
`--with-libarchive=direct` + `-larchive` needs nothing else. No new
`bazel_dep` beyond what's already in this tree.

### Updating the pin

1. Pick the new release from
   <https://github.com/libarchive/libarchive/releases>.
2. `curl -LO https://github.com/libarchive/libarchive/releases/download/v<version>/libarchive-<version>.tar.xz && sha256sum libarchive-<version>.tar.xz`.
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s
   `e2fsprogs_libarchive` `http_archive`.
4. `bazel test //third_party/e2fsprogs/...` -- if the new release added a
   new mandatory dependency for the tar format specifically (unlikely;
   tar/ustar/pax reading has never needed one), the build will fail with
   a real `configure` error, not a silent host fallback (see
   `PKG_CONFIG_LIBDIR=/nonexistent` in `BUILD.libarchive`).

## Configure flags

### e2fsprogs (`BUILD.e2fsprogs`, `E2FSPROGS_CONFIGURE_OPTIONS`)

| Flag | Why |
|---|---|
| `--disable-fuse2fs` | dcfs is its own FUSE filesystem (`third_party/libfuse`'s `bazel_dep`); e2fsprogs's own FUSE binding is unused and would need libfuse headers. |
| `--disable-nls` | Build-time-only tool invoked from genrules/tests, always in the "C" locale. |
| `--disable-uuidd` | The UUID daemon is a long-running service, irrelevant to a one-shot `mke2fs`/`debugfs` invocation. |
| `--disable-tdb` | Only used by an optional e2fsck metadata-csum-seed cache; e2fsck isn't built here. |
| `--disable-backtrace` | Diagnostic-only; a crash already fails the Bazel action loudly. |
| `--disable-imager`, `--disable-resizer`, `--disable-defrag` | e2image/resize2fs/e4defrag are never requested via `targets` (see below) -- keeps the configure line honest. |
| `--enable-libuuid`, `--enable-libblkid` | e2fsprogs's own bundled, private copies (not the host's util-linux libuuid/libblkid) -- a hermetic build must not depend on what's installed on the host. Functionally required: every ext4 filesystem needs a UUID, and mke2fs uses blkid internally for device/fs-type probing. |
| `--with-libarchive=direct` | Forces a direct, static link against `@e2fsprogs_libarchive` instead of the default dlopen-at-runtime mode (see "Bundled libarchive" above and "The debugfs.static gap" below). |
| `--without-udev-rules-dir`, `--without-crond-dir`, `--without-systemd-unit-dir` | This build never runs `make install` for real (see `targets`/`postfix_script` below) -- explicit `--without` keeps `configure`'s result independent of whether the sandbox happens to have udev/cron/systemd unit directories. |
| `--disable-rpath` | No baked-in rpath; these binaries are invoked by absolute Bazel-output path, never expect to find a sibling `.so`. |

`--enable-fsck`, `mmp`, largefile support, year2038, etc. are all left at
their defaults: none of them pull in an extra dependency, and disabling
them would not make the two binaries this build actually produces any
smaller (they're not part of the `targets` list below).

### libarchive (`BUILD.libarchive`, `LIBARCHIVE_CONFIGURE_OPTIONS`)

Every optional format backend/crypto backend is explicitly `--without`'d
(`zlib`, `bz2lib`, `libb2`, `lz4`, `zstd`, `lzma`, `openssl`, `xml2`,
`expat`, `nettle`, `cng`, `iconv`) and the command-line tools are
`--disable`'d (`bsdtar`, `bsdcpio`, `bsdcat`, `bsdunzip`) -- see the
per-flag comments in `BUILD.libarchive` and "Why not the BCR module"
above for the reasoning. `--disable-shared`/`--enable-static` and
`--disable-rpath` match the static-only, no-rpath goal.

## Build

Both `configure_make` rules were worked out with a manual,
non-Bazel `./configure`+`make` run first (test/build first for a build
system integration, same spirit as this project's "test first" rule for
behavior changes), confirming:

- `./configure --disable-shared --enable-static --without-zlib
  --without-bz2lib --without-libb2 --without-lz4 --without-zstd
  --without-lzma --without-openssl --without-xml2 --without-expat
  --without-nettle --without-cng --without-iconv --disable-bsdtar
  --disable-bsdcpio --disable-bsdcat --disable-bsdunzip --disable-rpath`
  (libarchive) succeeds, and `nm -u .libs/libarchive.a` lists only glibc
  symbols (`malloc`, `open`, `stat`, ..., plus libarchive's own bundled
  BLAKE2 reference implementation) -- no `BZ2_*`/`LZ4_*`/`lzma_*`/`ZSTD_*`
  symbol at all.
- A trivial `archive_read_new()`/`archive_read_support_format_tar()`
  program links and runs with `gcc -static` against that `.a`.
- `PKG_CONFIG_LIBDIR=/nonexistent ./configure --with-libarchive=direct
  --enable-libuuid --enable-libblkid ... CPPFLAGS=-I<libarchive headers>
  LDFLAGS=-L<libarchive .libs>` (e2fsprogs) reports "checking for
  archive_read_new in -larchive... yes" / "checking for archive.h...
  yes" -- the direct-link path works.
- `make libs && make -C misc mke2fs.static` succeeds outright.
- An end-to-end round trip -- a hand-built tar with a `root:root`,
  `04755` entry, fed to `mke2fs -q -t ext4 -d test.tar -F test.img`, then
  `debugfs -R 'stat /usr/bin/mount' test.img` -- shows `User: 0 Group: 0`
  and `Mode: 04755` in the output, confirming the whole point of this
  step actually works before wiring it into Bazel at all.

### The debugfs.static gap

`make -C debugfs debugfs.static` **failed** at this point: a real,
reproduced-first link error --

```
/usr/bin/ld: create_inode_libarchive.o: in function `libarchive_available':
.../create_inode_libarchive.c:197: undefined reference to `archive_entry_hardlink'
... (ten more archive_* symbols) ...
/usr/bin/ld: create_inode_libarchive.o: in function `__populate_fs_from_tar':
.../create_inode_libarchive.c:594: undefined reference to `archive_read_new'
collect2: error: ld returned 1 exit status
```

`debugfs/Makefile.in`'s `debugfs.static` recipe links `$(STATIC_LIBS)
$(READLINE_LIB)` and nothing else, while `misc/Makefile.in`'s
`mke2fs.static` recipe correctly appends `$(LIBARCHIVE)` at the end of
its own link line -- an upstream inconsistency (both `.static` targets
compile `create_inode_libarchive.o`, since `debugfs`'s `DEBUG_OBJS`
includes it for its own tar-import commands; only `debugfs`'s static
variant forgets to link what that object needs).
`0001-debugfs-static-link-libarchive.patch` adds `$(LIBARCHIVE)` to
`debugfs.static`'s link line, matching `mke2fs.static`'s own pattern
exactly; `make -C debugfs debugfs.static` then links cleanly (confirmed
manually before being wired into Bazel).

Static linking two glibc NSS-backed functions (`lib/ss/get_readline.c`'s
`dlopen()` for optional readline support, `lib/e2p/ls.c`'s `getgrgid`/
`getpwuid` for `debugfs`'s `ls -l`-style output) produces the usual
glibc link-time **warnings** ("Using 'dlopen'/'getgrgid'/'getpwuid' in
statically linked applications requires at runtime the shared libraries
..."), not errors -- this project only ever calls `debugfs -R 'stat
...'` (`third_party/debian:ownership_test`), which hits neither code
path.

### Sizes

Stripped (this build's `postfix_script` runs `strip`): `mke2fs` ~1.8 MiB,
`debugfs` ~2.0 MiB (measured on this host from the manual build above,
before being wired into Bazel; unstripped, each carries roughly 3-4 MiB
more of debug info). The bundled `libarchive.a` is ~7 MiB
unstripped/pre-link (object code for every reader/writer format it still
supports -- tar, cpio, iso9660, zip, 7z, ... -- `--without-*` above only
removes external *filter*/crypto dependencies, not libarchive's own
built-in format readers; disabling individual formats is not exposed as
a `configure` flag); only the `tar` reader's code and whatever it
transitively pulls in actually ends up link-time-reachable from
`mke2fs`/`debugfs`; the final stripped binary sizes above are what
matters.

## Host tools

None: `ar` (combining libraries, not used by this build -- see "Why not
the BCR module" above for why that was considered and avoided) and
`strip` are the only host tools this build's own scripts would reach
for, and `postfix_script`'s `strip` call is the only one actually used.
Both are as ubiquitous as the `tar`/tools other `third_party/*` READMEs
already note as implicit host dependencies.

## Test

`//third_party/e2fsprogs:smoke_test` (host-side, no root, no kernel --
same spirit as `third_party/qemu`/`third_party/busybox`'s own smoke
tests): confirms both binaries are statically linked, print their pinned
version, and -- the real exercise, not just a `--version` probe -- that
`mke2fs -d <tarball>` on a hand-built tar with a root-owned, setuid entry
produces an image where `debugfs stat` reports that same ownership and
mode. Before `@e2fsprogs_src`/`@e2fsprogs_libarchive` existed in
`MODULE.bazel`, this target failed with "no such package" (see this
step's commit history for the exact failing output).

`//third_party/debian:ownership_test` is the real end-to-end check this
step exists for: it runs the same `debugfs stat` idea against the actual
Debian rootfs image (`//third_party/debian:rootfs`), built by `mke2fs -d`
from `@debian//:flat` directly -- see `third_party/debian/README.md`.

## mke2fs.conf (R3, L5/L10)

`mke2fs.conf` is checked in and is e2fsprogs 1.47.4's own built-in
default profile (`misc/mke2fs.conf.in`, byte for byte apart from a header
comment). Without it an image depends on whichever file `mke2fs` finds:
`$MKE2FS_CONFIG`, else `<sysconfdir>/mke2fs.conf` -- for the Bazel-built
binary that is a path inside the sandbox of the build that produced it,
which does not exist later, so it silently fell back to the built-in
profile -- and, for the host's `mkfs.ext4`, `/etc/mke2fs.conf`, which
differs between distributions and releases (this machine's lacks
`metadata_csum_seed` and `orphan_file`, and uses 1 KiB blocks for
`small`/`floppy` filesystems, which 1.47.4's built-in profile gives a 1 KiB
block size and this machine's does not -- so a scratch disk under 512 MiB
gets different block sizes on different hosts).

Every consumer exports `MKE2FS_CONFIG=<this file>` (an environment
variable, not a wrapper target: the genrule/script already controls its
environment): `third_party/debian/scripts/mkrootfs.sh` takes the path as
its fifth argument; `test/qemu/scripts/run-qemu.sh` must do the same for
the scratch disks it formats. `$(location //third_party/e2fsprogs:mke2fs.conf)`
is the label.

`//third_party/e2fsprogs:mke2fs_conf_test` (host-side) makes a 256 MiB
image with `mke2fs -t ext4` (what `mkfs.ext4` does) under that file and
reads its superblock with the Bazel-built `debugfs -R 'show_super_stats
-h'` (standing in for `dumpe2fs -h`, so the host's e2fsprogs is not
needed): the feature set must be exactly `64bit dir_index dir_nlink
ext_attr extent extra_isize filetype flex_bg has_journal huge_file
large_file metadata_csum metadata_csum_seed orphan_file resize_inode
sparse_super`. It also derives the expected set from the config itself
(so editing one without the other fails) and checks that a different
`MKE2FS_CONFIG` gives a different result (the variable, not a baked-in
path, decides). Run against the host's `/etc/mke2fs.conf` it fails with
`the checked-in config's ext4 features changed` plus the two missing
features, which is the failure this test exists to catch.

### Updating

When the e2fsprogs pin moves, diff `misc/mke2fs.conf.in` of the new
release against `mke2fs.conf` and decide deliberately (a new default
feature changes every ext4 image the tests make); then update the test's
expected list.
