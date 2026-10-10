# Phase 7 — Pinned toolchain, coverage, warnings, UBSan, clang-tidy

**Decision (russ, 2026-10-05).** Every build uses a pinned, hermetic
toolchain instead of the host's GCC: `toolchains_llvm` from the BCR
(1.11.x) with a pinned LLVM release, registered in `MODULE.bazel`. Builds
are then the same on every machine and in CI.
- 7.1 Switch to the pinned clang for all targets (dcfs, tools, tests,
  pjdfstest/fsstress/fsx builds). Fix what clang's warnings find (test
  first where a warning is a bug). Re-check the libfuse overlay workaround
  (`--dynamic_mode=off` because the BCR libfuse `.so` did not link under
  ld.gold) with lld; keep it only if still needed. The guest initramfs
  gets whatever runtime the new toolchain's binaries need (sanitizer
  runtimes for 7.4). Done when the full suite passes, ASan included.
- 7.1 done 2026-10-07 (f218d45): toolchains_llvm 1.11.1 + LLVM 22.1.8
  (23.1.2's lld needs ICU 70); `main_static` static with lld, libc++,
  libc++abi, libunwind, compiler-rt; ASan/UBSan link their C++ runtimes
  (`-fsanitize-link-c++-runtime`; the C driver otherwise omits them: a
  guest self-check); `--dynamic_mode=off` stays (lld rejects libfuse's
  `.symver` in a shared link); liburing probes use compiler-rt; numactl's
  version script dropped (hid ASan's malloc); toolchain_test + self-check;
  llvm-project in the SBOM as toolchain runtime; CI repository cache keyed
  by the lock file. Remaining host dependencies (documented in
  `third_party/llvm/README.md`): the LLVM binaries' libc/libstdc++/zlib/
  libxml2/ICU/libgcc_s, glibc headers + static libs and the Linux UAPI
  headers from the host (no sysroot), bash/mktemp/realpath/rm.
- 7.1b (follow-up): close those with a Debian sysroot, or Alpine's
  clang/lld packages through the musl loader (the Phase 24 pattern).
- 7.2 Coverage: clang source-based coverage
  (`-fprofile-instr-generate -fcoverage-mapping`). The guest writes its
  `.profraw` files to a scratch virtio disk; `run-qemu.sh` copies them out,
  merges them (`llvm-profdata`) and writes lcov (`llvm-cov export
  -format=lcov`) to Bazel's coverage output, so `bazel coverage
  --combined_report=lcov //...` covers unit and e2e tests alike. CI
  publishes the report (no threshold gate at first); the first report is
  reviewed with russ to pick untested paths worth tests.
- 7.3 Warnings (russ: -Wall, -Wextra and every other warning we can
  get): our code (dcfs, tools, tests) builds with clang's `-Weverything
  -Werror` minus an explicit deny-list in `.bazelrc`, each exclusion with a
  one-line reason (e.g. `-Wno-c++98-compat`, `-Wno-padded`, warnings that
  conflict with Abseil idioms). Today's flags are only `-Werror
  -Wimplicit-fallthrough` plus `-Wno-sign-compare`, which goes away.
  `-Werror` stays on for our code: every warning is an error.
  External repositories are not built with these warnings at all (not just
  `-Wno-error` as today). Real bugs found get a test first; the rest are
  fixed in batches by directory.
- 7.4 UBSan (`--config=ubsan`, today blocked by GCC + Abseil constexpr):
  enabled with clang, `-fno-sanitize-recover=all` so any report fails the
  test, run over the whole QEMU suite like ASan. Each finding gets a test
  first, then a fix.
- 7.5 clang-tidy (russ, 2026-10-05): run the pinned LLVM's clang-tidy
  over our code through a Bazel aspect (the `bazel_clang_tidy` aspect, or
  a small one of our own if it does not fit the pinned toolchain), with a
  checked-in `.clang-tidy`: `bugprone-*`, `cert-*`, `clang-analyzer-*`,
  `concurrency-*`, `misc-*`, `modernize-*`, `performance-*`,
  `portability-*`, `readability-*`, minus a commented deny-list, and
  `WarningsAsErrors: '*'`. Our code only, not `third_party/` or external
  repositories. CI runs it on every push; findings that are real bugs get
  a test first. It runs on the host (no root or kernel needed).
  Landed early by 25.21 (2026-10-10, 66e0848): the aspect, the check set
  (minus clang-analyzer-*, too slow: a large-tier target later), the
  deny-list and the shrinking allowlist. Left for 7.5: the aspect failing
  the build itself rather than through the verdict test, and the analyzer
  target.
  `readability-*` is already in the set; style 1.6a (russ, 2026-10-10)
  names three that must not land on the deny-list and one option:
  `readability-else-after-return`, `readability-misleading-indentation`,
  `readability-function-cognitive-complexity` with `Threshold: 15` to
  start (tightened as the tree allows, never loosened without russ).
- 7.5b Our own style rules as AST matchers (russ's idea, 2026-10-07): the
  mechanical rules of `docs/style.md` that a regex cannot enforce (no
  `absl::XError(StrCat(...))`, the `syscalls::` call form and no raw libc
  syscall outside `syscalls.cc`, `ErrnoToStatus` for every syscall
  failure, `enum class` only, ADL hooks as hidden friends, no nested
  namespaces but `syscalls`/`sqlite3`) as a checked-in matcher file run
  by `clang-query` (from the pinned LLVM) over every translation unit,
  as a Bazel test; a clang-tidy plugin with the same matchers if we
  outgrow it. Replaces 25.1b's regex `raw_syscalls_test`.
- 7.5c Spike (russ, 2026-10-07): a tiny hermetic model as a judge for the
  rules matchers cannot express, one rule per prompt over one small chunk
  (a comment, an error message, a commit message): weights by `http_file`
  + sha256, llama.cpp built by Bazel, integer quantization, one thread,
  greedy decoding (deterministic on one binary). Measure first: two or
  three judgement rules ("the comment says why", "the message names the
  call and what failed", "the commit quotes the failing-first run"), a
  labelled set of ~100 chunks per rule from our tree (labelled by the
  review agents, spot-checked by russ), precision/recall per rule. A rule
  at >= 98% precision may gate; below that advisory or dropped. After
  7.5b, so the matchers show what is left for a model.
- 7.6a C++ reformat to Google's rules, now (russ, 2026-10-11: "When we do
  a codebase-wide reformat, I want Google's format rules to apply. Also
  when are we doing that?"; dispatched the same morning, mechanical,
  lane-2, while no lane holds an unmerged code branch): `.clang-format`
  becomes `BasedOnStyle: Google` alone (the `PointerAlignment: Right` and
  `DerivePointerAlignment: false` overrides go); `bazel run //tools:format`
  runs the pinned clang-format over the tracked C/C++ files;
  `//tools:format_test` (`small`, in `--config=fast`) runs it in
  `--dry-run -Werror` mode; one reformat commit over the whole tree, its
  hash in `.git-blame-ignore-revs`; the style-check allowlist's keys carry
  no line numbers, so they survive, but the counts of line-sensitive
  checks may move and the test tells. Also 7.6a: `tools/*.c` tabs and the
  long lines the old 7.6 text lists, as far as clang-format settles them.
- 7.6b (after the reset): buildifier, shfmt (`-i 2 -ci -bn`) and
  shellcheck pinned and added to `//tools:format` and `format_test`, the
  shell and BUILD reformat in one commit each, the pre-commit hook.
- 7.6 Formatting enforced (russ, 2026-10-07; `docs/style.md`): the pinned
  LLVM's clang-format and a pinned buildifier (BCR or `http_file`) behind
  `bazel run //tools:format` (replaces `tools/format.sh`'s "if installed"),
  and a `small` `//tools:format_test` that runs both in check mode
  (`clang-format --dry-run -Werror`, `buildifier -mode=check`) over the
  tracked C/C++ and Bazel files, plus a pinned `shfmt` (Google shell
  style: 2-space indent, `-i 2 -ci -bn`) and `shellcheck` over every
  shell script (russ, 2026-10-07: Google's style guides for every
  language), so `--config=fast` and CI fail on an unformatted file. An opt-in `.githooks/pre-commit` (`git config
  core.hooksPath .githooks`) runs the same check. The same change
  reformats the tree once (`tools/*.c` tabs, the ~100 long lines, the
  6-space BUILD lists, 48 tab-indented shell scripts, host scripts to bash), so the test is green from its first commit. A
  pinned Python formatter (Google Python style, 80 columns) for `tools/`
  and `man/` joins it if rules_python offers one without a pip
  dependency; otherwise it is a separate small step.
- 7.7 Include-what-you-use (russ, 2026-10-07): `layering_check` (a target
  includes only headers of its direct deps; needs the clang toolchain) on
  our targets, and clang-tidy's `misc-include-cleaner` in 7.5's
  configuration. Findings fixed in batches, mechanical.
Owner: Sonnet (7.1, 7.2), Opus review; findings from 7.4 by owner of
the affected area. Order: right after the CI phase, before the benchmarks
(so every later phase runs under UBSan and has coverage); within it,
7.1 then 7.2 (coverage) first, then 7.3, 7.4, 7.5, 7.6 and 7.7.

## 7.1c Sysroot to Debian 13 (russ, 2026-10-08)

The shipped glibc 2.36-9+deb12u14 (bookworm) carries 16 unfixed Debian
OSV records; russ chose to bump the sysroot to Debian 13 (trixie, glibc
2.41). Update `third_party/llvm`'s Debian package pins (libc6, libc6-dev,
linux-libc-dev already trixie, the runtime libs) to a trixie snapshot
with new sha256s, `tools/sbom/debian_sources.tsv` and the SBOM pins, rerun
the hermeticity gate and the reproducible-build gate, and make the osv
step's shipped-deb scan gate on fixable records: an `osv-scanner.toml`
ignore entry with reason and expiry for each record no Debian release
fixes, so the step stops being `continue-on-error`. Owner: Sonnet.
Status 2026-10-09: merged dea789b. As built: libc6 and libc6-dev to
2.41-12+deb13u4 from snapshot 20261006T082722Z (the runtime libs stay
bookworm: they only run clang); trixie is merged-usr, so `extract.py
usrmerge` links lib and lib64 into usr/ (libc.so's linker script names
/lib/x86_64-linux-gnu/libc.so.6; without the links liburing's configure
probe failed); glibc 2.41's libc.a links in popen,
posix_spawn_file_actions_adddup2, __fprintf_chk and __sprintf_chk, now
`<runtime>` allows in tools/banned_symbols.txt; the OSV shipped-debs scan
gates, with 21 ignores (none fixed in trixie: 10 only in forky, 11
nowhere) expiring 2027-01-06, when they come back for review. The first
Bazel command after rebasing onto this re-extracts @dcfs_llvm (about 25
minutes on the loaded host): see process.md.

## 7.1d Host-header shims removed (russ, 2026-10-09)

russ: "dcfs/fsuuid_compat.h probably isn't necessary anymore if we're
using hermetic kernel headers." Confirmed: the sysroot's UAPI headers are
trixie's linux-libc-dev 6.12 (7.1b), whose `linux/fs.h` defines `struct
fsuuid2` and `FS_IOC_GETFSUUID` (lines 71 and 233), so the header's
`#ifndef` never fires. Remove `dcfs/fsuuid_compat.h` (users: dcfs/BUILD.bazel,
device_id.cc, device_id_fault_test.cc include `<linux/fs.h>` directly) and
the `#ifndef FS_IOC_SHUTDOWN` shim in tools/testutil.c (same header). A
grep for other `#ifndef <UAPI constant>` shims found none.
Correction (2026-10-09, from the step): the sysroot's `linux/fs.h` does
NOT define `FS_IOC_SHUTDOWN` or `FS_SHUTDOWN_*` (the orchestrator's claim
was wrong), so that shim stays, recorded in style.md as the one remaining
UAPI copy, to go when the pin is raised. Removed instead: `fsuuid_compat.h`
and an `FS_CASEFOLD_FL` shim in tools/kernel_bugs (defined at fs.h:290). Rule for
style.md's includes section: no copies of UAPI definitions; the pinned
sysroot's headers are the only source, and a missing definition means the
pin is too old. Owner: dcfs-mechanical, any free lane after 26.16's P0.
Also in 7.1d (russ, 2026-10-09: "Keep it. Mechanical invariant enforcement
is more valuable."): style.md 1.5/1.8 state in one sentence why the
syscall wrappers are three libraries: `syscalls_backing.h` is its own
target so `//tools:syscalls_backing_users_test`'s golden list enforces
"every backing-reaching syscall is made in backing.cc" in the build graph,
which is what keeps the fault sweep and the trace recorder complete, since
both hook backing.cc; `syscalls.h` is the process-local set anyone may
call; `syscalls_process.h` exists so bench/ and the tools get wrappers
without dcfs's libraries.

