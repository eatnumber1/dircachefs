# liburing

The BCR module `liburing` (a dependency of `libfuse`), with one patch.

`0001-absolute-cc-for-configure.patch`: the module's `generate_headers`
genrule runs `./configure` after `cd`-ing into the package directory, so the
pinned clang's execroot-relative `$(CC)` is not found and every probe fails
(step 7.1). The patch makes `CC` absolute.

To update: change the version in the `single_version_override` in
`MODULE.bazel`, check whether the genrule still `cd`s before `configure`
(drop the patch if upstream fixed it), and refresh the lock file with
`bazel mod deps --lockfile_mode=update`.
