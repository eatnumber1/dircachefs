<!-- SPDX-License-Identifier: MIT -->
# Licences (step 7.8, work in progress)

dcfs is MIT (`LICENSE`). This file records what the licence audit has read so
far, from the pinned archives in the Bazel output base
(`$(bazel info output_base)/external/<repo>/`) and not from memory. It is
unfinished: see "Not done yet" at the end.

## What is linked into `//dcfs:main_static`

`//dcfs:linked_deps` lists the external repositories of `//dcfs:main` and
`//dcfs:main_static`; the link action of `main_static` names the objects
below (read with `bazel aquery 'mnemonic("CppLink", //dcfs:main_static)'`).

| Component | Pinned version | Licence as read | Read from | Linked |
|---|---|---|---|---|
| abseil-cpp | 20260817.0 | Apache-2.0 | `external/abseil-cpp+/LICENSE` (no NOTICE file) | its objects, 650 labels |
| gloop (abseil's) | 20260708.rc1 | Apache-2.0 | `external/gloop+/LICENSE` (no NOTICE file) | `gloop/base` |
| SQLite | 3.53.4 | public domain ("The author disclaims copyright", blessing) | header of `external/sqlite3+/sqlite3.h` and `sqlite3.c`; the BCR archive has no LICENSE file | `sqlite3.c` |
| libfuse | 3.18.2 | `lib/` and `include/`: LGPL-2.1 (`LICENSE` says so; every linked `lib/*.c` and `lib/modules/*.c` header says "GNU LGPLv2"); `include/fuse_kernel.h`: `(GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause`; everything else (`util/`, `example/`, `test/`) GPL-2.0 | `external/libfuse+/LICENSE`, `LGPL2.txt`, `GPL2.txt`, file headers | only `lib/` (buffer, compat, cuse_lowlevel, fuse, fuse_log, fuse_loop, fuse_loop_mt, fuse_lowlevel, fuse_opt, fuse_signals, fuse_uring, helper, modules/iconv, mount, mount_util, modules/subdir, util). Nothing from `util/` (fusermount3, mount.fuse3): the GPL parts are not linked |
| liburing | 2.14 | LGPL-2.1 or MIT (dual); the headers carry `SPDX-License-Identifier: MIT` (23 files) and one `io_uring.h` is `(GPL-2.0 WITH Linux-syscall-note) OR MIT` | `external/liburing+/LICENSE` (MIT), `COPYING` (LGPL-2.1), `README` | `src/queue.c register.c setup.c syscall.c version.c`; dcfs elects MIT |
| numactl (libnuma) | 2.0.19.bcr.1 | libnuma LGPL-2.1; the tools GPL-2.0 (not linked) | `external/numactl+/README.md`, `LICENSE.LGPL2.1`, headers of `libnuma.c`, `affinity.c`, `distance.c`, `syscall.c`, `numa.h` | `affinity distance libnuma rtnetlink syscall sysfs` |
| glibc | 2.41-12+deb13u4 (Debian 13 sysroot, `libc6-dev`) | mostly LGPL-2.1+ (`Files: *`), plus LGPL-2+, LGPL-3+ (one htl file), BSD and others per file; static linking brings in LGPL section 6 | `external/+llvm_distribution+dcfs_llvm/sysroot/usr/share/doc/libc6-dev/copyright` | libc.a, libm, crt files |
| LLVM libc++, libc++abi, libunwind, compiler-rt builtins | 22.1.8 | Apache-2.0 WITH LLVM-exception (headers say so; the release tarball carries no LICENSE.TXT, so the text has to be pinned separately: see "Not done yet") | `libc++.a`, `libc++abi.a`, `libunwind.a`, `libclang_rt.builtins.a` appear among the link inputs | yes |

Role of the rest: `bazel_tools`, `platforms`, `rules_cc` are build
configuration (`build_only` in `tools/sbom/pins.json`).

## Test-only and build-only pins read so far

| Component | Licence as read | Read from |
|---|---|---|
| googletest 1.18.0.bcr.1 | BSD-3-Clause | `external/googletest+/LICENSE` |
| google_benchmark 1.9.5 | Apache-2.0 | `external/google_benchmark+/LICENSE` |
| rules_cc, rules_shell, bazel_skylib, rules_distroless, rules_java, rules_python, platforms, toolchains_llvm | Apache-2.0 | each `external/<module>+/LICENSE` |
| pjdfstest (commit 85a8aea) | BSD-2-Clause (two clauses) | `external/+http_archive+pjdfstest/COPYING` |
| xfstests v2026.05.17 | GPL-2.0 (files carry `GPL-2.0` or `GPL-2.0+`) | `external/+http_archive+xfstests/LICENSES/GPL-2.0`, file headers of `check`, `common/rc`, `ltp/fsstress.c` |
| tla2tools 1.7.4 | MIT (its own `License.txt`); the jar bundles EPL-2.0 parts (`META-INF/LICENSE.txt`) | inside the jar |
| TLA+ CommunityModules 202610040242 | MIT | `LICENSE` inside the jar |
| act 0.2.89 | MIT | `external/+http_archive+act/LICENSE` |
| pandoc 3.12 | GPL-2.0-or-later ("Released under the GPL, version 2 or greater") | `pandoc.1` of the archive; the archive has no COPYRIGHT file |
| Debian packages (NFS rootfs) | per source package; 122 of 149 have a machine-readable copyright file | the `.deb`'s `usr/share/doc/<pkg>/copyright` |
| Alpine packages (kernel, QEMU, mkfs tools, busybox, strace) | the `L:` field of Alpine's APKINDEX (`external/+alpine_index+alpine_index/index.json`) | e.g. busybox `external/+http_archive+busybox/LICENSE` is GPL-2.0 only |

## Copied code found so far

- `tools/fhtest.c`: "Copied from fuse-generation-qemu/guest/fhtest.c". That
  repository (`~/Sources/fuse-generation-qemu`) has one commit, by Russell
  Harmon, and no licence file. Copyright is russ's own, so it can be MIT
  here.
- `test/qemu/guest/init`, `test/qemu/scripts/mkinitramfs.sh`,
  `test/qemu/scripts/run-qemu.sh`: "Adapted from fuse-generation-qemu"; same
  repository, same author.
- `dcfs/syscalls_backing.h`, `struct linux_dirent64`: "Definition from the
  getdents(2) manual page"; its five field comments are the manual page's
  words. The layout is the kernel's `include/linux/dirent.h` (not exported
  in the sysroot's UAPI headers, which was checked); the comments should be
  reworded and the kernel header named.
- `tools/testutil.c` (BTRFS_IOC_SUBVOL_CREATE, FS_IOC_SHUTDOWN, the ext4
  tune ioctl) and `tools/kernel_bugs/ext4_casefold_tune/casefold_helper.c`
  (`struct ext4_tune_sb_params`): UAPI declarations copied from the
  kernel's `<linux/btrfs.h>`, `<linux/fs.h>`, `<linux/ext4.h>`, which are
  `GPL-2.0 WITH Linux-syscall-note`; the note exists for exactly this use.
- `third_party/tlaplus/overrides/tlc2/overrides/TLCOverrides.java`: a
  reduced version of the class of the same name in CommunityModules (MIT).
- `third_party/xfstests/shim/xfs/*.h`, `glibc_strerror.c`: written here as
  stand-ins; the shim follows kernel and xfsprogs layouts "as far as the
  compiler needs".
- Patches: `third_party/xfstests/0001-dcfs-guest.patch` modifies `check`,
  `common/rc` (GPL-2.0 and GPL-2.0+; test-only); `third_party/pjdfstest/
  0001-linux-portability.patch` modifies `tests/conf`, `tests/misc.sh`
  (BSD-2-Clause; test-only); `third_party/liburing/0001-...patch` and
  `third_party/numactl/0001-...patch` modify the BCR overlay `BUILD.bazel`
  of two linked libraries (two lines each; build files, not library code).

## Not done yet (resume here)

1. Obligations section (notice and licence text for Apache/BSD/MIT; LGPL-2.1
   section 6 for libfuse's `lib/` and glibc; the relink recipe, which has to
   be measured: `bazel build --override_module=libfuse=<modified copy of
   external/libfuse+> //dcfs:main_static`; glibc needs a rebuilt `libc6-dev`
   in `sysroot_debs`).
2. `//:third_party_notices` and its test. Plan: a repository rule that
   copies the licence files with `rctx.path(Label(...))` (they are not
   exported by the modules' BUILD files, so `$(location)` cannot name them),
   plus `http_file`s for LLVM's `LICENSE.TXT`, `libcxx`, `libcxxabi`,
   `libunwind`, `compiler-rt` at commit ca7933e47d3a, and a checked-in
   `THIRD_PARTY_NOTICES`. `sbom_test` requires every `http_file` to have a
   `pins.json` entry.
3. `licenses` in the CycloneDX SBOM (pins.json gets a `license` per entry;
   Alpine from the index's `L:` field via `apk.py`'s `resolved.json`;
   Debian from a table of the copyright files).
4. SPDX headers and the gate in `tools/repo_shape.py` with
   `tools/licenses_allowed.txt` and `tools/third_party_files.txt`, as russ
   specified (an allowlist; a non-MIT identifier needs a provenance row;
   GPL identifiers only for test-only patch files).
5. README "License" section (it says "To be decided" today although
   `LICENSE` is MIT).
