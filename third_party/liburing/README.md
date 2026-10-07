# liburing (patched)

Why: libfuse (the BCR module) depends on liburing for FUSE over io_uring, so
every dcfs binary links it (a transitive dependency, not a `bazel_dep` of
ours).

## Pin

liburing 2.14 from the Bazel Central Registry, with `single_version_override`
in `MODULE.bazel` applying the patch here (`patch_strip = 1`).

## Patches

- `0001-absolute-cc-for-configure.patch`: the module's `generate_headers`
  genrule runs `./configure` after `cd`-ing into the package directory, so
  the pinned clang's execroot-relative `$(CC)` is not found and every probe
  fails ("has_idtype_t no", and the generated `compat.h` then redefines
  `idtype_t`; step 7.1). The patch makes `CC` absolute and links the probes
  with the toolchain's lld and compiler-rt (`-fuse-ld=lld
  -rtlib=compiler-rt`), so no host `ld` and no host GCC file (`crtbeginS.o`,
  libgcc) is used.

## Updating the pin

Change the `version` of the override, check whether the genrule still `cd`s
before `configure` (drop the patch if upstream fixed it), and refresh the
lock file with `bazel mod deps --lockfile_mode=update`. Verify with a build
that blocks the host compilers (`third_party/llvm/README.md`).
