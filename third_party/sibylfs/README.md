# SibylFS (pinned)

Why: SibylFS is an executable specification of POSIX file systems (Ridge,
Sheets, Tuerk, Giugliano, Madhavapeddy and Sewell, SOSP 2015). Plan step
12.18b pins its checker so that a guest test can run SibylFS's test suite on a
dcfs mount and on the backing file system under it, ask the model whether
each trace is allowed, and fail only where the model rejects dcfs and accepts
the backing file system. It is a second oracle beside `pjdfstest`: it checks
sequences of system calls against a model, where pjdfstest checks single calls
against expected results. Plan step 12.16 replaces it with a checker built
from `formal/Ideal.tla`, after which these pins are deleted. The test suite is
not pinned yet (see "What we do with it").

What is here:

- `BUILD.checker`: the Bazel overlay for `@sibylfs_checker`, which exports
  the two binaries the guest runs.
- `BUILD.bazel`: `testonly` filegroups over the pin. Nothing from it is
  reachable from a target that is not `testonly`.

Nothing of SibylFS is checked in.

## Pin

Declared in `MODULE.bazel`.

- **Checker**: `@sibylfs_checker`, the prebuilt x86-64 Linux binaries
  (`fs_test_check`, `fs_test_posix`, `fs_test`, `run_trace`, `tgen`) of
  release `2015-10-05` of `github.com/sibylfs/sibylfs_binaries`.
  - URL `https://github.com/sibylfs/sibylfs_binaries/releases/download/2015-10-05/sibylfs_linux_amd64.tar.gz`
  - sha256 `080c5bced08bbb34dedde43cdbd4f8fd3e1c5d678c60dff7067cc6f189208879`,
    from downloading the asset on 2026-10-16 (GitHub publishes no digest for
    it); the investigation of 2026-10-16 (`docs/plan/notes/sibylfs-checker-12.18.md`,
    correction header) found it behaves as the checker built from source.
  - The guest runs only `fs_test_posix` and `fs_test_check`. The binaries
    are dynamically linked against glibc (`libc`, `libm`, `libdl`); the
    guest's loader and libraries come from the toolchain's sysroot like any
    dynamic guest binary (`test/qemu/scripts/mkinitramfs.sh`, `copy_deps`).
## Licences

Found on 2026-10-16 from the repositories and the archives themselves. No
other statement was looked for or found (the project's site, `sibylfs.io`,
and its documentation repository say where to download binaries and nothing
about licences).

| Artifact | Licence | Where it is stated |
|---|---|---|
| The checker's own code (`fs_test_check`, `fs_test_posix`: built from `sibylfs_src`) | ISC | `LICENSE.txt` of `github.com/sibylfs/sibylfs_src`: "Copyright (c) 2013, 2014, 2015, Tom Ridge, David Sheets, Thomas Tuerk, Andrea Giugliano (as part of the SibylFS project). Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies." |
| The binaries as distributed (`sibylfs_binaries`, release 2015-10-05) | **nothing stated** | The repository holds a `README.md` and no licence file (GitHub's licence API answers 404); the README says "this repo contains no source code - it is only here to allow binaries to be uploaded to github". The archive holds `bin/` only: no licence or notice file. |
| OCaml runtime and standard library, `num`, linked statically into the binaries | LGPL 2 (`num`: 2.1) with a special exception that allows static linking and distributing the executable "under terms of your choice" | `LICENSE` of `github.com/ocaml/ocaml` at 4.02.3 (the version `sibylfs_src`'s source build used, `docs/plan/notes/sibylfs-checker-12.18.md`) and `LICENSE` of `github.com/ocaml/num` |
| Lem library (`Lem_*`, `Pmap`, `Pset`) | BSD 3-clause (no endorsement clause); `pmap` and `pset`, modified OCaml `Map` and `Set`: LGPL 2 | `LICENSE` of `github.com/rems-project/lem` (master, 2026; the fork of the binaries' build, `sibylfs-lem`, was not found) |
| Sexplib | Apache 2.0 | `LICENSE.txt` of `github.com/janestreet/sexplib` at tag 113.00.00 (`sibylfs_src`'s `opam` requires `< 113.01.00`) |
| ctypes | MIT | `LICENSE` of `github.com/ocamllabs/ocaml-ctypes` (master) |
| `sha`, `unix-fcntl`, `unix-errno` | ISC | `LICENSE.md` of `github.com/djs55/ocaml-sha`; `LICENSE` of `github.com/dsheets/ocaml-unix-fcntl`; `license: "ISC"` in the `opam` file of `github.com/dsheets/ocaml-unix-errno` (no `LICENSE` file) |
| `fd-send-recv` | LGPL 2.1 with the same linking exception as OCaml's | `LICENSE` of `github.com/xapi-project/ocaml-fd-send-recv` |
| glibc (the loader and libraries the guest runs the binaries with) | LGPL 2.1 or later | The toolchain sysroot's Debian packages (`third_party/llvm/README.md`); already in the test SBOM |

Which libraries are linked was read from the symbols of `fs_test_check` and
`fs_test_posix` (`nm`), not from a build record; the versions of the libraries
inside the binaries are unknown, and the licences above are those of the
repositories at the version named or at their head. `cppo` and `lem` itself
(build tools) and `menhir` and `cow` (in `sibylfs_src`'s `opam` file) are
not in the binaries' symbols.

## What we do with it

- It is fetched by Bazel at build time, from the URLs above, with the
  sha256 checked; it is not checked in.
- It is used by `testonly` targets only: the guest initramfs of the QEMU
  tests (`//test/qemu:initramfs`, `testonly = 1`) carries the two binaries
  and their glibc (`/sibylfs`), and the guest runs them. Nothing from it is
  linked into dcfs, installed by it, or part of a release artifact or
  package.
- Open question for russ (2026-10-16): the binaries as a distribution have no
  stated licence (the test suite, not pinned yet, has none either), and a
  Bazel disk cache or CI cache that stores the fetched tarball (and the
  initramfs built from it) is storage we run, not a redistribution to anyone,
  but nothing here decides that. The code of the checker is ISC, which permits what we do if the
  notice is kept; the prebuilt tarball carries none.

## Updating the pin

1. Pick the new release or commit. Download the asset, hash it (`sha256sum`)
   and set `url` and `sha256` in `MODULE.bazel`.
2. Look the licences up again (the table above): a licence file may have
   appeared, and the checker's libraries change with a new build.
3. Run `bazel test //test/qemu:sibylfs_selftest_test`. The guest's account
   commands (`guest/sibylfs_lib.sh`) may need to follow.
4. Update `tools/sbom/pins.json` if the repository names changed.
