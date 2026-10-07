# pandoc (pinned)

Why: the `dcfs(8)` man page (`//man:dcfs.8`) is generated from
`/README.md` by pandoc inside a Bazel genrule, and the tests read it back
with pandoc. pandoc runs only through Bazel (`@pandoc//:bin/pandoc`),
never from the host.

## Pin

- **pandoc 3.12** (latest 3.x as of 2026-10-06; Bazel repository
  `@pandoc`, an `http_archive` of the statically linked Linux amd64
  tarball).
  - URL: `https://github.com/jgm/pandoc/releases/download/3.12/pandoc-3.12-linux-amd64.tar.gz`
  - sha256: `67d7d011fed8c8543306022b985b9b2499ab9b74818df91d8727c7e9ebc5ba06`

`third_party/pandoc/` holds only the BUILD overlay (`BUILD.pandoc`).

## Updating the pin

1. Pick a release at <https://github.com/jgm/pandoc/releases> and download
   its `pandoc-<ver>-linux-amd64.tar.gz`.
2. `sha256sum` it; compare with the digest GitHub shows for the asset.
3. Update `urls`, `strip_prefix` and `sha256` of `http_archive(name =
   "pandoc")` in `MODULE.bazel`, and this file.
4. `bazel mod deps --lockfile_mode=update`, then `bazel test //man/...`.
