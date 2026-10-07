# numactl (patched)

Why: libfuse's `fuse_uring.c` (FUSE over io_uring, built into the BCR
libfuse module) calls libnuma, so every dcfs binary links numactl's `libnuma`
(a transitive dependency through `@libfuse`, not a `bazel_dep` of ours). Whether
dcfs should link libnuma at all is under review (the warnings-audit lane);
this patch stays until that is decided.

## Pin

numactl 2.0.19.bcr.1 from the Bazel Central Registry, with
`single_version_override` in `MODULE.bazel` applying the patch here
(`patch_strip = 1`). The module is libfuse's, so its version moves with
libfuse's `MODULE.bazel`: if a new libfuse needs another numactl, change the
`version` of the override and check that the patch still applies.

## Patches

- `0001-no-version-script.patch`: the BCR BUILD overlay passes
  `versions.ldscript` (a node per libnuma release, each with `local: *;`) in
  `linkopts` to every binary that links libnuma, not only to a shared
  library. With lld the `local: *` keeps every other symbol of the
  executable out of `.dynsym`, so clang's static ASan runtime cannot export
  malloc/free to libc.so (ASan tests failed with "bad-free" in libnuma's
  constructor; found by step 7.1); ld.gold warns about the wildcard in two
  nodes at every link. The patch drops the script from `linkopts` (two
  lines): the library is linked statically, so the version nodes mean
  nothing. This is the root-cause fix for gold and lld.
- The `getaddrinfo` warning ("Using 'getaddrinfo' in statically linked
  applications ...") of GNU ld and gold does not arise under lld, which
  ignores `.gnu.warning` sections: a patch removing `affinity.c`'s call is
  unnecessary with the pinned toolchain. `//tools:banned_symbols_test` bans
  `getaddrinfo` instead (the symbol is in `main_static` through libnuma).

## Updating the pin

Change the `version` of the override, check that `libnuma`'s `linkopts` in
the module's BUILD still look like the patch's context (drop the patch if
upstream changed them), regenerate the patch with `diff -u --label a/BUILD.bazel
--label b/BUILD.bazel`, and refresh the lock file with
`bazel mod deps --lockfile_mode=update`.
