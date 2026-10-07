# numactl


`0001-no-version-script.patch` drops the version script from `libnuma`'s
`linkopts`, so that `--config=asan` binaries can export clang's ASan malloc
interposers (see the patch header). Applied with `single_version_override`
in `MODULE.bazel`.

To update: change the version there (the module is `numactl`, version
`2.0.19.bcr.1` today), check that `libnuma`'s `linkopts` still look like the
patch's context (drop the patch if upstream changed it), and refresh the lock
file with `bazel mod deps --lockfile_mode=update`.

Whether dcfs should link libnuma at all (it comes in through libfuse) is
under review by the warnings-audit lane; this patch stays until that is
decided.
