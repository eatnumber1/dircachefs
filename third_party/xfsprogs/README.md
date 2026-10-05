# third_party/xfsprogs: pinned, Bazel-built static mkfs.xfs and xfs_io

R3 (`docs/plan/review-fixes.md`, audit finding L5). The QEMU tests used the
host's `mkfs.xfs` (whatever version and defaults the host has) to format
scratch disks. This builds xfsprogs from source with Bazel instead, so the
XFS filesystems under test are identical on every machine. `mkfs.xfs` is
what the tests use today; `xfs_io` is built because later phases (project
quotas, reflink, xfs ioctls) drive the filesystem with it.

## Pin

- Version: **7.2.0** (released 2026-09-17; the latest stable release as of
  2026-10-05, from the upstream release directory
  <https://mirrors.edge.kernel.org/pub/linux/utils/fs/xfs/xfsprogs/>,
  where 7.1.1 is the previous one).
- URL: `https://mirrors.edge.kernel.org/pub/linux/utils/fs/xfs/xfsprogs/xfsprogs-7.2.0.tar.xz`
- sha256: `501dfa363cd8e19997a4a1a71f70ded88241b45e9db233e8875c23b80520cc83`
  (computed locally; equal to the entry in the `sha256sums.asc` published
  in the same directory. This repository does not verify GPG signatures at
  fetch time, only the sha256 Bazel enforces).

### Updating the pin

1. Pick the new release from the directory above.
2. `curl -LO .../xfsprogs-<version>.tar.xz && sha256sum xfsprogs-<version>.tar.xz`
   and compare with that directory's `sha256sums.asc`.
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s
   `xfsprogs_src` `http_archive`, and the version argument of
   `smoke_test` in `BUILD.bazel`.
4. `bazel test //third_party/xfsprogs/... && bazel test --config=asan
   //third_party/xfsprogs/...`. A new mandatory library dependency shows up
   as a configure error ("could not find a valid ... library"); build it
   the way `third_party/urcu` and `third_party/util_linux` are built.

## Dependencies (all private, static, built by Bazel)

xfsprogs's configure requires four libraries; none comes from the host:

| Library | Provided by | Why |
|---|---|---|
| libuuid, libblkid | `@util_linux_src` (`third_party/util_linux`) | UUID generation; libblkid's probe and topology API (`blkid_probe_get_topology`) for sector-size and alignment detection. e2fsprogs's bundled copies lack the topology API. |
| liburcu | `@userspace_rcu_src` (`third_party/urcu`, userspace-rcu 0.15.7) | libxfs's caches use RCU; `<urcu.h>` is a hard configure requirement. |
| libinih | `@inih_src` (`third_party/inih`, inih r62) | `mkfs.xfs -c options=...` config-file parsing (`ini_parse`); a hard configure requirement. |

The Bazel Central Registry has `libuuid`, `zlib`, `zstd` and `lzo`
modules, but none of these four, and `libuuid` alone would not give
libblkid; building util-linux gives both from one pinned source.

### userspace-rcu 0.15.7

- URL: `https://lttng.org/files/urcu/userspace-rcu-0.15.7.tar.bz2` (the
  newest release; 2026-09-11)
- sha256: `2556b83adc0f9b3ac8024e613e17d014d04c4c49110604ce55fcb14eae32edd3`
  (equal to the `.sha256` file published next to the tarball)
- `configure --disable-shared --enable-static`, plus `CXXFLAGS=-Wno-error
  ...`: configure insists on a C++11 compiler (`AX_CXX_COMPILE_STDCXX`,
  mandatory) although nothing built is C++. Its probe program was rejected
  ("A compiler with support for C++11 language features is required")
  until `-Wno-error` followed the leaked `-Werror` in `CXXFLAGS`; setting
  `CXX=g++` alone did not help.

### inih r62

- URL: `https://github.com/benhoyt/inih/archive/refs/tags/r62.tar.gz`
  (latest release, 2025-09-11)
- sha256: `9c15fa751bb8093d042dae1b9f125eb45198c32c6704cd5481ccde460d4f8151`.
  Upstream publishes no checksum; this is the hash of GitHub's generated
  tag archive as downloaded on 2026-10-05 (GitHub's archives are not
  guaranteed byte-stable forever: if the fetch ever fails on the hash,
  re-verify the tag's tree and update it).
- Two files, so a plain `cc_library` (`third_party/inih/BUILD.inih`);
  upstream's own build is meson-only. Default options (not
  `distro_install`), as upstream's `meson.build` has them.

## Configure flags

`BUILD.xfsprogs`, `XFSPROGS_CONFIGURE_OPTIONS`:

| Flag | Why |
|---|---|
| `--enable-shared=no` | xfsprogs's own spelling: libxfs & co. are static libtool archives only. |
| `--disable-scrub`, `--enable-libicu=no`, `--disable-healer` | `xfs_scrub` and `xfs_healer` are online-repair daemons needing libicu, systemd, fanotify; nothing here mounts xfs for them. |
| `--disable-gettext` | No translated message catalogs, no gettext tools. |
| `--enable-editline=no`, `--enable-termcap=no` | xfs_io's line editing: off so a host with libedit cannot change the result. |
| `--enable-lib64=no`, `--enable-librt=no` | Probes of the host's multilib layout and `-lrt` (merged into libc since glibc 2.34); the install tree is never used. |
| `--without-systemd-unit-dir`, `--without-crond-dir`, `--without-udev-rule-dir` | Install-path probes: the build never installs (the two binaries are copied out), so make the result independent of the sandbox. |

Environment: `PKG_CONFIG_LIBDIR=/nonexistent` (no host pkg-config
lookups), `CFLAGS=-Wno-error -g -O2 -fno-sanitize=address,undefined -I...`,
`LDFLAGS=-fno-sanitize=address,undefined`. Why each:

- **`-Wno-error`**: the project-wide `build --copt -Werror` leaks into
  rules_foreign_cc's `CFLAGS` and would turn warnings in xfsprogs's own code
  into failures. It must come last: rules_foreign_cc appends this value to
  its own `CFLAGS` (same lesson as `third_party/e2fsprogs`).
- **`-fno-sanitize=address,undefined` as one word**: under `--config=asan`
  / `--config=ubsan` the global `--copt`/`--linkopt -fsanitize=...` leak the
  same way and would link a sanitizer runtime into a host-side image tool.
  The comma-separated single argument cancels both earlier flags.
- **`-I$EXT_BUILD_DEPS/{uuid_blkid,urcu}/include` and `-I.../inih_src`**:
  rules_foreign_cc puts the dependencies' include directories into
  `CPPFLAGS`, which is enough for configure's probes but which
  xfsprogs's generated makefiles ignore (`include/builddefs.in` forwards
  `CFLAGS` and `LDFLAGS` only), so the compile of `<urcu.h>` failed
  ("urcu.h: No such file or directory") until they were repeated in
  `CFLAGS`.

## Build

`configure_in_place = True` (xfsprogs builds in its tree), then
`make headers` and `make LLDFLAGS=-all-static mkfs io`. The top-level
Makefile makes each tool directory depend on the libraries it links.
**`LLDFLAGS=-all-static` is what makes the binaries static**: both
`mkfs/Makefile` and `io/Makefile` set `LLDFLAGS = -static-libtool-libs`,
and a plain `-static` in `LDFLAGS` is only libtool's "prefer static
libtool archives": the first manual build produced dynamically linked PIE
executables (`file`: "dynamically linked, interpreter
/lib64/ld-linux-x86-64.so.2") until the make variable on the command line
overrode it. Both binaries are then stripped.

Static linking glibc's `getpwnam`/`getgrnam` produces the usual linker
warnings (the binaries print no NSS names we use).

### Sizes (stripped, measured)

`mkfs.xfs` 2,022,432 bytes; `xfs_io` 1,319,184 bytes.

## Host tools

Still the host's: a C compiler, `make` via Bazel's own toolchain
(rules_foreign_cc builds its own `make` and `m4`), `strip`, `tar`, `sh`.
Nothing else: no host xfsprogs, libuuid, libblkid, liburcu, libinih or
pkg-config result is used (`PKG_CONFIG_LIBDIR=/nonexistent`).

## Test

`//third_party/xfsprogs:smoke_test` (host-side, no root, no kernel):

- both binaries are `statically linked` (`file`) and contain none of
  `AddressSanitizer`, `UndefinedBehaviorSanitizer`, `__asan_init`,
  `__ubsan_handle` (the runtime would be in a static binary built with
  the leaked flags; passes under `--config=asan` with the flags as written);
- `mkfs.xfs -V` and `xfs_io -V` print exactly the pinned version;
- `mkfs.xfs -q -f` on a 512 MiB file writes an `XFSB` superblock, and
  `xfs_io -c stat` runs on a regular file.

Test first: before the targets existed it failed with `missing input file
'//third_party/xfsprogs:mkfs_xfs'`.

## Under `--config=asan`

`bazel test --config=asan //third_party/...` passes. Two leaks had to be
cancelled for the whole chain, found the hard way: the libraries and
binaries (`-fno-sanitize=address,undefined` in each `configure_make`'s
`CFLAGS`/`LDFLAGS`), and libinih, a plain `cc_library` that the global
`--copt -fsanitize=address` instrumented, leaving undefined `__asan_*`
symbols that made xfsprogs's configure report "could not find a valid inih
library" (`third_party/inih/BUILD.inih` now has the cancel in its `copts`;
target `copts` come after `--copt`).
