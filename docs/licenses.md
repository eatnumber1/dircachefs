<!-- SPDX-License-Identifier: MIT -->
# Licences: what dcfs uses and what that obliges

dcfs itself is MIT (`LICENSE`: "MIT License, Copyright (c) 2023-2026 Russell
Harmon"). This file is the inventory of everything else the repository
fetches, builds, links, runs or copies, with the licence as read in the
primary source, how we use it, and what that use triggers if dcfs is shipped
as a binary. It is plan step 4.15, which finishes what step 7.8 started; the
gate that keeps our own files' headers honest is `tools/repo_shape.py`
(`spdx_licenses`, with `tools/licenses_allowed.txt` and
`tools/third_party_files.txt`).

How it was made: licences are read from the pinned archives in the Bazel
output base (`$(bazel info output_base)/external/<repo>/`), from the
dependency graph (`bazel mod graph`), from the link actions (`bazel aquery
'mnemonic("CppLink", //dcfs:main) union mnemonic("CppLink",
//dcfs:main_static)'`), from Alpine's index (the `L:` field), from the
`copyright` files inside the pinned `.deb`s, from the kernel tree for the
UAPI headers, and from upstream files at the pinned tag or commit for the
LLVM runtimes and the CI actions. Where a licence could not be read from a
primary source it says **unverified**, with what is missing. The read date is
2026-10-16. SPDX ids are the ones the source states, or the nearest id with
the wording quoted.

## Headline

- **8 components are linked into the dcfs binaries.** By licence: Apache-2.0
  (abseil-cpp, gloop: 2), Apache-2.0 WITH LLVM-exception (the LLVM runtime
  libraries: 1), LGPL-2.1 (libfuse `lib/`, libnuma, glibc: 3), MIT (liburing,
  elected from a dual licence: 1), public domain (SQLite: 1).
- **26 more Bazel modules** are in the resolved graph (32 in all): 16
  Apache-2.0, 3 BSD-3-Clause, 1 zlib, 1 GPL-3.0-or-later (gawk, a build
  tool), and 5 **unverified** because they are resolved but never fetched
  (boringssl, jq.bzl, libpfm, stardoc, tcmalloc). 19 are build-only, 2 are
  test or benchmark only, 5 are not fetched.
- **15 other pinned downloads and tools:** 7 build tools (LLVM release,
  the toolchain's runtime `.deb`s, Python, bazel_lib's tool binaries, uv,
  pandoc, Google's style guides) and 8 test or developer tools and images (JDK for TLC, act,
  pjdfstest, xfstests, SibylFS's checker, the two TLA+ jars, the Debian cloud
  image).
- **Two package sets for the test guests:** 78 Alpine origin packages (40
  carry a GPL term, 13 are LGPL only, 24 permissive only, 1 offers GPL or a
  permissive licence) and 149 Debian `.deb`s (131 on disk, 127 with a
  copyright file: 90 distinct files, 41 GPL, 12 LGPL, 37 permissive by
  `Files: *`; 22 unread).
- **7 CI inputs:** the four `actions/*` (MIT), osv-scanner's action and
  Bazelisk (Apache-2.0), the runner images (unverified).
- **No GPL-only code is linked into dcfs** (only GPL-with-syscall-note
  headers, compile time). Three LGPL-2.1 libraries are (libfuse, libnuma, and
  glibc in `main_static`), which is what a shipped binary has to deal with.
- **Six open questions** are at the end.

## How dcfs is linked (evidence)

`.bazelrc` sets `build --dynamic_mode=off`, so every library below is linked
as objects into the executable. `bazel aquery` of the two `CppLink` actions
shows, for both `//dcfs:main` and `//dcfs:main_static`:

- Object files, not shared libraries: about 150 abseil `.pic.o`, the 17 or 18
  `libfuse+/_objs/libfuse/*.pic.o` (`lib/` and `lib/modules/iconv`, `subdir`;
  nothing from `util/`), five `liburing+` objects (`queue register setup
  syscall version`), six `numactl+` objects (`affinity distance libnuma
  rtnetlink syscall sysfs`) and `sqlite3+/_objs/sqlite3/sqlite3.pic.o`.
  `gloop+` appears only as an include directory.
- The C++ runtime statically: `-l:libc++.a -l:libc++abi.a -l:libunwind.a`
  (the toolchain's builtin libc++) and `-rtlib=compiler-rt` (the builtins
  archive).
- glibc: `main` is linked dynamically against the sysroot's glibc (`-lm
  -lpthread -ldl`, no `-static`), so at run time it uses the host's glibc and
  none is in the file. `main_static` adds `-static` (the target has
  `features = ["fully_static_link"]`) and takes glibc 2.41's `libc.a` and crt
  files from the Debian sysroot, so its file contains glibc. The README's
  install line installs `main_static`.

So a distributed `main_static` contains LGPL-2.1 code (libfuse, libnuma,
glibc) in a static link; a distributed `main` contains LGPL-2.1 code (libfuse,
libnuma) in a static link and uses the host's glibc dynamically.

## Inventory

### 1. Linked into the dcfs binaries

| Component | Version | Licence as read | Where stated | Use |
|---|---|---|---|---|
| abseil-cpp | 20260817.0 | Apache-2.0 | `external/abseil-cpp+/LICENSE` (Apache License 2.0); no NOTICE file | static objects |
| gloop (abseil's) | 20260708.rc1 | Apache-2.0 | `external/gloop+/LICENSE`; no NOTICE file | headers only; in `//dcfs:linked_deps` |
| SQLite | 3.53.4 | public domain (SPDX `blessing`) | `sqlite3.h`, `sqlite3.c`: "The author disclaims copyright to this source code. In place of a legal notice, here is a blessing"; the BCR archive has no LICENSE file | one object |
| libfuse | 3.18.2 | `lib/`, `include/`: LGPL-2.1 (`LGPL2.txt` is "Version 2.1"; headers say "GNU LGPLv2" with no "or later", so read as `LGPL-2.1-only`, which is also Alpine's `L:` for fuse3); everything else GPL-2.0-only; `include/fuse_kernel.h`: `(GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause` | `external/libfuse+/LICENSE`: "All files in the include/ directory. All files in the lib/ directory ... may be used under the terms of the GNU Lesser General Public License, version 2.1 ... All other files may be used only under the terms of the GNU General Public License, version 2"; `lib/util.c` has no header of its own and falls under that rule | `lib/` only; `util/` (fusermount3, mount.fuse3: GPL) is not linked |
| liburing | 2.14 | MIT or LGPL-2.1 (dual); the linked sources are `SPDX-License-Identifier: MIT`; one `io_uring.h` is `(GPL-2.0 WITH Linux-syscall-note) OR MIT` | `external/liburing+/README`: "All software contained within this repo is dual licensed LGPL and MIT, see COPYING and LICENSE, except for a header coming from the kernel which is dual licensed GPL with a Linux-syscall-note exception and MIT"; `LICENSE` is the MIT text, "Copyright 2020 Jens Axboe" | five objects; dcfs elects MIT |
| numactl (libnuma) | 2.0.19.bcr.1 | libnuma LGPL-2.1-only; the tools and demos GPL-2.0 | `external/numactl+/README.md`: "`numactl` and the demo programs are under the GNU General Public License, v.2. `libnuma` is under the GNU Lesser General Public License, v2.1"; `libnuma.c`: "... GNU Lesser General Public License as published by the Free Software Foundation; version 2.1." | six `libnuma` objects; no tool is linked |
| glibc | 2.41-12+deb13u4 (`libc6-dev` of the Debian 13 sysroot) | LGPL-2.1-or-later; one htl file LGPL-3+; some files under LGPL-2.1+ with a link exception; BSD and others per file | `sysroot/usr/share/doc/libc6-dev/copyright` (DEP-5): "Files: * / Copyright: 1991-2025 Free Software Foundation, Inc. / License: LGPL-2.1+"; "License: LGPL-2.1+-with-link-exception"; "Files: sysdeps/htl/raise.c ... License: LGPL-3+" | `main_static` only (static `libc.a`) |
| LLVM libc++, libc++abi, libunwind, compiler-rt builtins | 22.1.8 | Apache-2.0 WITH LLVM-exception; compiler-rt and libunwind also carry a legacy dual licence (UIUC "BSD-like" or MIT, the user's choice) | `include/c++/v1/vector`: "Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions ... SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception"; upstream `llvmorg-22.1.8` `compiler-rt/LICENSE.TXT` and `libunwind/LICENSE.TXT`: "The LLVM Project is under the Apache License v2.0 with LLVM Exceptions"; "The libunwind library is dual licensed under both the University of Illinois "BSD-Like" license and the MIT license ... you may choose to use it under either license". **The release tarball carries no LICENSE.TXT** (searched the extracted tree); the text is only upstream | static archives in both binaries |
| Linux UAPI headers (`linux-libc-dev` 6.12.107-1 of the sysroot) | 6.12.107 | `GPL-2.0 WITH Linux-syscall-note` | `sysroot/usr/include/linux/fs.h` line 1; its `copyright`: "Files: * ... License: GPL-2" | compile-time headers only; the syscall note exists for programs that include them |

Build configuration only, no code in the binaries: `bazel_tools`, `platforms`,
`rules_cc` (`build_only` in `tools/sbom/pins.json`).

### 2. Bazel modules that are not linked

The resolved graph (`bazel mod graph`) is 32 modules: the six linked above
and these 26. "Fetched" means the directory is in the output base and its
licence file was read there.

| Module | Version | Licence as read | Use |
|---|---|---|---|
| aspect_bazel_lib | 2.19.3 | Apache-2.0 (`LICENSE`) | build rules (via rules_distroless, toolchains_llvm) |
| bazel_features | 1.50.0 | Apache-2.0 | build rules |
| bazel_lib | 3.1.0 | Apache-2.0 | build rules; its toolchains fetch tool binaries (section 3) |
| bazel_skylib | 1.9.2 | Apache-2.0 | build rules |
| gawk | 5.3.2.bcr.3 | GPL-3.0-or-later (`COPYING` is GPL v3) | build tool: `rules_distroless` builds and runs it from source; nothing of it is in an output |
| google_benchmark | 1.9.5 | Apache-2.0 | bench binaries only |
| googletest | 1.18.0.bcr.1 | BSD-3-Clause (`LICENSE`, "Neither the name of Google Inc.") | test binaries only |
| mboworks_bzl | 0.5.1 | Apache-2.0 | build rules (via toolchains_llvm) |
| platforms | 1.1.0 | Apache-2.0 | build configuration |
| protobuf | 35.0 | BSD-3-Clause (`LICENSE`) | dependency of gloop and rules_python; fetched, not linked |
| re2 | 2025-11-05.bcr.1 | BSD-3-Clause (`LICENSE`) | dependency of gloop; fetched, not linked |
| rules_cc | 0.2.25 | Apache-2.0 | build rules |
| rules_distroless | 0.9.4 | Apache-2.0 | build rules: the `apt` extension and the rootfs |
| rules_java | 9.3.0 | Apache-2.0 | build rules and the JDK toolchain |
| rules_license | 1.0.0 | Apache-2.0 | build rules (dependency of gloop) |
| rules_python | 1.9.0 | Apache-2.0 | build rules and the Python toolchain |
| rules_shell | 0.8.0 | Apache-2.0 | build rules |
| tar.bzl | 0.6.0 | Apache-2.0 | build rules |
| toolchains_llvm | 1.11.1 | Apache-2.0 | build rules |
| yq.bzl | 0.3.1 | Apache-2.0 | build rules |
| zlib | 1.3.2 | zlib licence (`LICENSE`: "This software is provided 'as-is', without any express or implied warranty") | dependency of gloop; fetched, not linked |
| boringssl | 0.20260413.0 | **unverified** (not fetched) | resolved only (dependency of gloop) |
| jq.bzl | 0.1.0 | **unverified** (not fetched) | resolved only |
| libpfm | 4.11.0.bcr.1 | **unverified** (not fetched) | resolved only (dependency of google_benchmark) |
| stardoc | 0.7.2 | **unverified** (not fetched) | resolved only |
| tcmalloc | 0.0.0-20250927-12f2552 | **unverified** (not fetched) | resolved only (dependency of gloop) |

### 3. Other pinned downloads and tools

| Input | Version | Licence as read | Where stated | Use |
|---|---|---|---|---|
| LLVM release (clang, lld, clang-tidy, llvm-nm, ...) | 22.1.8 (`LLVM-22.1.8-Linux-X64.tar.xz`) | Apache-2.0 WITH LLVM-exception | the headers carry the SPDX line (section 1); no LICENSE.TXT in the tarball | build tool; the runtime parts it supplies are in section 1 |
| Toolchain runtime libraries (the `runtime_debs` of `llvm_distribution`, a set of its own, not the rootfs packages of section 4 although some names coincide: libgcc-s1, libstdc++6 12.2.0-14+deb12u1, libicu72, libxml2, liblzma5, zlib1g) | as pinned in `MODULE.bazel` | **unverified**: these debs are not read (libgcc-s1 and libstdc++6 have no `copyright` file of their own; GCC's runtime is GPL-3.0-or-later WITH GCC-exception-3.1 upstream, not read here) | | shared libraries the pinned clang needs to run: build tool only, never linked into dcfs |
| Python | 3.12 (python-build-standalone, via rules_python) | PSF-2.0 | `lib/python3.12/LICENSE.txt` ("A. HISTORY OF THE SOFTWARE", the PSF licence); the libraries bundled into the standalone build are **unverified** | build tool and the tests' Python (standard library only: every `import` in `*.py` is the standard library) |
| JDK (`remotejdk21_linux`) | Azul Zulu 21.0.9+10 (`release`: `IMPLEMENTOR="Azul Systems, Inc."`) | GPL-2.0 WITH Classpath-exception | `legal/java.base/LICENSE` (GPL v2), `ASSEMBLY_EXCEPTION`, `ADDITIONAL_LICENSE_INFO` | runs TLC only; never distributed |
| bazel_lib tool binaries (uutils coreutils, zstd, bsdtar) | per bazel_lib | uutils coreutils MIT (`coreutils/LICENSE`, "Copyright (c) uutils developers"); zstd and bsdtar **unverified** (the fetched directories hold the binary only) | | build tools |
| uv (via rules_python) | per rules_python | **unverified** (binary only) | | build tool |
| pandoc | 3.12 | GPL-2.0-or-later ("Released under the GPL, version 2 or greater", `pandoc.1`); the archive has no COPYRIGHT file, so the licences of the static binary's Haskell libraries are **unverified** | the man page in the archive | build tool: renders `//man:dcfs.8` from README.md; the output is a man page, not pandoc code |
| Google style guides (`google_styleguide`) | commit b2a398d17f5e (gh-pages tip, 2026-10-17) | CC-BY-3.0 (`LICENSE`: "Creative Commons Legal Code, Attribution 3.0 Unported"; `README.md`: "The style guides in this project are licensed under the CC-By 3.0 License") | `external/+http_archive+google_styleguide/LICENSE` | build input: rendered to Markdown by pandoc for reviewers (`//third_party/google_styleguide:guides`); the rendered copies are derived works credited to Google in `third_party/google_styleguide/README.md`, never shipped |
| act | 0.2.89 | MIT | `external/+http_archive+act/LICENSE` | developer machine only (runs CI in Docker) |
| pjdfstest | 85a8aea9e6 | BSD-2-Clause | `COPYING`: "License for all regression tests available with pjdfstest ... Redistribution and use in source and binary forms, with or without modification, are permitted provided that ... 1. ... 2. ..." (two clauses) | test program run in the guests, built from the fetched source |
| SibylFS checker (`fs_test_check`, `fs_test_posix`: the prebuilt binaries of release 2015-10-05 of `sibylfs_binaries`) | 2015-10-05 | the code is ISC (`LICENSE.txt` of `sibylfs_src`: "Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies."); the binaries statically link OCaml's runtime (LGPL-2 with a static-linking exception), Lem's library (BSD-3-Clause; `pmap`, `pset`: LGPL-2), Sexplib (Apache-2.0), ctypes (MIT), `sha`, `unix-fcntl`, `unix-errno` (ISC) and `fd-send-recv` (LGPL-2.1 with the exception). **The distribution of the binaries states no licence**: no licence file in the repository or the archive | `third_party/sibylfs/README.md` has each source; read from the upstream repositories, the libraries by `nm` of the binaries, their versions unknown | test program run in the guests (differential oracle, step 12.18b); fetched by Bazel, never vendored, never in a release artifact. russ ruled (2026-10-17) that test-only use is fine and that caches are not redistribution |
| xfstests (`ltp/fsstress.c`, `ltp/fsx.c`, `src/log-writes`, helpers, the `check` scripts) | v2026.05.17 | GPL-2.0 (`LICENSES/GPL-2.0`; `check` is SPDX `GPL-2.0`, `common/rc` is `GPL-2.0+`, `ltp/fsstress.c` is `GPL-2.0`). **`ltp/fsx.c` states no licence** (header: "Copyright (C) 1991, NeXT Computer, Inc. All Rights Reserverd ... Rewritten 8/98 by Conrad Minshall ... Small changes to work under Linux -- davej."), and 42 C and header files of `src/`, `ltp/` and `lib/` carry no SPDX line (not all are built here) | `external/+http_archive+xfstests/` | test programs and scripts in the guests; never distributed |
| TLA+ tools (`tla2tools.jar`) | 1.7.4 | MIT (`License.txt`: "Copyright (c) 2019 Microsoft Research ... The MIT License (MIT)"); `META-INF/LICENSE.txt` is EPL-2.0 (for bundled parts); the jar also bundles `javax.mail`/`com.sun.mail` and `javax.activation` classes whose licences are **unverified** | inside the jar | model checker only |
| TLA+ CommunityModules (`CommunityModules-deps.jar`) | 202610040242 | MIT (`LICENSE`, "Copyright (c) 2019 TLA+"); the `-deps` jar bundles Apache Commons, JGraphT, JUNGrapht, Gson and SLF4J classes whose licences are **unverified** (no per-library licence file in the jar) | inside the jar | trace validation only |
| Debian cloud image (nocloud, Debian 13 trixie) | 20261001-2618 | per package inside the image (`/usr/share/doc/*/copyright`): **unverified**, the image is not in this output base | | boots in one systemd test guest; never distributed |

### 4. Test guests: Alpine and Debian packages

Neither set is linked into dcfs. They run only inside QEMU test guests (or as
host-side helpers run through musl's loader).

Alpine (branch v3.24; the `alpine_package` repositories of `MODULE.bazel`: the
kernel, QEMU and its closure, qemu-img, the filesystem tools, busybox, strace,
dmsetup, gawk, patchelf and the xfstests userland). `L:` is Alpine's own
licence field. The kernel is Alpine's `linux-virt` (origin `linux-lts`,
`GPL-2.0-only`); this repository carries no patch to it (`third_party/linux/`
holds the option list and its test only). Versions are what the fetch took;
the pin is the branch, and the index moves (the index read on 2026-10-16
has `linux-virt` 6.18.57, the resolved fetch 6.18.55; three packages of QEMU's
closure show the index's newer licence field for the same reason).

| Origin package | Version | `L:` | Repositories |
|---|---|---|---|
| acl | 2.3.2-r1 | LGPL-2.1-or-later AND GPL-2.0-or-later | xfstests-tools |
| attr | 2.5.2-r2 | LGPL-2.1-or-later | xfstests-tools |
| bash | 5.3.9-r1 | GPL-3.0-or-later | xfstests-tools |
| bc | 1.08.2-r1 | GPL-3.0-or-later | xfstests-tools |
| brotli | 1.2.0-r1 | MIT | qemu, qemu-img |
| btrfs-progs | 6.17.1-r1 | GPL-2.0-or-later | fstools, xfstests-tools |
| busybox | 1.37.0-r31 | GPL-2.0-only | busybox |
| bzip2 | 1.0.8-r6 | bzip2-1.0.6 | qemu, strace, xfstests-tools |
| capstone | 5.0.9-r0 | BSD-3-Clause-Clear | qemu |
| coreutils | 9.11-r0 | GPL-3.0-or-later | xfstests-tools |
| cyrus-sasl | 2.1.28-r9 | BSD-3-Clause-Attribution AND BSD-4-Clause | qemu |
| device-mapper | 2.03.35-r3 | GPL-2.0-or-later AND LGPL-2.1-or-later | dmsetup, xfstests-tools |
| diffutils | 3.12-r0 | GPL-3.0-or-later | xfstests-tools |
| dtc | 1.7.2-r1 | GPL-2.0-or-later OR BSD-2-Clause | qemu |
| e2fsprogs | 1.47.4-r0 | GPL-2.0-or-later AND LGPL-2.0-or-later AND BSD-3-Clause AND MIT | fstools, qemu, xfstests-tools |
| elfutils | 0.195-r0 | GPL-3.0-or-later AND ( GPL-2.0-or-later OR LGPL-3.0-or-later ) | qemu, strace |
| eudev | 3.2.14-r6 | GPL-2.0-or-later | fstools, xfstests-tools |
| file | 5.47-r2 | BSD-2-Clause | xfstests-tools |
| findutils | 4.10.0-r1 | GPL-3.0-or-later | xfstests-tools |
| fio | 3.41-r0 | GPL-2.0-only | xfstests-tools |
| gawk | 5.3.2-r2 | GPL-3.0-or-later | gawk, xfstests-tools |
| gcc | 15.2.0-r5 | GPL-2.0-or-later AND LGPL-2.1-or-later | patchelf, qemu |
| gdbm | 1.26-r0 | GPL-3.0-or-later | qemu |
| gettext | 1.0-r0 | LGPL-2.1-or-later | qemu, qemu-img, xfstests-tools |
| glib | 2.88.1-r1 | LGPL-2.1-or-later | qemu, qemu-img |
| gmp | 6.3.0-r4 | LGPL-3.0-or-later OR GPL-2.0-or-later | qemu, qemu-img |
| gnutls | 3.8.13-r0 | LGPL-2.1-or-later | qemu, qemu-img |
| grep | 3.12-r0 | GPL-3.0-or-later | xfstests-tools |
| inih | 62-r0 | BSD-3-Clause | fstools, xfstests-tools |
| libaio | 0.3.113-r2 | LGPL-2.1-or-later | qemu, qemu-img, xfstests-tools |
| libbpf | 1.7.0-r1 | LGPL-2.1-only OR BSD-2-Clause | qemu |
| libcap | 2.78-r0 | BSD-3-Clause OR GPL-2.0-only | xfstests-tools |
| libcap-ng | 0.8.5-r2 | GPL-2.0-or-later AND LGPL-2.1-or-later | qemu |
| libeconf | 0.8.3-r0 | MIT | fstools, qemu, qemu-img, xfstests-tools |
| libedit | 20260508.3.1-r1 | BSD-3-Clause | xfstests-tools |
| libffi | 3.5.2-r1 | MIT | qemu, qemu-img |
| libidn2 | 2.3.8-r0 | GPL-2.0-or-later OR LGPL-3.0-or-later | qemu, qemu-img |
| libjpeg-turbo | 3.1.3-r0 | BSD-3-Clause AND IJG AND Zlib | qemu |
| libnl3 | 3.11.0-r0 | LGPL-2.1-or-later | qemu |
| libpng | 1.6.59-r0 | Libpng | qemu |
| libseccomp | 2.6.1-r0 | LGPL-2.1-or-later | qemu |
| libslirp | 4.9.1-r0 | BSD-3-Clause | qemu |
| libtasn1 | 4.21.0-r0 | LGPL-2.1-or-later | qemu, qemu-img |
| libunistring | 1.4.2-r0 | GPL-2.0-or-later OR LGPL-3.0-or-later | qemu, qemu-img |
| liburing | 2.14-r0 | LGPL-2.1-or-later OR MIT | qemu, qemu-img |
| linux-lts | 6.18.55-r0 | GPL-2.0-only (index on 2026-10-16: 6.18.57-r0) | kernel |
| lzo | 2.10-r5 | GPL-2.0-or-later | fstools, qemu, xfstests-tools |
| musl | 1.2.6-r2 | MIT ; MIT AND BSD-2-Clause AND GPL-2.0-or-later | dmsetup, fstools, gawk, patchelf, qemu, qemu-img, strace, xfstests-tools |
| musl-fts | 1.2.7-r7 | BSD-3-Clause | qemu, strace |
| ncurses | 6.6_p20260516-r0 | X11 | xfstests-tools |
| nettle | 3.10.2-r0 | GPL-2.0-or-later OR LGPL-3.0-or-later | qemu, qemu-img |
| numactl | 2.0.19-r0 | LGPL-2.1-only | qemu, qemu-img, xfstests-tools |
| openssl | 3.5.9-r0 | Apache-2.0 | xfstests-tools |
| p11-kit | 0.26.2-r0 | BSD-3-Clause | qemu, qemu-img |
| patchelf | 0.18.0-r3 | GPL-3.0-or-later | patchelf |
| pax-utils | 1.3.9-r1 | GPL-2.0-only | xfstests-tools |
| pcre2 | 10.49-r0 | BSD-3-Clause | qemu, qemu-img, xfstests-tools |
| perl | 5.42.2-r1 | Artistic-1.0-Perl OR GPL-1.0-or-later | xfstests-tools |
| pixman | 0.46.4-r0 | MIT | qemu |
| procps-ng | 4.0.6-r0 | GPL-2.0-or-later AND LGPL-2.1-or-later | xfstests-tools |
| psmisc | 23.7-r0 | GPL-2.0-or-later | xfstests-tools |
| qemu | 11.0.3-r0 | GPL-2.0-only AND LGPL-2.1-only | qemu, qemu-img |
| readline | 8.3.3-r1 | GPL-3.0-or-later | xfstests-tools |
| sed | 4.9-r2 | GPL-3.0-or-later | xfstests-tools |
| skalibs | 2.15.0.0-r0 | ISC | xfstests-tools |
| snappy | 1.2.2-r0 | BSD-3-Clause | qemu |
| strace | 6.19-r1 | BSD-3-Clause | strace |
| userspace-rcu | 0.15.3-r0 | LGPL-2.1-or-later | fstools, xfstests-tools |
| util-linux | 2.42.3-r1 | LGPL-2.1-or-later ; BSD-3-Clause ; GPL-3.0-or-later AND GPL-2.0-or-later AND GPL-2.0-only AND GPL-1.0-only AND LGPL-2.1-or-later AND BSD-1-Clause AND BSD-3-Clause AND BSD-4-Clause-UC AND MIT AND Public-Domain ; GPL-2.0-or-later ; MIT ; BSD-4-Clause-UC ; GPL-1.0-only | fstools, qemu, qemu-img, xfstests-tools |
| utmps | 0.1.3.3-r0 | ISC | xfstests-tools |
| vde2 | 2.3.3-r6 | GPL (index on 2026-10-16: 2.3.3-r7) | qemu |
| which | 2.23-r0 | GPL-2.0-or-later | xfstests-tools |
| xen | 4.21.2-r1 | GPL-2.0-only (index on 2026-10-16: 4.21.2-r2) | qemu |
| xfsprogs | 7.0.1-r0 | LGPL-2.1-or-later | fstools, xfstests-tools |
| xz | 5.8.4-r0 | GPL-2.0-or-later AND 0BSD AND Public-Domain AND LGPL-2.1-or-later | qemu, strace, xfstests-tools |
| yajl | 2.1.0-r9 | MIT | qemu |
| zlib | 1.3.2-r1 | Zlib | fstools, qemu, qemu-img, strace, xfstests-tools |
| zstd | 1.5.7-r2 | BSD-3-Clause OR GPL-2.0-or-later | fstools, qemu, qemu-img, strace, xfstests-tools |

Debian (the NFS rootfs, `third_party/debian/debs.lock`: 149 `.deb`s of
bookworm; 131 were on disk to read, grouped here by identical `copyright`
file). The licences are the `License:` fields of each package's DEP-5 file;
`Files: *` is the licence the package states for the bulk, "others" are
the other licences the file names (a sample).

| Packages | `Files: *` | Others in the file |
|---|---|---|
| adduser | GPL-2+ |  |
| attr, libattr1 | GPL-2+ | LGPL-2+ |
| base-files | free-form file naming: GNU General Public License, Artistic |  |
| bash | GPL-3+ | GPL-3+ with Bison exception; GPL-2+; GFDL-NIV-1.3; Latex2e; BSD-4-clause-UC and MIT-like; BSD-4-clause-UC; MIT-like; permissive |
| btrfs-progs | GPL-2 | LGPL-2.1+; GPL-2+ |
| coreutils | GPL-3+ | GPL-3+ and BSD-4-clause-UC; BSD-4-clause-UC; GPL-3+ and ISC; ISC; FSFULLR; GFDL-NIV-1.3 |
| debconf | BSD-2-clause |  |
| debianutils | GPL-2+ | public-domain; SMAIL-GPL |
| diffutils | GPL-3+ | FSFULLR; GPL-3+ and FSFULLR; LGPL-2.1+; GPL-3+ with autoconf exception; GPL-3+ with texinfo exception; LGPL-2.0+; GPL-2+; X11; FSFAP; GFDL-N |
| dmsetup, libdevmapper1.02.1 | GPL-2.0 | LGPL-2.1; BSD-2-Clause; GPL-2.0+ |
| dpkg | GPL-2+ | GPL-2; BSD-2-clause; public-domain-s-s-d |
| e2fsprogs, libcom-err2, libext2fs2, libss2, logsave | GPL-2 | LGPL-2; BSD-3-Clause; Apache-2; ISC; GPL or MIT-US-export; Kazlib; Latex2e; GPL-2+ with Texinfo exception |
| findutils | GFDL-NIV-1.3+ | GPL-3+; FSFAP; GPL-3+ with Autoconf-data exception; FSFULLR; GPL-2+ with Autoconf-data exception; GPL-2+; X11; public-domain; GPL with autom |
| gcc-12-base | free-form file naming: GNU General Public License, BSD, MIT, public domain, Artistic |  |
| grep | GPL-3+ |  |
| init-system-helpers | BSD-3-clause | GPL-2+ |
| iproute2 | GPL-2 |  |
| keyutils, libkeyutils1 | GPL-2+ | LGPL-2+ |
| libacl1 | GPL-2+ | LGPL-2+ |
| libapparmor1 | GPL-2+ | BSD-3-clause or GPL-2+; LGPL-2.1+; GPL-2; BSD-3-clause |
| libargon2-1 | CC0 or Apache-2.0 | CC0; Apache-2.0 |
| libaudit-common, libaudit1 | GPL-2 | LGPL-2.1 |
| libblkid1, libfdisk1, libmount1, libsmartcols1, libuuid1, mount, util-linux, util-linux-extra | GPL-2+ | GPL-2; GPL-3+; public-domain; BSD-4-clause; MIT; BSD-3-clause; BSLA; LGPL-2+; LGPL-2.1+; LGPL; LGPL-3+ |
| libbpf1 | LGPL-2.1 or BSD-2-Clause | GPL-2.0; GPL-2 with Linux-syscall-note exception; GPL-2+; LGPL-2.1; BSD-2-Clause |
| libbsd0 | BSD-3-clause | BSD-4-clause-Niels-Provos; BSD-3-clause-Regents; BSD-3-clause-Regents and BSD-2-clause-NetBSD; BSD-3-clause-author; BSD-3-clause-John-Birrel |
| libbz2-1.0 | BSD-variant | GPL-2 |
| libc6 | free-form file naming: GNU General Public License, BSD, MIT |  |
| libcap-ng0 | LGPL-2.1+ | GPL-2+; GPL-3 |
| libcap2, libcap2-bin | BSD-3-clause or GPL-2 | BSD-3-clause or GPL-2+; BSD-3-clause; GPL-2; GPL-2+ |
| libcrypt1 | free-form file naming: BSD, public domain |  |
| libcryptsetup12 | GPL-2+ with OpenSSL exception | GPL-2+; GPL-3+; LGPL-2.1+; LGPL-2.1+ with OpenSSL exception; CC0 or Apache-2.0; public-domain; CC0; Apache-2.0 |
| libdb5.3 | Sleepycat and BSD-3-clause | Sleepycat; BSD-3-clause; Ms-PL; GPL or Artistic; X11; MIT-old; TCL-like; BSD-3-clause-fjord; GPL-3; zlib; Artistic or BSD-3-clause; GPL |
| libdbus-1-3 | GPL-2+ or AFL-2.1, and Expat and Tcl-BSDish | GPL-2+; BSD-3-clause and BSD-3-clause-generic; autoconf-archive-permissive; FSF-unlimited-permission; GPL-2+ with Autoconf exception; LGPL-2 |
| libedit2 | BSD-3-clause |  |
| libelf1 | GPL-3+ | LGPL-3+ or GPL-2+; BSD-2-clause; GFDL-NIV-1.3; LGPL-2.1+; GPL-3+ with Bison exception |
| libelogind0 | LGPL-2.1+ | CC0-1.0; Expat |
| libffi8 | Expat | X11; GPL-2+; GPL-3+; MPL-1.1 or GPL-2+ or LGPL-2.1+; public-domain; GPL |
| libgdbm-compat4, libgdbm6 | GPL-3+ | GPL-2+; GFDL-NIV-1.3+ |
| libgmp10 | GPL-2+ or LGPL-3+ | GPL-3+; GPL-3+ with Bison exception; GPL-2+; LGPL-3+ |
| libgpg-error0 | LGPL-2.1+ | LGPL-2.1+ or BSD-3-clause; g10-permissive; GPL-3+; BSD-3-clause |
| libhogweed6, libnettle8 | LGPL-3+ or GPL-2+ | LGPL-2+; Expat; GPL-3+; GPL-2+; GPL-3+ with Autoconf exception; public-domain; GPL-2; GAP; LGPL-3+ |
| libidn2-0 | GPL-3+ | LGPL-3+ or GPL-2+; Unicode; GPL-2+; LGPL-3+ |
| libinih1 | BSD-3-Clause |  |
| libip4tc2, libxtables12 | GPL-2 | Artistic; GPL-2+; custom |
| libjson-c5 | Expat |  |
| libkmod2 | LGPL-2.1+ | GPL-2+ |
| libldap-2.5-0 | OpenLDAP-2.8 | FSF-unlimited and GPL-2+ with Autoconf exception; GPL-3+ with Autoconf exception; FSF-unlimited; GPL-2+ with Libtool exception and GPL-3+ wi |
| liblz4-1 | BSD-2-clause | GPL-2+; GPL-2+ or BSD-2-clause |
| liblzo2-2 | GPL-2+ |  |
| libmd0 | BSD-3-clause | BSD-3-clause-Aaron-D-Gifford; BSD-2-clause; BSD-2-clause-NetBSD; ISC; Beerware; public-domain-md4; public-domain-md5; public-domain-sha1 |
| libmnl0 | LGPL-2.1 | GPL-2+ |
| libnfsidmap1, nfs-common, nfs-kernel-server | free-form file naming: GNU General Public License, MIT |  |
| libnl-3-200, libnl-genl-3-200 | free-form file naming: GNU General Public License, GNU Lesser General Public License, MIT |  |
| libnsl2 | LGPL-2.1+ | LGPL-2.1; BSD-3-clause; permissive-fsf; permissive-makefile-in; permissive-autoconf-m4-no-warranty; GPL-3+-autoconf-exception; permissive-co |
| libnumber-compare-perl | Artistic or GPL-1+ | Artistic; GPL-1+ |
| libp11-kit0 | BSD-3-Clause | permissive-like-automake-output; ISC; ISC+IBM; LGPL-2.1+; Apache-2.0; same-as-rest-of-p11kit |
| libpam-modules, libpam-modules-bin, libpam0g | BSD-3-clause or GPL | GPL-2; GPL-2+; GPL-3; GPL-3+ with Bison exception; BSD-tcp_wrappers; LGPL-2+; public-domain; Beerware |
| libproc2-0, procps | LGPL-2.1+ | LGPL-2.0+; GPL-2.0+ |
| libpython3-stdlib, python3, python3-minimal | free-form file naming: GNU General Public License, MIT |  |
| libpython3.11-minimal, python3.11, python3.11-minimal | free-form file naming: GNU General Public License, BSD, MIT |  |
| libreadline8, readline-common | GPL-3+ | GPL-2+; GFDL-NIV-1.3+; ISC-no-attribution; GPL-3 |
| libsasl2-2, libsasl2-modules-db | BSD-4-clause | OpenSSL and SSLeay; BSD-3-clause; BSD-2-clause; GPL-3+; GPL-3; BSD-4-clause-UC; RSA-MD; BSD-4-clause and BSD-4-clause-KTH; BSD-4-clause and  |
| libseccomp2 | LGPL-2.1 |  |
| libselinux1 | free-form file naming: GNU General Public License, GNU Lesser General Public License, MIT, public domain |  |
| libsemanage-common, libsemanage2 | free-form file naming: GNU General Public License |  |
| libsepol2 | LGPL-2.1+ | Zlib; GPL-2; GPL-2+ |
| libsqlite3-0 | public-domain | GPL-2+ |
| libsystemd-shared, libsystemd0, libudev1, systemd, systemd-sysv | LGPL-2.1+ | CC0-1.0; GPL-2 with Linux-syscall-note exception; Expat; public-domain; GPL-2+ |
| libtext-glob-perl | Artistic or GPL-1+ | Artistic; GPL-1+ |
| libtinfo6 | MIT/X11 | X11; BSD-3-clause |
| libtirpc-common, libtirpc3 | BSD-3-Clause | GPL-2; __AUTO_PERMISSIVE__; BSD-2-Clause; BSD-4-Clause; LGPL-2.1+; PERMISSIVE |
| libunistring2 | LGPL-3+ or GPL-2+ | FreeSoftware; GPL-3+ or GFDL-1.2+; GPL-3+; GPL-2+; GPL-2+ with distribution exception; MIT; LGPL-3+; GFDL-1.2+ |
| libunwind8 | Expat | GPL-2+ |
| liburcu8 | LGPL-2.1+ | GPL-2+; FSFUL; GPL-3+; MIT-MINIMAL; Expat; LGPL-2.1; GPL-2; BSD-2-clause |
| libwrap0 | TCP-wrappers | BSD-3-clause; SSH |
| libzstd1 | BSD-3-clause or GPL-2 | zlib; Expat; GPL-2; BSD-3-clause |
| mawk | GPL-2 | X11; CC-BY-3.0 |
| media-types | ad-hoc |  |
| netbase | GPL-2 |  |
| passwd | BSD-3-clause | GPL-1; GPL-2+; public-domain |
| quota | free-form file naming: GNU General Public License, BSD |  |
| rpcbind | BSD-3-Clause | MIT; PERMISSIVE; GPL-2.0+; BSD-4-clause; BSD-4-Clause |
| sensible-utils | GPL-2+ | All-permissive; configure; installsh |
| strace | free-form file naming: GNU General Public License, GNU Lesser General Public License |  |
| sysvinit-utils | GPL-2.0+ | GPL-3.0; GPL-2.0 |
| tar | GPL-3+ | GPL-3+ with Bison exception; LGPL-3+; GPL-2+ |
| ucf | GPL-2 |  |
| usrmerge | GPL-2+ |  |
| xfsprogs | GPL-2 | LGPL-2.1; GPL-3+ with autoconf exception |
| zlib1g | Zlib |  |

Not read: 4 packages had no `copyright` file of their own (libgcc-s1,
libstdc++6, libncursesw6, libpython3.11-stdlib: they point at their source
package's directory) and 18 of the 149 `.deb`s were not fetched by the build
into this output base, so not read (libevent-core, libexpat1,
libfile-find-rule-perl, libgcrypt20, libgnutls30, libgssapi-krb5-2, libicu72,
libk5crypto3, libkrb5-3, libkrb5support0, liblzma5, libpcre2-8-0, libperl5.36,
libssl3, libtasn1-6, perl-base, perl-modules-5.36, perl). Their licences are
**unverified**.

### 5. CI

| Input | Pin | Licence as read | Use |
|---|---|---|---|
| actions/checkout | `3d3c42e5aac5` (v7.0.1) | MIT ("The MIT License (MIT) Copyright (c) 2018 GitHub, Inc. and contributors", `LICENSE` at that commit) | workflows |
| actions/cache | `55cc83458639` (v6.1.0) | MIT (same text) | workflows |
| actions/upload-artifact | `ea165f8d65b6` (v4.6.2) | MIT (same text) | workflows |
| actions/download-artifact | `d3f86a106a0b` (v4.3.0) | MIT (same text) | workflows |
| google/osv-scanner-action | `a345acffa64b` (v2.6.0) | Apache-2.0 (`LICENSE` at that commit); the osv-scanner it runs is **unverified** here | `osv` job |
| Bazelisk | v1.29.0 (`prepare.sh`) | Apache-2.0 (`LICENSE` of `bazelbuild/bazelisk`, master) | downloaded by `prepare.sh` on runners; not in the repository |
| the ubuntu-24.04 hosted runner; the `act` runner image `ghcr.io/catthehacker/ubuntu@sha256:62d572...` | not ours | **unverified** (GitHub's and catthehacker's images) | where the jobs run |

## What is redistributed

- **The repository** is public (GitHub reports `isPrivate: false`, licence
  MIT). It holds our files, the patches in `third_party/*/`, build overlays
  and lock files; no third-party source. Its history holds removed patches
  to GPL and LGPL code (`third_party/libfuse/0001-attr-generation.patch`,
  `third_party/qemu/0001-add-dcfs-device-config.patch`, the ubdsrv patches of
  2023), none in the tree.
- **CI artifacts** (`upload-artifact`): Bazel profiles, test logs and
  `outputs.zip` (guest consoles), the lcov report, the badge JSON. They hold
  our coverage and log text, not third-party binaries.
- **CI caches** (`actions/cache`): the Bazel repository cache (the downloaded
  archives: the LLVM tarball, Alpine packages, xfstests and pjdfstest source,
  the Debian `.deb`s), the disk cache (compiled objects, including those of
  libfuse, abseil and the glibc pieces) and the extracted LLVM toolchain.
  Settled (russ, 2026-10-17: "No, it's fine since we're not shipping it"):
  these caches are not redistribution, because nothing from them ships.
- **No binary of dcfs is distributed today.** Nothing below is triggered
  until one is.

## Obligations if a dcfs binary is shipped

What each linked component asks for in a distributed `main_static` (and, where
noted, `main`). This is a reading of the licence texts, not legal advice.

| Component | Obligation |
|---|---|
| abseil-cpp, gloop (Apache-2.0) | Give recipients a copy of the licence; keep copyright and attribution notices; carry any NOTICE file (there is none in either archive); state changes (we change none of their sources). |
| LLVM runtimes (Apache-2.0 WITH LLVM-exception) | The exception lets the embedded portions be redistributed in object form without Apache sections 4(a), 4(b) and 4(d) (the licence copy, the change notice, the NOTICE carry); nothing is owed for a binary. Redistributing the source would need the licence text, which is not in the tarball and has to come from the upstream tag. |
| SQLite (public domain) | None. |
| liburing (MIT elected) | Keep the copyright and permission notice "in all copies or substantial portions of the Software": ship the MIT text with the binary. Electing MIT leaves LGPL-2.1 out of liburing. |
| libfuse `lib/`, libnuma (LGPL-2.1) | Linked statically in `main` and `main_static`. LGPL-2.1 section 6 lets a combined work be distributed under terms of our choice provided recipients can modify the library and relink: ship the complete corresponding source of the work or the object files, with the licence text and a notice that the library is used. dcfs's own source is MIT and public and the build is a pinned Bazel build, so a recipient can rebuild with a modified libfuse through `--override_module=libfuse=<dir>`; that recipe has not been exercised. |
| glibc (LGPL-2.1+, `main_static` only) | The same section 6 terms for a static glibc: the heaviest obligation here. `main` avoids it (the host's glibc is used dynamically). Relinking against a modified glibc needs a rebuilt `libc6-dev` in `sysroot_debs`. |
| Linux UAPI headers | None for the binary: the Linux-syscall-note exempts programs that use the headers. |
| Everything in sections 2-5 | Not in the binary. Source or notice duties arise only if the tool, image or archive itself is redistributed (the cache question). |

**Shipping a binary** (russ, 2026-10-17): a release binary is ideally a goal,
and may require the system's glibc and libfuse to be installed (a dynamically
linked `main`); the static target stays for people who build by hand. The
build change is step 7.9, parked late. The relink recipe stays unmeasured
until then.

A notices file for the shipped set (`THIRD_PARTY_NOTICES`, assembled by Bazel
from the pinned archives' licence files, with `http_file`s for LLVM's licence
text) was planned in 7.8 and is not built.

## Our own tree

`LICENSE` is MIT with no other grant. The SPDX gate covers `.cc .h .c .py .sh
.bzl .bazel .tla .java .tpl .sql .yml .yaml`, `BUILD`, `BUILD.bazel`,
`MODULE.bazel`, `.bazelrc`, `.clang-format`, `.clang-tidy` and
`test/qemu/guest/init`, minus fixtures and test data (`tools/repo_shape.py`,
`SPDX_*`). Every covered file has a header; every non-MIT header is listed in
`tools/third_party_files.txt` with a provenance.

Outside the gate and without headers, by design: Markdown, `*.txt` lists,
`*.cfg` model configurations, `*.query` matchers, fixture and testdata
directories (sample inputs), `.claude/agents/` and `CLAUDE.md`, and
`third_party/e2fsprogs/mke2fs.conf` (below).

### Third-party-derived content in the tree

| File | Derived from | Licence of the source | Marking |
|---|---|---|---|
| `tools/fhtest.c`, `test/qemu/guest/init`, `test/qemu/scripts/mkinitramfs.sh`, `test/qemu/scripts/run-qemu.sh` | russ's own `fuse-generation-qemu` repository (one commit, by Russell Harmon, no licence file) | his own copyright | headed `MIT`, with "Copied from" / "Adapted from" |
| `tools/testutil.c` (`BTRFS_IOC_SUBVOL_CREATE` and `struct btrfs_ioctl_vol_args`, `FS_IOC_SHUTDOWN`, the ext4 tune ioctl and `struct ext4_tune_sb_params`), `tools/kernel_bugs/ext4_casefold_tune/casefold_helper.c` | the kernel's UAPI headers `<linux/btrfs.h>`, `<linux/fs.h>`, `<linux/ext4.h>` | `GPL-2.0 WITH Linux-syscall-note` (line 1 of each header in the kernel tree) | headed `MIT`; the comments name the licence and the note |
| `dcfs/syscalls_backing.h`: `struct linux_dirent64` | the kernel's `include/linux/dirent.h`: `SPDX-License-Identifier: GPL-2.0`, no syscall note, not a UAPI header (the sysroot defines no such struct) | GPL-2.0 for the header; the five-field layout is the getdents64 ABI | headed `MIT`; the comments are our own words and name the kernel header |
| `third_party/tlaplus/overrides/tlc2/overrides/TLCOverrides.java` | CommunityModules' class of the same name | MIT | headed `MIT`, listed with provenance |
| `third_party/e2fsprogs/mke2fs.conf` | e2fsprogs 1.47.4's `misc/mke2fs.conf.in`, apart from a header comment of ours and one change | `GPL-2` for `Files: *` in Debian's e2fsprogs `copyright`; no header of its own | unheaded data file; listed test-only `GPL-2.0-only` in `tools/third_party_files.txt` and in `third_party/e2fsprogs/README.md` (question 5) |
| `third_party/xfstests/shim/xfs/*.h`, `glibc_strerror.c` | written here as stand-ins that follow xfsprogs layouts "as far as the compiler needs" | n/a | headed `MIT` |
| `tools/testutil.c` names corpus | written independently "in the spirit of" xfstests generic/453 and generic/454 (GPL-2.0, Oracle) without copying code | n/a | stated in its comment |

### Patches in `third_party/`

| Patch | Modifies | Licence of what it modifies | Marking |
|---|---|---|---|
| `xfstests/0001-dcfs-guest.patch` | `check` (`GPL-2.0`) and `common/rc` (`GPL-2.0+`) | GPL-2.0-only for the combination | `tools/third_party_files.txt`: `GPL-2.0-only`, test-only |
| `pjdfstest/0001-linux-portability.patch` | `tests/conf`, `tests/misc.sh` | BSD-2-Clause (`COPYING`) | `tools/third_party_files.txt`: `BSD-2-Clause`, test-only |
| `liburing/0001-absolute-cc-for-configure.patch` | the BCR overlay `BUILD.bazel` of liburing (its line 1 is `SPDX-License-Identifier: Apache-2.0`) | Apache-2.0 | `tools/third_party_files.txt`: `Apache-2.0`, role build; the added lines are ours (MIT), the context lines the overlay's |
| `numactl/0001-no-version-script.patch` | the BCR overlay `BUILD.bazel` of numactl (no header; the BCR's licence for overlays is **unverified**) | **unverified** | not listed (two lines of build file); question 7 |

The patch files themselves carry no SPDX line (a patch is a diff that
`patch` applies, and the gate does not read `.patch`); their marking is the
row above and this table.

There are no kernel patches in the tree. `third_party/linux/` holds
`required_options.txt`, a Python test and a README: the test kernel is Alpine's
`linux-virt`, unpatched. The kernel patches russ sends upstream
(`docs/plan/notes/kernel-patches.md`) and their test suite
(`~/Sources/fuse-generation-qemu`) live in other trees; a patch to the kernel
is GPL-2.0 and would be marked so if it ever landed here.

### SibylFS

Its checker is in section 3; the pins are `third_party/sibylfs/README.md`.
Nothing from SibylFS is in the tree. `formal/dcfs.tla` and `design.md` cite its
vocabulary (call, effect, return), which is an idea, not text.

## Open questions

Pending russ's decision.

1. **LLVM's licence text** is not in the release tarball; if sources or the
   toolchain are ever redistributed the text must be pinned from the upstream
   tag (7.8's plan was `http_file`s of the five `LICENSE.TXT`s).
2. **`ltp/fsx.c`** has no licence grant. It is built from the fetched archive
   for tests and never distributed, but a file with only a 1991 NeXT
   copyright line is not covered by xfstests's GPL-2.0 tree. Keep, replace, or
   accept?
3. **`third_party/e2fsprogs/mke2fs.conf`** is e2fsprogs's file apart from a
   header comment of ours and one change; it is test-only data. Keep marking
   it GPL, or treat the profile as uncopyrightable configuration?
4. **Alpine's `vde2`** (the origin of `vde2-libs`) states `L: GPL` with no
   version in the index, so the guest's QEMU closure has a package whose
   licence cannot be named exactly. Test-only; noted.
5. **Unverified licences** (listed above): five unfetched Bazel modules, the
   toolchain's runtime `.deb`s, 18 Debian rootfs debs, the Debian cloud
   image, the bundled libraries of the TLA+ jars and of the standalone Python,
   zstd, bsdtar and uv binaries, pandoc's Haskell libraries, the BCR
   overlay licence of numactl. All are build or test inputs. Reading them
   needs archives the build does not fetch; a later pass can close them.
6. **The gate misses the `BUILD.<name>` overlays** and `.patch` files; making
   it read both is a small change to `tools/repo_shape.py`, not made yet.
