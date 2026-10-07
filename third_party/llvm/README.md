# The pinned C/C++ toolchain

`toolchains_llvm` 1.11.1 (BCR) with LLVM 22.1.8, configured in
`MODULE.bazel`: clang, lld and a statically linked libc++ from LLVM's own
release tarball (fetched by sha256 by the module). It is the toolchain for
every configuration, host and exec alike.

What stays on the host: LLVM's release binaries are dynamically linked
against the host's libc, libstdc++, zlib, libxml2 and ICU (lld), and the
glibc headers and static libraries are the host's (no sysroot yet).

`0001-cc-wrapper-response-file-last-line.patch` (applied to toolchains_llvm
with `single_version_override`): the module's `cc_wrapper.sh` dropped the
last line of a response file without a trailing newline, which is how ninja
and meson write them, so QEMU's link got no inputs.

To update: change `llvm_version` (the newest release the pinned
toolchains_llvm lists for Linux x86_64 and that runs on the build hosts:
`ldd bazel-*/external/*llvm_toolchain_llvm/bin/ld.lld` must find its libraries; 23.1.2
needed ICU 70), refresh the lock file with `bazel mod deps
--lockfile_mode=update`, and change the version in `tools/toolchain_test.sh`.
