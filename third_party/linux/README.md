# The pinned, stock upstream test kernel (step 3.1a)

A second kernel the QEMU tests can select (`--//test/qemu:kernel=stock`,
see `test/qemu/README.md`), next to the patched kernel that stays the
default for now (`test/qemu/scripts/build-kernel.sh`,
`docs/plan/phases/03-drop-kernel-patch-and-build-test-kernel.md`). Unlike
that kernel, this one is fetched and built entirely by Bazel: no
`~/Sources/linux` checkout, no `~/.cache/dcfs/kernel-build`, no out-of-tree
script.

## Pin

- Version: **7.2.9** (latest `stable` release on kernel.org as of
  2026-10-05; `>= 6.9` for FUSE passthrough and `FS_IOC_GETFSUUID`, `>= 6.8`
  for `STATX_MNT_ID_UNIQUE` -- both comfortably satisfied).
- URL: `https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.9.tar.xz`
- sha256: `b4c5dfbe51a364a6c7f03869200f88c8e1f77403539005f14b7fc6bc91b8d8ba`
  (downloaded directly and hashed; matches the sha256 kernel.org publishes
  at `https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc`).

Declared in `MODULE.bazel` as the `linux_source` `http_archive`.

## Update procedure

1. Check `https://www.kernel.org/releases.json` (or the front page) for the
   current `stable` release.
2. Download the `.tar.xz` from `cdn.kernel.org` and compute its sha256
   (`sha256sum`); cross-check against `sha256sums.asc` in the same
   directory.
3. Update the `url`, `sha256` and `strip_prefix` in `MODULE.bazel`'s
   `linux_source` `http_archive`, and the version in this file.
4. `bazel build //third_party/linux:kernel_build`. If it fails with
   `MISSING: ... fragment symbol(s) did not survive olddefconfig`, a
   Kconfig symbol in `kernel.config` was renamed, removed, or gained a new
   dependency in the new version -- see "Kconfig dependency notes" below for
   how to track one down, and fix `kernel.config`.
5. `bazel test //test/qemu:boot_test --//test/qemu:kernel=stock` and then
   the full suite on the default (patched) kernel.

## Config: `tinyconfig` plus `kernel.config`

`build_kernel.sh` runs `make tinyconfig` (effectively `allnoconfig` plus a
few size-optimization defaults -- every optional symbol off), merges
`kernel.config` on top (`scripts/kconfig/merge_config.sh -m`, merge only),
runs `make olddefconfig` to resolve dependencies, and then **fails the
build** if `olddefconfig` dropped anything `kernel.config` asked for (a
line-for-line comparison -- see the script). This is deliberately much
stricter than `test/qemu/scripts/build-kernel.sh`'s `x86_64_defconfig` +
`kvm_guest.config` + a long `scripts/config -e/-d` list: that baseline
already turns on a large amount of scaffolding (`BLOCK`, `NETDEVICES`,
`HYPERVISOR_GUEST`, ...) that `tinyconfig` does not, so `kernel.config` has
to ask for all of it explicitly. See `docs/plan/execution.md`'s "Kernel
config once": later phases needing a new option add a line here instead of
editing concurrently.

### Kconfig dependency notes

Getting `kernel.config` right took real trial and error, worth recording so
the next pin update isn't a repeat:

- **`menuconfig`/`bool "..." if EXPERT`-style symbols with a `default y`
  are still off under `tinyconfig`.** `allnoconfig` (what `tinyconfig` is
  built on) explicitly ignores ordinary Kconfig defaults and sets
  everything to `n` unless something forces it on. `x86_64_defconfig`
  doesn't have this problem (it honors defaults), which is why
  `build-kernel.sh`'s list never needed to mention `BLOCK`, `TTY`,
  `HYPERVISOR_GUEST`, `MULTIUSER`, `NETWORK_FILESYSTEMS`, `NETDEVICES` or
  `MD` -- `kernel.config` has to turn every one of these "wrapping" symbols
  on explicitly, or everything nested under them (`EXT4_FS`,
  `VIRTIO_BLK`/`VIRTIO_NET`, `PARAVIRT`/`KVM_GUEST`/`PVH`, `NFS_FS`/`NFSD`,
  `BLK_DEV_DM`/`DM_*`) silently disappears -- not a build failure, just an
  absent option, which is exactly what the build script's fragment-vs-final
  comparison is for.
- **Prompt-less symbols (`tristate`/`bool` with no quoted string) can only
  be turned on by another symbol's `select`, never by a fragment.** Kconfig
  recalculates them from scratch every time regardless of what's sitting in
  the merged `.config`. `ZLIB_DEFLATE`, `LZO_COMPRESS`, `ZSTD_COMPRESS` (and
  their `_DECOMPRESS`/`_INFLATE` counterparts) and `SUNRPC` are all like
  this: `CONFIG_BTRFS_FS` already `select`s the compression ones, and
  `CONFIG_NFS_FS`/`CONFIG_NFSD` already `select` `SUNRPC`, so listing them
  directly in `kernel.config` does nothing but trip the "dropped by
  olddefconfig" check. Trust the `select`ing symbol instead.
- **A symbol can simply stop existing.** `CONFIG_LIBCRC32C` (present in
  `build-kernel.sh`'s list) was folded into the crypto subsystem some
  releases back; `CONFIG_CRYPTO_CRC32C` is the whole story in 7.2.9.
- To track one of these down for a future pin: extract the new tarball,
  `grep -rn "^config SYMBOL_NAME$" .` to find its definition, read upward
  for an enclosing `if FOO`/`menuconfig FOO` block, and check whether FOO
  itself needs the same treatment.

## Build

`BUILD.bazel`'s `:kernel_build` genrule runs `build_kernel.sh` as an
ordinary Bazel action:

- Out-of-tree (`make O=...`), so the read-only `@linux_source` checkout
  Bazel hands the action is never written to -- only the action's own
  scratch directories (`mktemp -d`, cleaned up by Bazel, not a declared
  output) are.
- No network access.
- Declared outputs: `bzImage` and `kernel-build.log` (enabled-symbol counts
  and the final `bzImage` size, same information
  `test/qemu/scripts/build-kernel.sh` used to log).

### Hermetic build tools

Per `AGENTS.md`'s third-party convention, the Bazel Central Registry is
preferred over host tools wherever practical:

- **GNU bc**: not in the BCR. Fetched and built from source by Bazel --
  see `third_party/bc/README.md`. Used on `PATH` ahead of any host `bc` for
  `kernel/time/timeconst.bc`.
- **flex and bison**: *tried and reverted.* Both are in the BCR
  (`flex@2.6.4.bcr.3`, `bison@3.8.2`) and both build and run standalone
  fine (`bazel build @flex//:flex @bison//:bin/bison`; `--version` works).
  But `scripts/kconfig/conf` needs them to actually *generate*
  `lexer.lex.c`/`parser.tab.c` from `lexer.l`/`parser.y` (no pre-generated
  `.c` ships in the kernel's release tarball), and invoking either BCR
  binary for that -- whether from its own `bazel-out` location or copied/
  symlinked elsewhere, with or without `RUNFILES_DIR`/
  `RUNFILES_MANIFEST_FILE` set -- fails: flex forks an internal `m4`-based
  pipeline (`@m4+//:m4`, declared in `flex.runfiles`) that dies with
  `SIGPIPE`, and bison can't find its own `bison/data/m4sugar/m4sugar.m4`
  runtime data. Both are Bazel-runfiles-dependent tools, and the specific
  failure mode didn't resolve with the two straightforward fixes tried, so
  per the step's own guidance this was reverted rather than pursued
  further: `build_kernel.sh` uses whatever `flex`/`bison` the *host* has on
  `PATH`, same as `third_party/bc`'s build. A future pass could revisit
  this by wrapping the BCR binaries in a small launcher that sets up their
  runfiles environment correctly before exec-ing them (e.g. mimicking what
  `bazel run` itself does), but that's nontrivial enough to be its own unit
  of work.
- **The C toolchain (gcc, binutils) and libelf/zlib headers**: left to the
  *host*, as the step text explicitly allows until Phase 7 pins an LLVM
  toolchain (`make LLVM=1` then). `kernel.config`'s minimal feature set
  (`tinyconfig`-based, no `DEBUG_INFO_BTF`, no `STACK_VALIDATION`/objtool)
  does not actually exercise libelf or zlib during this build -- this
  kernel's object files are produced by plain `gcc`/`ld`/`as`/`ar`, no
  `objtool`/`pahole` step runs -- so this build has no real runtime
  dependency on the BCR's `elfutils`/`zlib` modules either way; they were
  not pursued for that reason (nothing to wire them into).

### Remaining host tools

Not made hermetic, listed here per the step's instructions so nothing is
silently depended on:

- `flex`, `bison` (kconfig lexer/parser generation -- see above).
- `gcc`/`cc`, `ld`, `as`, `ar`, `nm`, `objcopy` (the host C
  toolchain/binutils -- Phase 7 pins an LLVM toolchain and switches this to
  `make LLVM=1`).
- `perl`, `python3`, `awk`, `sed`, `bash`/`sh`, `make`, coreutils
  (ordinary kbuild scripting dependencies; these are effectively universal
  on any Linux build host and were not considered worth pinning
  separately).
- `libelf`/`zlib` *headers* are on the host's default include path in case
  some future config change needs them (e.g. enabling `STACK_VALIDATION`),
  but this build's current config does not actually use them.

## Selecting the kernel

`test/qemu:kernel` is a `string_flag` (`stock` default, or `patched`; see
`test/qemu/BUILD.bazel` and `test/qemu/kernel.bzl`). `stock` (the default
since Phase 3b dropped the kernel patch) uses this package's `:bzImage`
for every `qemu_test`/`qemu_test_matrix`/`qemu_cc_test` target. `patched`
is deprecated: it selects `@kernel_image//:bzImage` (the out-of-tree,
not-Bazel-tracked build of `test/qemu/scripts/build-kernel.sh`'s patched
kernel), kept only until Phase 4 removes that build path entirely. Select
it with `--//test/qemu:kernel=patched`, e.g.:

```
bazel test //test/qemu:boot_test --//test/qemu:kernel=patched
```

dcfs's full suite passes against the stock kernel (the default).
