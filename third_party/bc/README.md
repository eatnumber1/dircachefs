# GNU bc (pinned)

Why: the kernel build (`//third_party/linux:bzImage`) needs a `bc` on PATH
for `kernel/time/timeconst.bc` (and a couple of other `scripts/` uses). GNU
bc is not in the Bazel Central Registry, so it's fetched by `http_archive`
and built here (`BUILD.bazel`'s `:bc_build` genrule, via `build_bc.sh`) --
see `AGENTS.md`'s third-party-code convention.

## Pin

- Version: 1.08.2 (latest release on ftp.gnu.org/gnu/bc as of 2026-10-05).
- URL: `https://ftp.gnu.org/gnu/bc/bc-1.08.2.tar.gz`, with
  `https://mirrors.kernel.org/gnu/bc/bc-1.08.2.tar.gz` as a second URL for
  the same sha256 (ftp.gnu.org was unreachable during step 5.2's CI run).
- sha256: `ae470fec429775653e042015edc928d07c8c3b2fc59765172a330d3d87785f86`
  (matches the detached `.sig` published alongside the tarball; verified by
  downloading the tarball directly and hashing it).

Declared in `MODULE.bazel` as the `gnu_bc` `http_archive`.

## Update procedure

1. Check `https://ftp.gnu.org/gnu/bc/` for a newer `bc-*.tar.gz`.
2. Download it and compute its sha256 (`sha256sum`).
3. Update the `url`, `sha256` and `strip_prefix` in `MODULE.bazel`'s `gnu_bc`
   `http_archive`, and the version in this file.
4. `bazel build //third_party/bc:bc` and confirm it still produces a working
   `bc` (`echo '2+2' | bazel-bin/third_party/bc/bc`).
5. Rebuild the kernel (`bazel build //third_party/linux:kernel_build`) and
   run the full suite (`bazel test //...`).

## Build

GNU bc's release tarball ships a pre-generated `configure` (autoconf) and
`Makefile.in`, so `build_bc.sh` does a plain out-of-tree autoconf build:
`configure` is invoked from a separate scratch directory (`mktemp -d`) and
supports building there via VPATH, so the read-only `@gnu_bc` source Bazel
hands the action is never written to -- only that scratch directory, which
Bazel treats as the action's private, untracked work area, is. No network
access; the only declared output is the `bc` binary.

`bc/bc.c` and `bc/scan.c` (generated from `bc/bc.y`/`bc/scan.l`) are also
shipped pre-generated in the release tarball, and `configure`'s "checking
that generated files are newer than configure" check confirms a plain tar
extraction keeps that invariant, so `make` never regenerates them and this
build never actually invokes flex/bison (only `configure`'s
`AC_PROG_LEX`/`AC_PROG_YACC` probe for their presence, which doesn't require
them to work correctly). This build uses whatever flex/bison the *host* has
on `PATH` for that probe -- see `//third_party/linux:README.md`'s "Hermetic
build tools" section for why the Bazel Central Registry's flex/bison were
tried for the kernel build and reverted; the same reasoning applies here,
though it's moot in practice since this build path never actually runs
either tool.

## Remaining host tools

- `gcc` (or whatever host C compiler Bazel's genrule PATH finds) and
  binutils (`ar`, `ranlib`) to compile and link `bc`. Making this hermetic
  is Phase 7's pinned LLVM toolchain; until then, the host compiler is used
  deliberately (see the step 3.1a plan text).
- `flex`/`bison` are on PATH for `configure`'s capability probes, but (see
  above) are never actually invoked to generate anything in this build.
