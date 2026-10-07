# The pinned C/C++ toolchain

Every C and C++ target, in the target and the exec configuration alike, is
built by clang, linked by lld against a statically linked libc++ (with
libc++abi, libunwind and compiler-rt's builtins), all from LLVM's own
release tarball. `MODULE.bazel` registers it with `toolchains_llvm`;
`.bazelrc` sets `BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1`, so the build never
falls back to a host compiler. `//tools:toolchain_test` checks the compiler
and its version.

## Pin

- Module: `toolchains_llvm` 1.11.1 from the Bazel Central Registry (the
  latest 1.x on 2026-10-06).
- LLVM 22.1.8: `LLVM-22.1.8-Linux-X64.tar.xz` from
  `https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/`,
  sha256
  `df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384`
  (1.9 GB). The module carries the sha256 in its release table
  (`toolchain/distributions/github.jsonc`); it was not computed here.
  Obtained 2026-10-06. The upstream commit of the release tag is
  `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1` (`pins.json`'s
  `toolchain_runtime`: the static C++ runtime is in every binary).
- LLVM 22.1.8 and not the newest the module lists (23.1.2): 23.1.2's
  `ld.lld` needs `libicui18n.so.70`, which Ubuntu 24.04 (ICU 74) does not
  have.

## Known limits (to close in step 7.1b)

The toolchain is hermetic for what it compiles with, not for what it runs on
and links against. Verified by building `//dcfs:main_static //tools:testutil
//tools:fhtest` with `--disk_cache=`, `--action_env=PATH=/usr/bin:/bin` and
`--sandbox_block_path` on the host's `gcc`, `cc`, `c++`, `g++`,
`x86_64-linux-gnu-{gcc,g++,ld}`, `ld`, `ld.bfd`, `ld.gold`, `/usr/lib/gcc`
and `/usr/libexec/gcc`. What is still the host's:

- Running the toolchain: the release binaries link the host's shared
  libraries: `ld.lld` needs `libxml2.so.2` (and through it ICU and
  `liblzma`), clang and `llvm-nm` need `libgcc_s` and `libstdc++`, and the
  cc_wrapper needs `/bin/bash` and the host's `mktemp`, `realpath` and `rm`
  (a build with `PATH=/nonexistent` fails). `.github/ci/prepare.sh` installs
  `libxml2` because it uses `--no-install-recommends`.
- Headers: `/usr/local/include`, `/usr/include/x86_64-linux-gnu` and
  `/usr/include` (glibc, and the Linux UAPI headers `linux/openat2.h`,
  `linux/btrfs.h`, `linux/fs.h`, so the kernel interface dcfs is built
  against is the host's), and glibc's static libraries and `crt1.o`.
  There is no sysroot.
- Every link line carries `-L/usr/lib/gcc/x86_64-linux-gnu/13`. Nothing is
  taken from it (the build passes with it blocked); it is a toolchains_llvm
  default for the host layout.
- The libfuse, liburing and numactl patches in `third_party/` exist because of
  this toolchain; see their READMEs.

Two ways to close them, for the plan as 7.1b: a Debian sysroot (the
`rules_distroless` pins exist), or Alpine's clang/lld packages run through
the musl loader (the Phase 24 pattern).

## Upstream issue worth filing

toolchains_llvm 1.11.1's `cc_wrapper.sh` copies a response file line by line
with `while IFS= read -r opt`, which drops a last line without a trailing
newline (ninja's and meson's response files have none; the link then fails
with "clang: error: no input files"). We carried a patch while QEMU was built
from source; with Alpine's QEMU nothing hits it, so it is dropped.

## Updating the pin

Change `bazel_dep(name = "toolchains_llvm")` and `llvm_version` in
`MODULE.bazel`; the version must be one the module lists for Linux x86_64
and whose `ld.lld` runs on the build hosts (`ldd` the unpacked
`bin/ld.lld`). Refresh the lock file with `bazel mod deps
--lockfile_mode=update`, change the version in `tools/toolchain_test.sh` and `tools/toolchain_fixtures/llvm_22.txt`,
and `toolchain_runtime` in `tools/sbom/pins.json` (version, tag and the
commit `git ls-remote` gives for the peeled tag). Then run the hermeticity
build above, `--config=asan //dcfs/...` and `bazel build --config=ubsan
//...`, and update the sha256 here from the module's release table.
