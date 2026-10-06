# Build speed: where the time goes and what to do (investigation, 2026-10-07)

Status: research and partial measurements. Live measurements stopped at the
orchestrator's request (host out of memory with the S3 suite running): the
items marked "measurements pending" are not done. Nothing in the tree was
changed or committed.

Host: 4 cores, 11.9 GB RAM, `--jobs=2`, `--local_resources=memory=3072`.
Load average was 8-19 throughout (lane-3's Bazel plus a QEMU test ran
alongside), so every wall time below is inflated and CPU times include
contention.

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
we link: they bootstrap m4/gawk/make/pkgconf for rules_foreign_cc. Where m4
and gawk come from was not traced (pending).

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
lower bound. Unexpected: the **busybox genrule re-ran (91 s)** after a pure
`--per_file_copt` change that its inputs should not see. Cause unknown
(measurements pending: aquery action-key diff).

### 1.4 From the logs (not re-measured)

- Kernel: ~9 min cold (3a merge), 28 min under load (5.1), 40 min inside
  act (5.2: the act cold job took 1h04 total). `build_kernel.sh` runs
  `nice -n 19 make -j4` regardless of `--jobs`. bzImage is 3.4 MB.
- QEMU+glib: ~450-500 s cold (6.2 follow-up note); qemu-system-x86_64 is
  11.4 MB.
- Debian rootfs: ~40 s once the packages are fetched; 805 MB sparse ext4.
- Full suite 35-42 min wall; this is test time, not build time.
- Not measured anywhere: e2fsprogs+libarchive, xfsprogs, btrfs-progs,
  util-linux, urcu, busybox, TLC (measurements pending).

### 1.5 What rebuilds when

Measurements pending: aquery under `--config=asan`/`ubsan` and action-key
comparison. From the config and earlier logs: sanitizer configs change
`--copt` for every target-configuration C++ compile, so abseil, sqlite3,
libfuse, googletest, our code and (before 6.3) glib/zlib/pcre2/QEMU all
rebuild; 6.3 stopped glib/zlib/pcre2 for asan via `per_file_copt`, and
QEMU's own CFLAGS were fixed in its BUILD. Kernel and busybox are genrules
with no toolchain dependency, so they should be config-independent but this
is unverified, and 1.3's busybox re-run shows genrule keys are not always
as expected. A guest script change reruns that test's initramfs genrule and
test only. A BUILD edit reruns only the targets whose command lines change
but costs a full analysis (the 100+ s above; `--per_file_copt` changes
discard the analysis cache).

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
| 1 | (c) Config-independent tool builds (exec/transition) for QEMU, glib, e2fsprogs, xfsprogs, btrfs-progs, util-linux, urcu, busybox | ~500 s+ per sanitizer config on a cold cache, nothing on plain | Small: Bazel transition or `--host_copt` style flags, 1 step (the 6.3 follow-up); verify with aquery keys | Nothing |
| 2 | (b) Shared remote cache: bazel-remote on this machine (lanes already share a disk cache, so this mainly serves CI/act) or on a small VPS | Everything for repeat builds in a fresh clone, act, and any second machine: cold act job 1h04 to roughly warm 3-8 min | Medium: a daemon, auth, `--remote_cache` in `ci.bazelrc`; upload only from trusted CI | A server to run; a poisoned cache poisons results (mitigate: upload from CI only, read-only elsewhere) |
| 3 | (a) Prebuilt-by-us artifacts: CI builds kernel, QEMU, busybox, e2fsprogs etc., publishes to release assets or an OCI registry, repo records sha256 | Kernel 9-40 min and QEMU ~8 min and the unmeasured tools, per cold machine, independent of Bazel cache state | High: build-and-publish workflow, hash bump after each source or flag change (a bot commit), a Bazel switch (`select` between prebuilt `http_file` and source build) with source fallback; reproducibility only partly demonstrated (kernel, busybox, rootfs stamps are reproducible; QEMU, e2fsprogs are not verified) | A second path to maintain; the source build can rot unless a CI job still builds it |
| 4 | (e) Cheaper builds: kernel `-j` taken from Bazel's resources instead of hardcoded 4 with `nice 19`; drop unneeded kernel config; ccache for kernel (helps only config changes); `-O0` for third-party in test configs | Small to moderate; no help on cold-cache wall time for QEMU. `-O0` for abseil/sqlite changes test behaviour and ASan ABI (see the SwissTable note in `.bazelrc`) so do not | Low | Possible behaviour drift |
| 5 | (f) Right-size parallelism per lane | No speed-up, but avoids the OOM kills and the 2-3x inflated timings seen today: three lanes at `--jobs=2` plus kernel `-j4` plus QEMU guests on 4 cores/12 GB | Low: lane count and `local_resources`, or one shared Bazel server | Fewer lanes in parallel |
| 6 | (d) Distro QEMU/kernel for local development | ~8 min of QEMU once per config; the kernel still has to be ours (custom config, virtio-mmio, microvm), so most of the cost stays | Low | Pinning, the minimal device set, qboot: guests would differ from CI. Not recommended |
| 7 | (g) Nix/Guix as the pinning mechanism | Same as (a) via a binary cache, plus no source builds | Very high: second build system next to Bazel, sandboxing differs, russ must adopt it | Bazel-only simplicity |

Split of the Debian image build: the 150 `.deb` extractions are fetches, the
rootfs is ~40 s; not worth splitting.

## 4. Recommendation and decisions

1. Do (c) now (one step, test-first with an aquery key comparison across
   plain/asan/ubsan). It is the cheapest and fixes the biggest repeat cost.
2. Add (b) for CI and act: read-write only from the pinned CI job,
   read-only for lanes, `--remote_download_minimal`. Start with
   bazel-remote on this machine since act runs here. This makes the cold
   1h04 a one-time cost per pin change.
3. Add (a) only if cold builds on fresh machines remain a pain after 1-2:
   begin with the kernel (largest, most reproducible), then QEMU.
4. Finish the pending measurements before choosing: the per-config aquery
   comparison, a cold QEMU+tools build with the profile, and the unexplained
   busybox re-run.
5. Skip (d) and (g).

Russ must decide: (1) where prebuilts or the remote cache are hosted (this
machine, a VPS, GitHub releases/GHCR) and who pays; (2) the trust model:
hashes in the repo make a prebuilt exactly as trusted as the source pin,
whereas a remote cache is trusted only if uploads are restricted to CI; (3)
whether the source-build fallback must stay green in CI (cost: one cold job
per pin change); (4) how many lanes the host may run at once.

## Pending measurements added 2026-10-07 (russ's question: a minimal distro kernel?)

Evaluate prebuilt, distribution-maintained guest kernels against our
3.4 MB tinyconfig build, measured, no change until russ has seen the
numbers:
- Alpine `linux-virt` (the VM-specific kernel): version in the current
  branch; which of our required options are built in, modules, or off
  (FUSE_PASSTHROUGH, DM_LOG_WRITES/DELAY/FLAKEY/DUST, XFS, BTRFS, NFSD,
  NFS client, UNICODE/casefold, FS_ENCRYPTION, POSIX_TIMERS, cgroups and
  the rest of third_party/linux/kernel.config); initramfs size with the
  needed modules; boot-to-init time under KVM and TCG; pin durability
  (Alpine mirrors replace updated packages and have no snapshot service:
  can a pinned .apk still be fetched after the package is updated, or do
  we have to mirror it ourselves?).
- Debian 13 `linux-image-amd64` from the snapshot service (durable,
  already how the test image is pinned): the same checks; expected larger
  and slower to boot.
- Firecracker's published microvm kernel configs as a reference for a
  minimal VM configuration (binaries are not a maintained distribution).
- Tiny Core: noted and not recommended (small team, lagging versions,
  mirror rot).
Decision criteria: boot time per guest times about 100 guests per full
run, versus 9-40 min of cold kernel build; the version matrix (minimum
and latest stable) still needs at least one kernel we build.
