# The pinned C/C++ toolchain

`toolchains_llvm` 1.11.1 (BCR) with LLVM 22.1.8, configured in
`MODULE.bazel`: clang, lld and a statically linked libc++ from LLVM's own
release tarball (fetched by sha256 by the module). It is the toolchain for
every configuration, host and exec alike.

To update: change `llvm_version` (the newest release the pinned
toolchains_llvm lists for Linux x86_64 and that runs on the build hosts:
`ldd bazel-*/external/*llvm_toolchain_llvm/bin/ld.lld` must find its
libraries; 23.1.2 needed ICU 70), refresh the lock file with `bazel mod deps
--lockfile_mode=update`, and change the version in `tools/toolchain_test.sh`.

## Known limits (step 7.1; to close in 7.1b)

- LLVM's release binaries (clang, lld) are dynamically linked against the
  host's libc, libstdc++, zlib, libxml2 and ICU. That is why LLVM 23.x could
  not be used: its lld wants ICU 70.
- The glibc headers and static libraries come from the host: there is no
  sysroot.
- The module's `cc_wrapper.sh` needs the host's `mktemp`, `realpath` and `rm`
  (a build with `PATH=/nonexistent` fails).

Two ways to close them, for the plan as 7.1b: a Debian sysroot (the
rules_distroless pins already exist), or Alpine's clang/lld packages run
through the musl loader (the Phase 24 pattern).

## Upstream issue worth filing

toolchains_llvm 1.11.1's `cc_wrapper.sh` copies a response file line by line
with `while IFS= read -r opt`, which drops a last line without a trailing
newline (ninja's and meson's response files have none; the link then fails
with "clang: error: no input files"). We carried a patch while QEMU was built
from source; with Alpine's QEMU nothing hits it, so it is dropped.
