# Build speed: where the time goes and what to do (investigation, 2026-10-07)

Status: measurements complete (second pass, 2026-10-06 evening, lane-4
checkout `investigate-build-speed`; the first pass stopped early because the
host ran out of memory). Sections 1.3 to 1.6 and the last section carry the
numbers; section 4 is the revised recommendation. Nothing in the tree was
changed. Raw data (aquery JSON, Bazel profiles, serial logs, the downloaded
kernels) is in the session scratch directory, not in the repository.

Host: 4 cores, 11.9 GB RAM (swap in use, 6-7 GB), `--jobs=2`,
`--local_resources=memory=3500`, Bazel server heap 1500 MB.
First pass: load average 8-19 (lane-3's Bazel plus a QEMU test), so every
wall time from section 1.2 and 1.3 is inflated.
Second pass: 1-minute load average 3-8 for the cold builds in 1.6 (other
lanes' Bazel servers were up, one or two ran at a time) and 7-12 for the
guest boots in the last section; the first cold kernel run in 1.6 happened at
load 70-85 with heavy swapping and is reported separately. Each number says
which.

## 1. Where the time goes

### 1.1 What is in the graph (aquery `deps(//...)`, plain config)

9,292 actions. Excluding file writes, symlink trees and the 2,006
`CcAutoconfCheck` probe actions (cheap, from rules_cc_autoconf):

| Group | Actions |
|---|---|
| CppCompile total | 1,457 |
| of which build-tool bootstraps: m4 524, gawk 74, make 64, pkgconf 50, rules_cc_autoconf 34 | ~750 |
| abseil | 442 compiles, 85 archives, 85 links |
| dcfs (+testonly, bench, tools) | 60 compiles, ~70 links, ~25 genrule initramfs |
| glib 99, pcre2 30, zlib 15 (QEMU's deps) | 144 compiles |
| libfuse 17, google_benchmark 19, googletest 15, numactl 6, liburing 5, sqlite3 1 | 63 compiles |
| rules_foreign_cc configure_make | 9 (QEMU, e2fsprogs x2, libarchive x2, xfsprogs, btrfs-progs, util-linux, urcu) |
| Genrules: kernel 1, busybox 1, bc 1, Debian rootfs 1, ~150 Debian `.deb` data extractions | ~190 |
| Test runners | 138 |

About half of all compile actions are not ours and not third-party libraries
we link: they bootstrap m4/gawk/make/pkgconf for rules_foreign_cc. They are
BCR modules (`m4`, `gawk`, `make`, `pkgconf`, `rules_cc_autoconf`, all in the
lock file already) and aquery shows every one of them in the exec
configuration (`k8-opt-exec`), so no `--copt` reaches them; the cold cost of
this bootstrap is in 1.6.

### 1.2 Warm build

`bazel build //...` on this tree (action cache plus shared disk cache):
**112 s wall, critical path 10 s**, 4,894 actions, 94 disk-cache hits, 512
internal. Almost all of it is loading/analysis (skyframe 108 s of the
profile), not compiling. Bazel 9 has no `analyze-profile`; the JSON trace was
parsed directly.

### 1.3 Cold C++ and Bazel-native third party (partial, measured)

I could not expunge, so I busted only the compile keys with
`--per_file_copt=...@-DBUSTA1` on our sources and on abseil, sqlite3,
googletest, google_benchmark, libfuse, liburing, numactl, inih, with
`--disk_cache=` off, `--jobs=2`. The run was interrupted at 607 s wall
(critical path 94.7 s, 344 processes). Profile data:

| Part | Compiles done | Compile CPU |
|---|---|---|
| third-party Bazel-native | 194 | 709 s (sqlite3.c alone 93 s, cctz time_zone_info.cc 21 s, gtest.cc 19 s) |
| dcfs | 37 | 353 s (metadata_cache_test.cc 31 s, backing_test.cc 28 s) |
| bench, tools | 5 | 14 s |
| links/archives | ~75 | 13 s |

So roughly 1,090 CPU-seconds for the compiles that finished, about 10
minutes wall at 2 jobs on a loaded host; the run did not finish, so this is a
lower bound.

The busybox genrule re-ran (91 s) in that run. Second pass, explained as far
as the evidence goes:

- Its action key is identical under plain, `--config=asan`, `--config=ubsan`
  and with `--per_file_copt=abseil-cpp.*@-DBUSTA1`
  (`c22f95f3...` in all four `aquery` outputs). Inputs are the 2,986 files of
  `@busybox//:srcs`, `build_busybox.sh`, the config fragment and
  `genrule-setup.sh`; environment is only `PATH=/bin:/usr/bin:/usr/local/bin`;
  no toolchain file is among them. Nothing a `--copt` or `--per_file_copt`
  can reach.
- Measured: `bazel build //third_party/busybox:busybox_build` after a plain
  build, then with the `--per_file_copt` change, then under `--config=asan`
  (together with bc and the kernel): "1 action cache hit" each time, 0.9 s to
  2.8 s, busybox never executed. The same holds for the kernel and bc
  genrules (they ran in the asan build only because lane-4 had never built
  them: the checkout's output base had no kernel or bc outputs, and the
  run had `--disk_cache=` off).
- What does re-run a genrule: a change of `--action_env` (genrules use the
  default shell env: 1 busybox, 1 bc, 1 kernel, 1 Debian rootfs and 149
  `.deb` genrules all changed key with `--action_env=X=1`), a fresh output
  base, or an empty disk cache. The 1.3 run had `--disk_cache=` empty, and
  each lane has its own output base; the most likely explanation is that
  this was busybox's first execution in that lane, not a consequence of the
  flag change. I cannot prove it (the run's profile was not kept), but no
  key-level cause exists.

### 1.4 From the logs (not re-measured)

- Kernel: ~9 min cold (3a merge), 28 min under load (5.1), 40 min inside
  act (5.2: the act cold job took 1h04 total). `build_kernel.sh` runs
  `nice -n 19 make -j4` regardless of `--jobs`. bzImage is 3.4 MB.
- QEMU+glib: ~450-500 s cold (6.2 follow-up note); qemu-system-x86_64 is
  11.4 MB.
- Debian rootfs: ~40 s once the packages are fetched; 805 MB sparse ext4.
- Full suite 35-42 min wall; this is test time, not build time.
- The rest are measured in 1.6 (second pass).

### 1.5 What rebuilds when (measured)

Method: `bazel aquery 'deps(//test/qemu:boot_test)' --output=jsonproto` under
plain, `--config=asan` and `--config=ubsan` (7,880 actions each); actions
matched by (target, mnemonic, primary output path); "changed" means the
action key differs, "propagated" means it differs or an input is produced
by a changed action. `FileWrite` contents (the foreign_cc `build_script.sh`)
were compared with `--include_file_write_contents`. The Debian rootfs and the
TLC jar are not in boot_test's graph; their own `deps()` (6,920 actions) were
compared the same way.

asan and ubsan have the same shape: 774 keys change directly and 1,010 of
7,880 actions (13%) re-run, including 656 of 1,328 C++ compiles.

| Third-party action | Key changes plain to asan/ubsan? | Cause |
|---|---|---|
| kernel genrule, busybox genrule, bc genrule | no | genrules: command and env only |
| Debian rootfs genrule and its 149 `.deb` genrules | no (0 of 6,920 actions change) | same |
| TLC overrides jar (`TlcOverridesJar`) | no | `run_shell`, JDK and jars only |
| exec-config tools (m4, gawk, make, pkgconf, autoconf probes, exec e2fsprogs/libarchive for the rootfs) | no | `--copt`/`--linkopt` do not apply to the exec configuration (`--host_copt` would) |
| QEMU (`configure_make`) | yes | `--copt` is written into `CFLAGS`/`CXXFLAGS`/`ASFLAGS` and `--linkopt` into `LDFLAGS` of `build_script.sh`: asan adds `-fsanitize=address -DADDRESS_SANITIZER -O1 -g`, ubsan `-fsanitize=undefined -fno-sanitize-recover=all -O1 -g` |
| e2fsprogs, libarchive, xfsprogs, btrfs-progs, util-linux uuid_blkid, urcu (all `configure_make`) | yes | same |
| glib (99 compiles), pcre2 (30), zlib (15) | yes, all of them | the compile command gains the `--copt` flags plus the 6.3 `per_file_copt` flags (`-fno-sanitize=address -UADDRESS_SANITIZER`) |
| abseil (437 compiles, 686 actions with archives and links), libfuse 17, google_benchmark 19, numactl 6, liburing 5, sqlite3 1, inih 1, pjdfstest, our code (dcfs 19, bench 4, tools 2) | yes | `--copt`; these must keep matching dcfs's own sanitizer settings (the SwissTable note in `.bazelrc`) |

Consequences, each read off the data above:

- The 6.3 `per_file_copt` fix stops glib/pcre2/zlib from being
  *instrumented*, not from being *rebuilt*: their 144 compiles re-run per
  sanitizer config (24 s CPU in total, cheap). The expensive action is the
  QEMU `configure_make` itself (517 s, 1.6): its script embeds `--copt`, and
  BUILD.qemu's `--extra-cflags=-fno-sanitize=address,undefined` neutralises
  the effect on the binary but not on the action key. The same holds for
  the five other foreign_cc builds.
- Per sanitizer config, on a cold cache, the avoidable third-party work is
  all foreign_cc: QEMU 517 s, e2fsprogs+libarchive 123 s, xfsprogs 70 s,
  btrfs-progs 64 s, util-linux 63 s, urcu 53 s = about 890 s (15 min), plus
  24 s of glib/pcre2/zlib. None of it depends on the sanitizer.
- plain, asan and ubsan all write to `bazel-out/k8-fastbuild/...` (aquery:
  1,180 of the 7,880 actions in boot_test's graph have a `k8-fastbuild`
  output in each, the other 6,700 are `k8-opt-exec`; same paths), so an
  output base holds one of them at a time: switching config in a lane
  re-runs those 1,010 actions or fetches them from the disk cache. The disk
  cache is what makes switching cheap, and lanes share it.
- e2fsprogs is built twice per cold build: in the target configuration
  (the scratch-disk mkfs in boot_test's data) and in the exec configuration
  (`//third_party/e2fsprogs:mke2fs` as a tool of the Debian rootfs genrule):
  135 s for the exec copy (libarchive 82 s, e2fsprogs 53 s) next to 123 s
  for the target copy.
- A guest script change reruns that test's initramfs genrule and test only.
  A BUILD edit reruns only the targets whose command lines change but costs
  a full analysis (the 100+ s above; `--per_file_copt` changes discard the
  analysis cache).

### 1.6 Cold third-party builds (measured)

Method: `--disk_cache=` empty, repository cache intact (every archive and
`.deb` already fetched), `--jobs=2`, `--profile` on, one target per
invocation, lane-4 checkout. To force exactly one target and its
target-configuration dependencies to re-execute without rebuilding the
exec-configuration toolchains, each run passes
`--action_env=DCFS_BUST=<n> --host_action_env=DCFS_HOST=0` with a fresh `n`.
Checked first with aquery: changing only `DCFS_BUST` changes 165 of 7,000
actions (the 7 foreign_cc actions plus glib/pcre2/zlib/inih compiles) and no
exec action; genrules use the default shell env so they are covered too.
`TlcOverridesJar` is not (it ignores `--action_env`), so its output was deleted
instead. 1-minute load average before each run: 3 to 6.

| Target | Wall (s) | Dominant action (profile) | Notes |
|---|---|---|---|
| exec bootstrap: m4, gawk, make, pkgconf, 2,006 autoconf probes | about 133 | 3,157 actions, 1,540 sandboxed | once per output base and exec config; measured as 186 s minus urcu's 53 s |
| urcu | 59 | `configure_make` 53 | |
| QEMU (+ glib, pcre2, zlib) | 545 | `configure_make qemu` 517; 144 dep compiles 24 s CPU | critical path 518 s: the QEMU build is one serial action |
| e2fsprogs + libarchive | 129 | libarchive 73, e2fsprogs 50 | the two run in sequence |
| xfsprogs (+ urcu, util-linux uuid, inih) | 138 | xfsprogs 70, uuid_blkid 64, urcu 60 | uuid and urcu overlap at 2 jobs |
| btrfs-progs (+ util-linux uuid, zlib) | 128 | btrfs 64, uuid_blkid 59 | |
| util-linux uuid_blkid | 67 | 63 | |
| busybox genrule | 34 | 32 | |
| bc genrule | 8 | 6 | 9 s each for the target and the exec copy when built inside the kernel run |
| kernel genrule, run 1 | 731 | 705 | load average 70-85, swap thrashing (`--config=asan`, irrelevant to its key) |
| kernel genrule, run 2 | 694 | 690 | load 5-8; includes the exec bc (9 s) |
| Debian image, exec tools cold | 154 | exec libarchive 82 + exec e2fsprogs 53, 149 `.deb` extractions 36.5 s CPU, flatten 6.5, rootfs 10.2 | |
| Debian image, exec tools cached | 29 | 150 extractions 39.9 s CPU, rootfs genrule 8.6 | fetching the `.deb` files is excluded (repository cache) |
| TLC overrides jar | 5 | javac + jar 1.4 | |

Summed (action time): kernel 690 s, QEMU 518 s, everything else about 740 s
(bootstrap 133, e2fsprogs 123, exec e2fsprogs 135, xfsprogs 70, btrfs 64,
util-linux 63, urcu 53, Debian about 55, busybox 32, bc 12, TLC 1): about
1,950 s, 32 minutes of work in a cold lane with `--disk_cache=` off. The
kernel and QEMU are 62% of it. The kernel is the largest config-independent
cost (the disk cache already covers it between lanes); QEMU is the largest
cost that sanitizer configs repeat for no reason (1.5).

The kernel took 11.5 to 12 minutes in both runs although the load average
differed by a factor of ten (the first run's load was mostly I/O wait from
swapping, the second shared the CPUs with one other lane's Bazel server).
`build_kernel.sh` runs `nice -n 19 make -j4` whatever `--jobs` says. I did
not isolate why the two runs are so close; the 9 min (3a) and 28-40 min
(5.1, 5.2) in 1.4 are other points on the same contention curve.

## 2. What others do

- **Prebuilt, hash-pinned, from the project's own storage** is the dominant
  pattern for guest kernels and root filesystems. crosvm's e2e tests
  download a prebuilt guest kernel and rootfs from Google Cloud Storage and
  bump a `PREBUILT_VERSION` file when they change
  ([crosvm e2e_tests README](https://android.googlesource.com/platform/external/crosvm/+/44e40e1560890dbf34a731e19bab24b7f2c72b8a/e2e_tests/README.md)).
  Fuchsia fetches host tools, including a QEMU built by its own bots, as
  CIPD packages addressed by hash
  ([prebuilt CIPD packages](https://fuchsia.dev/docs/development/prebuilt_packages/prebuilt_cipd_packages_in_fuchsia),
  [buildtools hashes](https://fuchsia.googlesource.com/buildtools/+/73666bcc32a690c0109e929129dfb5595a5998b0)).
  KernelCI builds kernels in published Docker images and uploads kernels,
  rootfs and logs to a storage server
  ([kci build](https://docs.kernelci.org/legacy/core/kci_build)).
  syzkaller's documented path builds the kernel on the host and makes a
  Debian image with debootstrap, i.e. distro packages for the userspace and
  QEMU
  ([syzkaller setup](https://github.com/google/syzkaller/blob/master/docs/linux/setup_ubuntu-host_qemu-vm_x86-64-kernel.md)).
  From memory, not fetched: Firecracker's CI pulls prebuilt guest kernels
  and rootfs images from S3; gVisor uses a distro QEMU or the host KVM and
  its own prebuilt images; nobody I know of builds QEMU inside the test
  build.
- **Bazel projects with expensive third-party builds** use a shared remote
  cache rather than rebuilding: Bazel's remote caching treats the cache as
  an opaque HTTP/gRPC store and any such server works
  ([Bazel docs](https://bazel.build/remote/caching)); bazel-remote and
  BuildBuddy are the common servers
  ([BuildBuddy](https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained)).
  CI configs use `--remote_download_minimal` (download only what the build
  needs; ~3x faster in one comparison) and
  `--remote_upload_local_results` only from CI
  ([Bazel blog](https://blog.bazel.build/2019/05/07/builds-without-bytes.html)).
  GitHub's Actions cache is LRU per repository with a 10 GB default
  (raised to pay-as-you-go in late 2025)
  ([changelog](https://github.blog/changelog/2025-11-20-github-actions-cache-size-can-now-exceed-10-gb-per-repository/)).
  Our local disk cache is already 29 GB, so a GitHub cache would hold only a
  subset.
- **Nix/Guix** pin the whole closure and fetch from a binary cache
  (cache.nixos.org), compiling only on a cache miss: the same idea as
  "prebuilt keyed by input hash, build as fallback", with the pinning
  mechanism changed.

## 3. Options for us, ranked by saving per unit cost

| # | Option | Saves | Cost | Gives up |
|---|---|---|---|---|
| 1 | (c) Config-independent tool builds for QEMU, e2fsprogs, xfsprogs, btrfs-progs, util-linux, urcu (the six foreign_cc targets; glib/pcre2/zlib only matter through QEMU, busybox and the kernel already are config-independent, 1.5) | Measured: ~890 s (15 min) per sanitizer config on a cold cache (QEMU 517, e2fsprogs+libarchive 123, xfsprogs 70, btrfs 64, util-linux 63, urcu 53) plus 24 s of glib/pcre2/zlib; nothing on plain. Building e2fsprogs once instead of twice (target and exec copy) saves another 123-135 s on every cold build, plain included | Small: either an alias rule with `cfg = "exec"` (the pattern `//third_party/e2fsprogs:mke2fs` already follows for the Debian rootfs, whose exec copy is already independent of the sanitizer flags) or a transition that fixes `copt`/`linkopt`; 1 step (the 6.3 follow-up); verify with the aquery key comparison used in 1.5 (script in this session's scratch, easy to recreate) | Exec-configuration builds use the exec toolchain flags (`-O2` rather than fastbuild): re-check QEMU's smoke test and the guests' behaviour |
| 2 | (b) Shared remote cache: bazel-remote on this machine (lanes already share a disk cache, so this mainly serves CI/act) or on a small VPS | Everything for repeat builds in a fresh clone, act, and any second machine: cold act job 1h04 to roughly warm 3-8 min | Medium: a daemon, auth, `--remote_cache` in `ci.bazelrc`; upload only from trusted CI | A server to run; a poisoned cache poisons results (mitigate: upload from CI only, read-only elsewhere) |
| 3 | (a) Prebuilt-by-us artifacts: CI builds kernel, QEMU, busybox, e2fsprogs etc., publishes to release assets or an OCI registry, repo records sha256 | Measured cold (1.6): kernel 11.5 min (9-40 in the logs), QEMU 8.6 min, the other tools about 12 min together (bootstrap, e2fsprogs twice, xfsprogs, btrfs, util-linux, urcu, busybox, Debian), per cold machine, independent of Bazel cache state; about 32 min in all | High: build-and-publish workflow, hash bump after each source or flag change (a bot commit), a Bazel switch (`select` between prebuilt `http_file` and source build) with source fallback; reproducibility only partly demonstrated (kernel, busybox, rootfs stamps are reproducible; QEMU, e2fsprogs are not verified) | A second path to maintain; the source build can rot unless a CI job still builds it |
| 4 | (e) Cheaper builds: kernel `-j` taken from Bazel's resources instead of hardcoded 4 with `nice 19`; drop unneeded kernel config; ccache for kernel (helps only config changes); `-O0` for third-party in test configs | Small to moderate; no help on cold-cache wall time for QEMU. `-O0` for abseil/sqlite changes test behaviour and ASan ABI (see the SwissTable note in `.bazelrc`) so do not | Low | Possible behaviour drift |
| 5 | (f) Right-size parallelism per lane | No speed-up, but avoids the OOM kills and the 2-3x inflated timings seen today: three lanes at `--jobs=2` plus kernel `-j4` plus QEMU guests on 4 cores/12 GB | Low: lane count and `local_resources`, or one shared Bazel server | Fewer lanes in parallel |
| 6 | (d) Distro QEMU for local development | ~8.6 min of QEMU once per config (cheaper through option 1) | Low | Pinning, the minimal device set, qboot: guests would differ from CI. Not recommended |
| 6b | (h) Distro guest kernel (Alpine `linux-virt`, Debian) instead of ours | Measured: no boot-time gain (last section: KVM 1.9-2.1 s to init for ours and Alpine alike); saves only the 11.5-min kernel build per cold machine, which a hash-pinned prebuilt of our own 3.6 MB bzImage saves as well | Medium: initramfs module loading, `uname -r` check, dm-dust missing in both distros, Debian needs ACPI on and hangs under TCG 4 times in 10 (cloud flavour), Alpine's pin does not last (the last section) | The tinyconfig's pinned, minimal, latest-stable kernel; the version matrix still needs a kernel we build. Not recommended for build speed |
| 7 | (g) Nix/Guix as the pinning mechanism | Same as (a) via a binary cache, plus no source builds | Very high: second build system next to Bazel, sandboxing differs, russ must adopt it | Bazel-only simplicity |

Split of the Debian image build, measured: 150 `.deb` extractions 39.9 s CPU
and the rootfs genrule 8.6 s (29 s wall) once the exec mke2fs exists; not worth
splitting. The 154 s first run is the exec e2fsprogs (option 1).

## 4. Recommendation and decisions (revised after the second pass)

1. Do (c) now, test-first, as one step: make the six foreign_cc builds
   independent of `--copt`/`--linkopt` and build e2fsprogs once, then check
   with the aquery key comparison (1.5) that all QEMU, e2fsprogs, xfsprogs,
   btrfs-progs, util-linux and urcu action keys are equal under plain, asan
   and ubsan, and that `//third_party/e2fsprogs:mke2fs` has one copy. Measured
   gain: ~890 s per sanitizer config on a cold cache, 123-135 s per cold
   build of any config. The cheapest option and the only one that changes
   what runs.
2. Add (b) for CI and act: read-write only from the pinned CI job,
   read-only for lanes, `--remote_download_minimal`. Start with
   bazel-remote on this machine since act runs here. The cold build is now
   known to be about 32 minutes of action time in a lane (1.6; 1h04 in act
   with its own overheads), of which the kernel (11.5 min) and QEMU (8.6 min)
   are 62%: a cache turns all of it into a one-time cost per pin change.
3. Add (a) only if cold builds on fresh machines remain a pain after 1-2,
   and for the kernel only: it is the largest config-independent action
   (690 s), reproducible, and 3.6 MB to host. QEMU stops being worth
   prebuilding once (c) removes the per-config repeats.
4. Do not replace our kernel with a distribution kernel to speed up builds
   or boots (last section): boot time is the same, two required options are
   missing or absent, the harness needs module loading and (Debian) ACPI,
   and Alpine's pin does not outlive the next package update. If a
   second-kernel matrix entry is wanted later, Debian's cloud kernel from
   snapshot.debian.org is the only durable pin, and it needs the hang under
   TCG understood first.
5. The busybox re-run is explained as far as the keys go (1.3): no flag
   in `.bazelrc` can re-run it; a lane's first build or an empty disk cache
   can. Keep `--disk_cache` on in lanes; use `--disk_cache=` only for
   deliberate cold measurements, and expect every genrule (kernel 11.5 min,
   busybox 32 s) to run once in a fresh output base.
6. Skip (d) and (g).

Russ must decide: (1) where prebuilts or the remote cache are hosted (this
machine, a VPS, GitHub releases/GHCR) and who pays; (2) the trust model:
hashes in the repo make a prebuilt exactly as trusted as the source pin,
whereas a remote cache is trusted only if uploads are restricted to CI; (3)
whether the source-build fallback must stay green in CI (cost: one cold job
per pin change); (4) how many lanes the host may run at once (the 1.6 numbers
were taken with one or two other lanes busy; the first kernel run at load
70-85 differed from the second by only 15 s, so the kernel is not the number
that suffers most, but swap use of 6-7 GB shows the host is over-committed);
(5) whether exec-configuration (`-O2`) builds of QEMU and the mkfs tools are
acceptable in place of fastbuild ones.

## Distribution guest kernels (measured 2026-10-06)

Question (russ): could a prebuilt, distribution-maintained kernel replace our
3.6 MB tinyconfig build? Method: fetched Alpine's current `linux-virt` apk
(branch v3.24, `APKINDEX` of `latest-stable`) and Debian 13's
`linux-image-amd64` and `linux-image-cloud-amd64` from snapshot.debian.org
(debian-security `20261006T081244Z`, the archive that carries the current
kernel; the main-archive snapshot the test image pins, `20261006T082722Z`,
has the older 6.12.107). Extracted the kernel and config, compared every
option we set, built modules-only initramfs extensions by hand
(the existing `bazel-bin/test/qemu/initramfs.cpio.gz` with a second cpio
concatenated after it: modules decompressed to plain `.ko`, a `/init` that
loads them in dependency order and then execs the original init, a patched
copy of `guest/boot.sh` with only the `uname -r` check relaxed), and booted
each with `run-qemu.sh` itself, unmodified, through a wrapper that execs the
pinned QEMU 11.1.2 and qboot from lane-4's `bazel-bin` (found in
`boot_test.runfiles_manifest`; the wrapper can rewrite `acpi=off` to `acpi=on`
for the Debian runs). Test: boot_test (`boot.sh`, `vdb:ext4:64M`). Times are
seconds from the start of `run-qemu.sh`, which includes the mkfs of the
scratch disk and QEMU's start, measured by timestamping the serial output;
"init" is the guest's first line, "PASS" is `ALL-TESTS-PASSED`. 1-minute load
average during these runs: 5 to 13.

### Sizes

| | ours | Alpine `linux-virt` 6.18.55-r0 | Debian 13 `linux-image-amd64` 6.12.111-1 | Debian 13 `-cloud-amd64` 6.12.111-1 |
|---|---|---|---|---|
| Kernel version / line | 7.2.9 (latest stable) | 6.18 LTS | 6.12 LTS | 6.12 LTS |
| Kernel image | 3.57 MB bzImage | 12.64 MB (gzip) | 12.15 MB (zstd) | 11.72 MB |
| Download | built, 690 s | 44.5 MB apk | 108.0 MB deb | 34.5 MB deb |
| Modules on disk | none (all `=y`) | 34 MB (`.ko.gz`) | 102 MB (`.ko.xz`) | 24 MB |
| Modules needed by boot_test | none | virtio_blk, crc16, mbcache, jbd2, ext4: 0.69 MB gz | virtio_mmio, virtio_blk, crc32c_generic, crc16, mbcache, jbd2, ext4: 0.70 MB | virtio_mmio, virtio_blk: 0.03 MB |
| Modules for the whole option list (virtio, ext4, btrfs, xfs, quota_v2, fuse, nfsd, nfs, nfsv4, loop, dm-mod/delay/log-writes/flakey, autofs4, with dependencies) | none | 29 modules, 6.0 MB gz (27.7 MB raw) | 33 modules, 5.3 MB gz (24.8 MB raw) | 28 modules, 4.7 MB gz (21.8 MB raw) |
| Initramfs (shared one is 8.30 MB) | 8.30 MB | +1.8 MB for boot_test, +7.1 MB full | +1.8 / +6.4 MB | +1.1 / +5.8 MB |

The initramfs additions include 1.1 MB for a static busybox (the host's), used
only because our busybox build has no `insmod`/`modprobe` applet; with those
applets enabled the payload is the module figures above.

### Option coverage (94 symbols in `third_party/linux/kernel.config`, `LOCALVERSION` excluded)

| | built in (`y`) | module (`m`) | off | symbol absent |
|---|---|---|---|---|
| Alpine | 76 | 17 | 1 | 0 |
| Debian | 73 | 17 | 3 | 1 |
| Debian cloud | 75 | 15 | 3 | 1 |

- Modules in Alpine: VIRTIO_BLK, VIRTIO_NET, EXT4_FS, BTRFS_FS, XFS_FS, QFMT_V2,
  FUSE_FS, NFSD, NFS_FS, NFS_V4, BLK_DEV_LOOP, BLK_DEV_DM, DM_DELAY,
  DM_LOG_WRITES, DM_FLAKEY, AUTOFS_FS, CRYPTO_CRC32C. Debian adds
  VIRTIO_MMIO; its cloud kernel builds EXT4_FS, CRYPTO_CRC32C and FUSE_FS in.
- **DM_DUST is off in all three distribution kernels** (Phase 11's I/O error
  injection with dm-dust cannot run on them).
- FUSE_PASSTHROUGH, UNICODE, FS_ENCRYPTION, POSIX_TIMERS, cgroups, namespaces,
  SECCOMP, QUOTA, IO_URING and the rest of the list are built in on all
  three.
- **FUSE_IO_URING does not exist in Debian's 6.12** (Alpine 6.18: built in).
- Debian only: VIRTIO_MMIO is a module and VIRTIO_MMIO_CMDLINE_DEVICES is off,
  and DEVTMPFS_MOUNT is off (our init mounts devtmpfs itself, so that one is
  harmless).

### What booting required

- Alpine: `run-qemu.sh` unmodified, `acpi=off`; virtio_mmio is built in.
  Needs the initramfs module loader (virtio_blk, then ext4 and its
  dependencies) and nothing else.
- Debian: with `acpi=off` `/dev/vdb` never appears (verified: virtio_mmio
  loaded, no device: no command-line devices compiled in). With `acpi=on` it
  works, after also loading `crc32c_generic`, which `ext4` needs through the
  crypto API but `modules.dep` does not list (the mount failed with "EXT4-fs
  (vdb): Cannot load crc32c driver"). So the harness would need `acpi=on` for
  Debian guests, and then every config in `run-qemu.sh`'s comments about
  ACPI (no ACPI poweroff, kernel start-up with ACPI) applies again.
- `guest/boot.sh`'s `uname -r` must end in `-dcfs-stock`: a distribution
  kernel would fail that check by design; I booted a scratch copy with only
  that check relaxed, and the other nine checks pass on Alpine and on
  Debian (with `acpi=on`). The test in the tree is untouched.

### Boot time (seconds; wall clock from `run-qemu.sh` start)

| Kernel, accelerator | Runs | Init (median, range) | boot_test PASS (median, range) |
|---|---|---|---|
| ours, KVM | 3 | 2.09 (1.91-2.25) | 2.29 (2.03-2.44) |
| Alpine, KVM | 3 | 1.91 (1.84-2.10) | 2.04 (1.96-2.23) |
| Debian, KVM, acpi=on | 3 | 2.65 (2.20-2.92) | 3.10 (2.31-3.17) |
| Debian cloud, KVM, acpi=on | 3 | 1.69 (1.69-1.78) | 1.86 (1.80-1.92) |
| ours, TCG | 3 | 6.84 (6.54-6.96) | 7.60 (7.29-7.87) |
| Alpine, TCG | 7 of 7 pass | 7.51 (6.40-9.05) | 8.39 (7.37-10.45) |
| Debian, TCG, acpi=on | 7 of 7 pass | 10.09 (8.99-10.78) | 11.16 (9.74-11.84) |
| Debian cloud, TCG, acpi=on | 6 of 10 pass, **4 hang** | 8.76 (7.77-10.76; 4 runs) | 9.83 (8.73-12.02; 4 runs) |

KVM: the distribution kernels boot as fast as ours (differences of a few
tenths of a second, inside the load noise), because the time to init is
dominated by QEMU's start, qboot and the initramfs unpack, not by the kernel
image. TCG: Alpine is the same as ours within noise (the later Alpine runs
were slower because the host was busier), Debian's full kernel about 3 s
slower. The cloud flavour hangs under TCG in 4 of 10 runs, always at the
point `run-qemu.sh`'s comments describe ("tsc: Unable to calibrate against
PIT / No reference (HPET/PMTIMER) available / Marking TSC unstable", then
silence until the timeout); our kernel, Alpine and Debian's full kernel did
not hang in 3, 7 and 7 runs.

### Pin durability

- Alpine keeps one version of each package in a branch. The `v3.24`
  `linux-virt` was 6.18.55-r0 (built 2026-10-05); the aports log shows
  upgrades on 2026-10-05, 09-28, 09-22, 09-15, 09-14, 09-08, 09-07, 08-28,
  08-10, 08-05, 07-31, 07-27, 07-21: 13 versions in 11 weeks.
- Tried the previous versions on dl-cdn.alpinelinux.org: 6.18.54-r0 (a week
  old) answers 200 and downloads in full; 6.18.53-r0, .52, .51, .50, .49, .48,
  .44, .42, .41, .40, .39, .38 and the guessed `-r1` variants are 404. The
  directory listing of `v3.24/main/x86_64/` contains only
  `linux-virt-6.18.55-r0.apk`, and mirrors.edge.kernel.org, uk., dl-2.alpinelinux.org
  and mirror.leaseweb.com all return 404 for 6.18.54-r0: the one old copy is a
  CDN cache remnant, not an archive. A pinned Alpine apk therefore vanishes
  within days of the next kernel update (the branch got 13 updates in 11
  weeks) and we would have to mirror it ourselves (44.5 MB) as a prebuilt
  anyway. `v3.23` currently serves the same 6.18.55; `v3.22` and `v3.21` serve
  6.12.112-r0.
- Debian: durable. snapshot.debian.org serves every file by timestamp and
  hash; the three `.deb` files above were fetched by URL and their sha256
  matched the `Packages` index (amd64 `792e7a40...`, cloud `61e99b7a...`). The
  test image already pins its packages this way.

### Verdict on the criteria

Boot time per guest times about 100 guests: no gain under KVM (Alpine and
ours are within 0.2 s; at 100 guests that is below the noise of a full run)
and none under TCG. What a distribution kernel would save is the 11.5-minute
kernel build per cold machine, which a hash-pinned prebuilt of our own 3.6 MB
kernel saves just as well without giving up dm-dust, a latest-stable kernel
for the version matrix, or `acpi=off`. The
distribution kernels lose on each other criterion: Alpine's pin does not
last, Debian needs ACPI and has no FUSE_IO_URING, the cloud flavour hangs
under TCG 4 times in 10, and two distributions cannot run Phase 11's dm-dust
tests at all. Not recommended; see 4.4.

Not done: Firecracker's published guest configs (not requested in the second
pass), Tiny Core (noted and not recommended, unchanged).

## Follow-up (6.3b, 2026-10-07): config-independent tool builds, done and measured

Change: `//third_party/qemu:qemu_system_x86_64`, `//third_party/e2fsprogs:{mke2fs,debugfs}`,
`//third_party/xfsprogs:{mkfs_xfs,xfs_io}` and `//third_party/btrfs-progs:{mkfs_btrfs,btrfs}`
are now `exec_file` rules (`third_party/exec_file.bzl`): a one-file forwarding rule whose
`src` (the former filegroup over the `configure_make` output group) has `cfg = "exec"`.
Every consumer (the test macros, `formal/trace.bzl`, the smoke tests, the Debian rootfs
genrule's `tools`) kept its label. An explicit transition was not needed: the exec
configuration already ignores `--copt`/`--linkopt`, the tools' own flags (static linking,
`-fno-sanitize`, the pkg-config shims) are untouched, and the graph gained one rule per tool.
`--per_file_copt` and `--define` do not matter either (the key test is green for all three
configs). Consequences found on the way: the exec toolchain is `-c opt` (`-O2 -DNDEBUG`), and
QEMU's `osdep.h` refuses `NDEBUG`; BUILD.qemu now passes `--extra-cflags=-UNDEBUG`. e2fsprogs
and libarchive, which the Debian image already built for exec, are now the same configured
targets the tests use, so they build once.

Tests (both new):

- `//tools:tool_keys_test` (manual; starts its own Bazel server, 335 s): `bazel aquery
  'deps(T)'` action keys of every tool target under plain, `--config=asan` and
  `--config=ubsan`, kernel/busybox/bc/Debian rootfs/TLC jar as the positive control. Failing
  first, on the tree before the change: QEMU 147 of 6,976 actions differ per sanitizer config
  (the `build_script.sh` plus glib/pcre2/zlib compiles), mkfs.btrfs 18 of 6,715, mkfs.xfs 4 of
  6,685, mke2fs and debugfs 2 of 6,680 each; the five controls passed. After: all ten pass.
- `//tools:tool_identity_test` (small, in `//...`): the tool files as built with the
  `--config=asan` flags (a Starlark transition on `--copt`, `--linkopt`, `--per_file_copt`,
  `--strip`, `tools/sanitizer_variant.bzl`) against the plain ones: same sha256 and the same
  file. Failing first: all five differ (different binaries, different `bazel-out` directories);
  after: all five are `bazel-out/k8-opt-exec/...` and identical.

Cold-cache measurement (`--disk_cache=` empty, repository cache intact, `--jobs=2`, lane-4;
`--host_action_env=DCFS_HOST=cold1` forces the exec actions, bootstrap included, to run;
load average 5 at the start and 12-14 at the end because another lane's Bazel and guests
were running):

| Build | Wall (s) | Actions executed |
|---|---|---|
| `--config=asan`, the five tool targets (QEMU, mke2fs, debugfs, mkfs.xfs, mkfs.btrfs, with glib, pcre2, zlib, libarchive, util-linux uuid/blkid, urcu, inih and the exec bootstrap) | 1,090 (critical path 689: the QEMU `configure_make`, 658 s under load against 517 s unloaded) | 3,617 |
| then `--config=ubsan`, same targets | 19 | 0 |
| then plain, same targets | 10 | 0 |

So a cold asan or ubsan lane, which used to build the six tools again (about 890 s of
`configure_make` plus 24 s of glib/pcre2/zlib on top of plain's own copy), now builds
nothing: one build (about 890 s of tools plus the 133 s exec bootstrap, roughly the same
1,090 s measured under load) serves all three configurations. Saving per sanitizer
configuration on a cold cache: about 890-915 s (the note's estimate, confirmed by the 0-action
ubsan and plain rebuilds; the "before" is the section 1.6 numbers, not re-measured). Saving
for the plain build too: e2fsprogs plus libarchive (123-135 s) is built once instead of twice.
Not measured again: the whole-suite `--config=asan` wall time, which is dominated by the
kernel and the tests. Presubmit (`--config=presubmit //...`) 129 of 129 pass; `--config=asan
//test/qemu:boot_test //test/qemu:write_test_ext4 //tools:tool_identity_test` pass.

Open item: the guests now run the exec build of QEMU (`-O2 -DNDEBUG`, `-UNDEBUG` for QEMU
itself) and the mkfs tools (`-O2 -DNDEBUG`, so their `assert`s are off, as in the exec e2fsprogs
the Debian image already used); the smoke tests and every guest test passed unchanged.
