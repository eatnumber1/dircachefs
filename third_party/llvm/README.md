# The pinned C/C++ toolchain

Every C and C++ target, in the target and the exec configuration alike, is
built by clang, linked by lld against a statically linked libc++ (with
libc++abi, libunwind and compiler-rt's builtins), all from LLVM's own
release tarball, against a glibc sysroot of pinned Debian packages.
`MODULE.bazel` registers it with `toolchains_llvm`; `.bazelrc` sets
`BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1`, so the build never falls back to a host
compiler. `//tools:toolchain_test` checks the compiler and its version and
`//tools:toolchain_hermetic_test` that nothing it finds outside the
workspace is the host's (below).

## What `@dcfs_llvm` is

One repository (`llvm.bzl`, step 7.1b), handed to `toolchains_llvm` as its
`toolchain_root` and `sysroot`:

- LLVM's release (clang, lld, libc++, compiler-rt, llvm-ar and the other
  tools), laid out as `toolchains_llvm`'s `BUILD.llvm_repo` expects.
  `extract.py` leaves out what no build step uses (the static libraries of
  LLVM, clang, MLIR and flang, lldb, and the tools of flang and MLIR) and
  every link whose target it left out: 1.9 of the archive's 11.6 GB are
  written. `extract_test` checks the skip list against what `toolchains_llvm`
  refers to (its tool list, the fixed sources of its BUILD template) and that
  no written link dangles.
- `sysroot/`: the target's glibc headers, static libraries and `crt*.o`, and
  the Linux UAPI headers, merged from pinned Debian packages. Debian's absolute
  symlinks are written as relative ones, so the tree is movable. dcfs's
  binaries are statically linked against this glibc: the host's glibc does not
  matter to them.
- `bin/clang.cfg` (and `clang++.cfg`, `clang-cpp.cfg`): the default
  `--sysroot` of every clang the repository provides, and the linker
  (`-fuse-ld=lld`), runtime (`--rtlib=compiler-rt`) and unwinder
  (`--unwindlib=libunwind`) as link-only flags, so a configure script's probe
  (liburing's) sees the same headers and links the same way as the build.
  `clang++.cfg` adds `-stdlib=libc++`. `toolchain_hermetic_test` checks that
  clang reads this file and no other.
- `lib/`: the shared libraries the release's binaries link besides libc:
  libstdc++, libgcc_s, zlib, libxml2 (which `ld.lld` needs) and what libxml2
  loads (ICU, liblzma). The binaries' RUNPATH is `$ORIGIN/../lib`, so they find
  these before the host's. A RUNPATH covers a library's direct dependencies
  only, so the libraries that load others (libxml2, ICU) get `$ORIGIN` as their
  own RUNPATH, set with Alpine's patchelf (`@alpine_patchelf`, run through
  musl's loader like the other Alpine tools) when the repository is fetched;
  the fetch fails if it finds no library to patch.

## What triggers a refetch

The repository is fetched again when any of these changes: an attribute of
`llvm_distribution` in `MODULE.bazel` (a URL, a sha256, a package), `llvm.bzl`,
`extract.py` (its skip list), the hermetic Python interpreter, the
`@alpine_patchelf` repository, or the template `BUILD.llvm_repo.tpl` of
`toolchains_llvm`. The rule watches each of them (`rctx.watch`): a path taken
from a label is not watched by itself, and before it was, editing
`extract.py` ran `bazel fetch` for 1.2 s without refetching, while the same
edit now makes it refetch. A refetch unpacks the archive again:
3.8 CPU minutes, 9.4 minutes of wall time at load 13 on a 4-core machine
(decompressing dominates); writing all of the 12 GB took 12 to 17 minutes at
load 20.

## Pin

LLVM:

- LLVM 22.1.8: `LLVM-22.1.8-Linux-X64.tar.xz` from
  `https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/`,
  sha256
  `df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384`
  (1.9 GB; 12 GB unpacked). The sha256 is the one in `toolchains_llvm`
  1.11.1's release table (`toolchain/distributions/github.jsonc`); it was
  not computed here. Obtained 2026-10-06. The upstream commit of the release
  tag is `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1` (`pins.json`'s
  `toolchain_runtime`: the static C++ runtime is in every binary).
- LLVM 22.1.8 and not the newest the module lists (23.1.2): 23.1.2's
  `ld.lld` needs `libicui18n.so.70`, which Ubuntu 24.04 (ICU 74) does not
  have. With the ICU shipped in `lib/` the host's ICU no longer matters, so
  this limit is gone; moving to LLVM 23 means finding the ICU it needs.
- Module: `toolchains_llvm` 1.11.1 from the Bazel Central Registry.

Debian: every package is a `.deb` named by URL and sha256 in `MODULE.bazel`.
The sha256s are the `SHA256` fields of the `Packages.xz` of the suite and
snapshot named below (obtained 2026-10-07; `Packages.xz` is served over TLS by
snapshot.debian.org, and Bazel's downloader checks each `.deb` against its
hash). The snapshots are those of `third_party/debian`:
`debian/20261006T082722Z` for bookworm and trixie,
`debian-security/20261006T081244Z` for the security update.

| Package | Suite and snapshot | Why |
|---|---|---|
| `libc6`, `libc6-dev` 2.36-9+deb12u14 | bookworm | the sysroot's glibc: headers, `libc.a` and the other static libraries, `crt1.o`, `ld-linux`. `libc6-dev` is shipped (linked statically into the binaries) |
| `linux-libc-dev` 6.12.107-1 | trixie | the sysroot's Linux UAPI headers (`linux/openat2.h`, `btrfs.h`, `fs.h`, `statx.h`). dcfs needs 6.8 or later (`STATX_MNT_ID_UNIQUE`); bookworm's are 6.1. UAPI headers do not depend on the libc |
| `libstdc++6`, `libgcc-s1` 12.2.0-14+deb12u1 | bookworm | run clang, lld and the other LLVM tools (`GLIBCXX_3.4.30`) |
| `zlib1g`, `libxml2`, `libicu72` | bookworm | run `ld.lld` (libxml2, and what it loads) and the other tools |
| `liblzma5` 5.4.1-1+deb12u2 | bookworm-security (`debian-security/20261006T081244Z`) | loaded by libxml2; the security update the rootfs of `third_party/debian` has |

`@alpine_patchelf` is Alpine's `patchelf` of the branch `v3.24`, verified by
the signatures `third_party/alpine/README.md` describes.

## What is still the host's

The host's kernel, Bazel, and what Bazel's own actions use. Verified by
building `//dcfs:main_static //tools:testutil //tools:fhtest` with
`--disk_cache=`, `--action_env=PATH=/usr/bin:/bin` and `--sandbox_block_path`
on: `/usr/include`, `/usr/local/include`, `/usr/lib/gcc`, `/usr/libexec/gcc`,
the host's `gcc`, `cc`, `c++`, `g++`, `x86_64-linux-gnu-{gcc,g++,ld}`, `ld`,
`ld.bfd`, `ld.gold`, every file of the host's `libc6-dev` under
`/usr/lib/x86_64-linux-gnu` (`libc.a`, `crt1.o`, ...), and the host's
`libstdc++`, `libgcc_s`, `libxml2`, `libicu*`, `liblzma` and `libz` shared
objects. What remains:

- **glibc's runtime** (`libc.so.6`, `libm.so.6`, `ld-linux-x86-64.so.2`),
  which runs the LLVM binaries (their interpreter is the host's) and every
  other program of an action. The host's glibc must be 2.36 or newer:
  the release needs 2.34, and Debian's libstdc++6 (12.2) needs 2.36. Ubuntu
  24.04 (2.39) qualifies, Ubuntu 22.04 (2.35) does not. It is the one host
  input of the compiler's run; dcfs's own binaries do not link it.
- **bash, `mktemp`, `realpath`, `rm`**: `toolchains_llvm`'s `cc_wrapper.sh`.
  Bazel's genrules and `sh_test`s need a shell and coreutils anyway.
- **`/bin/sh` and `dirname`**: the wrapper of `@alpine_patchelf` (and of every
  other Alpine tool), which `llvm.bzl` runs while fetching.
- **The python-build-standalone interpreter** of `rules_python`: Bazel fetches
  it, so it is pinned, but it is a binary that runs on the host's glibc. It
  runs `extract.py` and `apk.py`.
- Fetching: `rules_distroless` (the Debian rootfs of `third_party/debian`)
  runs the host's `tar` and `grep` in its repository rules; `@dcfs_llvm`
  itself uses neither (Bazel and `extract.py` unpack the archives).

The libfuse, liburing and numactl patches in `third_party/` exist because of
this toolchain; see their READMEs.

## Updating the pins

LLVM: change `url`, `sha256` and `strip_prefix` of `llvm_distribution` in
`MODULE.bazel`, `llvm_version` of the `toolchains_llvm` tag and
`llvm_major_version`; the version must be one `toolchains_llvm` lists for
Linux x86_64 (take the sha256 from its `github.jsonc`). Run `ldd` on the
unpacked `bin/ld.lld` to see what else it needs. After a new LLVM or a new
`toolchains_llvm`, check the skip list: `bazel test
//third_party/llvm:extract_test` compares it with the new template (add to
`KNOWN_NEEDED` what the new `toolchains_llvm` refers to), then build
everything: a file the list leaves out shows up as a missing input or a
dangling link. Change the version in
`tools/toolchain_test.sh` and `tools/toolchain_fixtures/llvm_22.txt`, and
`toolchain_runtime` in `tools/sbom/pins.json` (version, tag and the commit
`git ls-remote` gives for the peeled tag). Then run the hermeticity build
above, `--config=asan //dcfs/...` and `bazel build --config=ubsan //...`.

Debian: pick a snapshot timestamp (a real run; the listing is
`https://snapshot.debian.org/archive/debian/?year=2026&month=10`), then for
each package look up its `Filename` and `SHA256` in
`.../dists/<suite>/main/binary-amd64/Packages.xz` of that snapshot (the
`debian-security` archive has its own timestamps) and put the URL and sha256
in `MODULE.bazel`. Keep `libc6` and `libc6-dev` at one version. Keep
`libstdc++6`, `libgcc-s1` and the other runtime libraries at versions whose
glibc requirement the build hosts meet (`objdump -T lib/*.so* | grep -o
'GLIBC_[0-9.]*' | sort -uV | tail -1` in the unpacked repository). A new
`libicu` soname changes the file names in `lib/`; nothing else names them.
Then add a row for every new binary package to `tools/sbom/debian_sources.tsv`
(its source package and versions, from the same `Packages.xz`; a fifth column
names a release other than bookworm), or `//tools/sbom:sbom_test` fails.
Changing any pin refetches the whole repository (see "What triggers a
refetch").

## Upstream issue worth filing

toolchains_llvm 1.11.1's `cc_wrapper.sh` copies a response file line by line
with `while IFS= read -r opt`, which drops a last line without a trailing
newline (ninja's and meson's response files have none; the link then fails
with "clang: error: no input files"). We carried a patch while QEMU was built
from source; with Alpine's QEMU nothing hits it, so it is dropped.
