# pjdfstest (pinned)

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
