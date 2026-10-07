# pjdfstest (pinned)

<<<<<<< HEAD
Why: pjdfstest is the POSIX filesystem conformance suite
(`docs/conformance.md` has the results); the pjdfstest QEMU tests run it
against dcfs in a guest (`test/qemu/guest/pjdfstest*.sh`, plan steps 4.5
and 5.2).

What is here:

- `BUILD.pjdfstest`: the Bazel overlay for `@pjdfstest`. The upstream
  autoconf build is not used; `config.h` is written by hand for Linux and
  glibc.
- `0001-linux-portability.patch`: two fixes for the busybox-only guest:
  `tests/conf`'s filesystem-type detection and `tests/misc.sh`'s use of
  `openssl md5`, which the guest does not have.

## Pin

Declared in `MODULE.bazel` (`http_archive` `pjdfstest`).

- **Commit** `85a8aea9e685999ef0540392fd80535f873d7ff7`, the tip of
  `master` of `github.com/pjd/pjdfstest` on 2026-09-27 (upstream tags no
  releases).
- **URL** `https://github.com/pjd/pjdfstest/archive/85a8aea9e685999ef0540392fd80535f873d7ff7.tar.gz`
- **Integrity** `sha256-IAXN2Dt2IEF3zxNnkrHyBYp0GLT8WyA/USdKaYVH11Q=`
  (as recorded in `MODULE.bazel`).

## Updating the pin

1. Pick the new commit of `master` and set `url` and `strip_prefix` in
   `MODULE.bazel`.
2. Download the archive, hash it (`sha256sum`, then base64 for `integrity`)
   and set `integrity`.
3. Check the patch still applies (`bazel build @pjdfstest//...`), then run
   the pjdfstest tests and update the expected-failure lists
   (`test/qemu/guest/pjdfstest.*.expected_failures`) and
   `docs/conformance.md` if the suite's results changed.
=======
Why: pjdfstest is the POSIX filesystem conformance suite. `pjdfstest_test`
(`//test/qemu`) runs all of it against a dcfs mount and against the raw
backing filesystem, so the two can be compared (`docs/conformance.md`;
`docs/design.md` has the test table). It is built and run only through Bazel
(`@pjdfstest`), never from the host.

## Pin

- **pjdfstest master commit `85a8aea9e685999ef0540392fd80535f873d7ff7`**
  (the current master as of 2026-09-27; the project makes no releases that
  we could pin instead).
  - URL: `https://github.com/pjd/pjdfstest/archive/85a8aea9e685999ef0540392fd80535f873d7ff7.tar.gz`
  - sha256: `2005cdd83b76204177cf136792b1f2058a7418b4fc5b203f51274a698547d754`
    (the hex form of the `integrity = "sha256-..."` value in `MODULE.bazel`).

`third_party/pjdfstest/` holds only what we write for it:

- `BUILD.pjdfstest`, the Bazel overlay: no autoconf, so a genrule writes
  the `config.h` that `configure.ac` would generate, by hand, for Linux and
  glibc (the comments in the file say how each value was derived).
- `0001-linux-portability.patch`, two guest-shell fixes: `tests/conf`
  detects the filesystem type with `df -PT` (fixed to `EXT4`, so a run on
  the dcfs mount and a run on the raw ext4 make the same `supported()`
  and `todo()` decisions), and `tests/misc.sh` calls `openssl md5`, which
  the busybox-only guest lacks (`md5sum` instead).
- `BUILD.bazel`, which exports those two files.

## Updating the pin

1. Pick a commit of <https://github.com/pjd/pjdfstest> (master's head, or
   the commit a conformance finding needs).
2. Download `https://github.com/pjd/pjdfstest/archive/<commit>.tar.gz` and
   `sha256sum` it.
3. In `MODULE.bazel`'s `http_archive(name = "pjdfstest")` update `url`,
   `strip_prefix` and `integrity` (`sha256-` plus the base64 of the digest:
   `sha256sum -b <file> | cut -d' ' -f1 | xxd -r -p | base64`), and this
   file.
4. Check that `0001-linux-portability.patch` still applies (`bazel build
   @pjdfstest//...`); regenerate it against the new tree if not. If new
   `.t` files use libc functions `BUILD.pjdfstest`'s `config.h` does not
   define, add them there.
5. `bazel mod deps --lockfile_mode=update`, then `bazel test
   //test/qemu:pjdfstest_test_ext4` (the other two filesystems are
   `enormous`; CI runs them). Compare the failures with
   `docs/conformance.md`.
>>>>>>> edac220 (25.1: ASSERT_OK_AND_ASSIGN once; third_party/pjdfstest/README.md)
