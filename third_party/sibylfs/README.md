# SibylFS (pinned)

Why: SibylFS is an executable specification of POSIX file systems (Ridge,
Sheets, Tuerk, Giugliano, Madhavapeddy and Sewell, SOSP 2015). The guest
test `test/qemu/guest/sibylfs.sh` (plan step 12.18b) runs its test suite on a
dcfs mount and on the backing file system under it, asks the SibylFS model
whether each trace is allowed, and fails only where the model rejects dcfs
and accepts the backing file system. It is a second oracle beside
`pjdfstest`: it checks sequences of system calls against a model, where
pjdfstest checks single calls against expected results. Plan step 12.16
replaces it with a checker built from `formal/Ideal.tla`, after which these
pins are deleted.

What is here:

- `BUILD.checker`: the Bazel overlay for `@sibylfs_checker`, which exports
  the two binaries the guest runs.
- `BUILD.bazel`: `testonly` filegroups over the two pins. Nothing from
  either pin is reachable from a target that is not `testonly`.

Nothing of SibylFS is checked in.

## Pin

Both are declared in `MODULE.bazel`.

- **Checker**: `@sibylfs_checker`, the prebuilt x86-64 Linux binaries
  (`fs_test_check`, `fs_test_posix`, `fs_test`, `run_trace`, `tgen`) of
  release `2015-10-05` of `github.com/sibylfs/sibylfs_binaries`.
  - URL `https://github.com/sibylfs/sibylfs_binaries/releases/download/2015-10-05/sibylfs_linux_amd64.tar.gz`
  - sha256 `080c5bced08bbb34dedde43cdbd4f8fd3e1c5d678c60dff7067cc6f189208879`,
    from downloading the asset on 2026-10-16 (GitHub publishes no digest for
    it); the investigation of 2026-10-16
    (`docs/plan/notes/sibylfs-checker-12.18.md`, correction header) found it
    behaves as the checker built from source.
  - The guest runs only `fs_test_posix` and `fs_test_check`. The binaries
    are dynamically linked against glibc (`libc`, `libm`, `libdl`); the
    guest's loader and libraries come from the toolchain's sysroot like any
    dynamic guest binary (`test/qemu/scripts/mkinitramfs.sh`, `copy_deps`).
- **Test suite**: `@sibylfs_test_suite`, `github.com/sibylfs/sibylfs_test_suite`
  at commit `14f81504ca2d497e8b945133cd6fe0159e163079`, the tip of `master`
  (last pushed 2015-08-19), as the tarball GitHub makes of that commit.
  - URL `https://github.com/sibylfs/sibylfs_test_suite/archive/14f81504ca2d497e8b945133cd6fe0159e163079.tar.gz`
  - sha256 `39205e0803f7773f9e86751b7e030cdfe24fcf23a29be098b735645b2400813d`,
    from downloading it on 2026-10-16.
  - 21,070 scripts (`*-int.trace`, `@type script`) outside the `.tmpl`
    directories, already generated: the `.tmpl` directories hold the
    hand-written ones, which `gentests.sh` copies into the generated
    directories and the tarball has copied. 52 are hand-written (`adhoc_*`);
    one of them (`-check-int.trace`) is for the checker alone, so 21,069 run:
    51 hand-written and 21,018 generated.

## Licences

Found on 2026-10-16 from the repositories and the archives themselves. No
other statement was looked for or found (the project's site, `sibylfs.io`,
and its documentation repository say where to download binaries and nothing
about licences).

| Artifact | Licence | Where it is stated |
|---|---|---|
| The checker's own code (`fs_test_check`, `fs_test_posix`: built from `sibylfs_src`) | ISC | `LICENSE.txt` of `github.com/sibylfs/sibylfs_src`: "Copyright (c) 2013, 2014, 2015, Tom Ridge, David Sheets, Thomas Tuerk, Andrea Giugliano (as part of the SibylFS project). Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies." |
| The binaries as distributed (`sibylfs_binaries`, release 2015-10-05) | **nothing stated** | The repository holds a `README.md` and no licence file (GitHub's licence API answers 404); the README says "this repo contains no source code - it is only here to allow binaries to be uploaded to github". The archive holds `bin/` only: no licence or notice file. |
| The test suite (`sibylfs_test_suite` at 14f8150) | **nothing stated** | The repository holds a `README.md` and `test-suite/`; GitHub's licence API answers 404; no file of the archive contains "copyright" or "licen[cs]e" in any case. The scripts come from the generator `tgen` of `sibylfs_src` (ISC) and from hand-written scripts of the same authors, but nothing says so. With no licence stated, all rights are reserved. |
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

- Both are fetched by Bazel at build time, from the URLs above, with the
  sha256 checked; neither is checked in.
- They are used by `testonly` targets only: the guest initramfs of the QEMU
  tests (`//test/qemu:initramfs`, `testonly = 1`) carries the two binaries,
  their glibc and the suite tarball (`/sibylfs`), and the guest runs them.
  Nothing from either is linked into dcfs, installed by it, or part of a
  release artifact or package.
- **russ ruled (2026-10-17)**: the test suite has no stated licence, and
  using it only in our tests is fine; a Bazel disk cache or CI cache that
  stores the fetched tarballs (and the initramfs built from them) is storage
  we run, not a redistribution. The checker's code is ISC, which permits what
  we do if the notice is kept; the prebuilt tarball carries none and has no
  stated licence of its own, and the ruling is taken to cover it as the same
  kind of test-only use (russ may correct that). Anything that would copy
  either pin into a release artifact, a package or another repository is not
  covered and needs a new decision.

## Updating the pin

1. Pick the new release or commit. Download the asset, hash it (`sha256sum`)
   and set `url` and `sha256` in `MODULE.bazel`; for the suite, change the
   commit in the URL.
2. Look the licences up again (the table above): a licence file may have
   appeared, and the checker's libraries change with a new build.
3. Run `sibylfs_lib_test`, `sibylfs_selftest_test` and `sibylfs_test` of
   `//test/qemu` (and `sibylfs_full_test`, which is manual). The guest's
   account commands, the scripts it runs and the
   divergences it records
   (`test/qemu/guest/sibylfs.<fstype>.expected_divergences`) may need to
   follow.
4. Update `tools/sbom/pins.json` if the repository names changed.
