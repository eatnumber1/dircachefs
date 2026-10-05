# third_party/btrfs-progs: pinned, Bazel-built static mkfs.btrfs and btrfs

R3 (`docs/plan/review-fixes.md`, audit finding L5). The QEMU tests used the
host's `mkfs.btrfs` to format scratch disks. This builds btrfs-progs from
source with Bazel instead, so the btrfs filesystems under test are the same
on every machine. `mkfs.btrfs` is what the tests use today; `btrfs` is built
because later phases (subvolumes, qgroups, `inspect-internal`) drive the
filesystem with it.

## Pin

- Version: **v7.1** (released 2026-07-14; the latest stable release as of
  2026-10-05, from the upstream directory
  <https://mirrors.edge.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/>;
  the previous ones are v7.0 and v6.19.1).
- URL: `https://mirrors.edge.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/btrfs-progs-v7.1.tar.xz`
- sha256: `d1f55cc2971398c9142eaa79d203e63d586a3b4b867f956664a1d68322cd4e34`
  (computed locally; equal to the entry in the `sha256sums.asc` published
  in the same directory. GPG signatures are not verified at fetch time).

### Updating the pin

1. Pick the new release from the directory above.
2. `curl -LO .../btrfs-progs-v<version>.tar.xz && sha256sum ...` and
   compare with that directory's `sha256sums.asc`.
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s
   `btrfs_progs_src` `http_archive`, and the version argument of
   `smoke_test` in `BUILD.bazel` (without the `v`).
4. `bazel test //third_party/btrfs-progs/... && bazel test --config=asan
   //third_party/btrfs-progs/...`. Read `./configure --help` of the new
   release for added mandatory dependencies.

## Dependencies

| Library | Provided by | Why |
|---|---|---|
| libblkid, libuuid | `@util_linux_src` (`third_party/util_linux`) | Mandatory; `mkfs.btrfs` probes devices with libblkid. |
| zlib | the BCR `zlib` module (the same one QEMU's build uses) | Mandatory (`mkfs.btrfs --rootdir --compress zlib`, `btrfs` itself). |

configure finds all three **only through pkg-config** (`PKG_CHECK_MODULES`,
and `PKG_STATIC`, which runs `pkg-config --libs --static` and fails
without a `.pc` file), and rules_foreign_cc does not export the `.pc`
files util-linux writes. So `third_party/util_linux:{blkid,uuid}_pc` and
`third_party/qemu:zlib_pc` are `pkgconfig_shim`s
(`third_party/qemu/pkgconfig_shim.bzl`) naming the Bazel-built static
archives, and `PKG_CONFIG_LIBDIR` (not `_PATH`, which only adds a
directory) points at exactly those three. The shim paths are relative to
the `.pc` file's real location (the shim directory is symlinked into the
sandbox and pkg-config resolves `..` physically), i.e. into the real
execroot, so the archives must be built there: the first attempt failed to
link with `cannot open .../third_party/qemu/zlib_pc/../../../../../../bazel-out/.../libz.a`
until `deps = ["@util_linux_src//:uuid_blkid", "@zlib"]` was added (the
same arrangement `third_party/qemu/BUILD.qemu` has for glib and zlib).

## Configure flags

`BUILD.btrfs_progs`, `BTRFS_PROGS_CONFIGURE_OPTIONS`:

| Flag | Why |
|---|---|
| `--disable-documentation`, `--disable-python` | No man pages (asciidoc/sphinx), no Python bindings. |
| `--disable-convert` | `btrfs-convert` needs libext2fs and reiserfs libraries. |
| `--disable-zoned` | Zoned (SMR) support needs `linux/blkzoned.h` and serves devices the guests do not have; off so the host's kernel headers cannot change the build. |
| `--disable-zstd`, `--disable-lzo` | Only add compression to `mkfs.btrfs --rootdir`, `btrfs restore` and `btrfs receive`; the test filesystems are created empty, and the kernel (not mkfs) decides what a mount can compress. (BCR has `zstd` and `lzo` modules if a later phase needs them; `--version` prints `-LZO -ZSTD`.) |
| `--disable-libudev` | Multipath device resolution only. |
| `--disable-backtrace` | Diagnostic backtraces (execinfo). |
| `--disable-shared` | Static archives only. |

Environment: `PKG_CONFIG_LIBDIR` as above, `CFLAGS=-Wno-error -g -O2
-fno-sanitize=address,undefined`, `LDFLAGS=-fno-sanitize=address,undefined`
(why: `third_party/xfsprogs/README.md`, "Configure flags": `-Wno-error`
must come last, and the sanitizer cancel is one comma-separated word).

## Build

`configure_in_place = True`, then the Makefile's own fully static targets
`make mkfs.btrfs.static btrfs.static` (`STATIC_LDFLAGS = ... -static
-Wl,--gc-sections`); both are stripped and copied out. Checked by hand
first (configure and `make`, outside Bazel, against the same libraries):
both link, `file` says `statically linked`, `mkfs.btrfs --version` and
`btrfs --version` print `v7.1` and a second line of compiled-in features
(`-EXPERIMENTAL -INJECT +STATIC -LZO -ZSTD -UDEV +FSVERITY -ZONED
CRYPTO=builtin`), which is why the smoke test compares only the first line.

### Sizes (stripped, measured)

`mkfs.btrfs` 1,830,992 bytes; `btrfs` 2,500,504 bytes.

## Host tools

Still the host's: a C compiler, `strip`, `tar`, `sh`; rules_foreign_cc
builds its own `make`, `m4` and `pkgconf` (the `PKG_CONFIG` it sets). No
host btrfs-progs, libblkid, libuuid or zlib is used.

## Test

`//third_party/btrfs-progs:smoke_test` (host-side, no root, no kernel):

- both binaries are `statically linked` and contain no sanitizer runtime
  strings (passes both plain and under `--config=asan`);
- the first line of `mkfs.btrfs --version` and `btrfs --version` is exactly
  the pinned version;
- `mkfs.btrfs -q -f` on a 256 MiB file writes the `_BHRfS_M` magic at
  offset 65600, and `btrfs inspect-internal dump-super` reads it back.

Test first: before the targets existed it failed with `missing input file
'//third_party/btrfs-progs:mkfs_btrfs'`.
