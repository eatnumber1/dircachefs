# Phase 3 — Drop the kernel patch; download and build the test kernel

**Decision (russ, 2026-10-04).** The kernel patch (generation in
GETATTR/SETATTR replies, FUSE_ATTR_GENERATION, protocol 7.47) gives dcfs
nothing: dcfs never changes a nodeid's generation, and after Phase 14 (Identity from the backing filesystem) the
kernel's existing LOOKUP-time check rejects stale handles. Drop it entirely so
dcfs runs on a stock kernel and stock libfuse.
- Tests first: a guest test that boots a **stock** kernel (no patch) and runs
  the suite fails today (dcfs requests FUSE_CAP_ATTR_GENERATION / uses
  fuse_reply_attr_with_generation, and the libfuse build needs the patch);
  record the failure.
- Remove `third_party/libfuse/0001-attr-generation.patch`, the
  `single_version_override`, `FUSE_CAP_ATTR_GENERATION` use in Init,
  `FuseRequest::ReplyAttr` with generation (use `fuse_reply_attr`),
  `libfuse_version_test`'s ATTR_GENERATION assertions, and every doc mention
  (README requirements, design.md, conformance.md, test/qemu/README.md).
- Test kernel, downloaded and built (needed by CI, Phase 5 (CI runs the QEMU suite)):
  - Source: a pinned upstream release tarball from cdn.kernel.org (>= 6.9
    for FUSE passthrough and FS_IOC_GETFSUUID; >= 6.8 for
    STATX_MNT_ID_UNIQUE), fetched by Bazel (`http_archive` with sha256).
    `~/Sources/linux` and `build-kernel.sh` are no longer used.
  - Built by Bazel: a rule (ours, or `rules_foreign_cc`'s `make`) runs the
    kernel build with the pinned LLVM toolchain (`make LLVM=1`, from
    Phase 7; until then the host compiler), with flex, bison, elfutils and
    zlib from the BCR and GNU bc fetched and built from source (not in the
    BCR). Output: the `bzImage` other targets depend on. Bazel's caches
    replace `~/.cache/dcfs/kernel-build`.
  - Config: truly minimal. Today's starts from `x86_64_defconfig` +
    `kvm_guest.config` (the script quotes 30-90 minute builds, and it is a
    suspect for slow TCG boots). Instead: `make tinyconfig` plus a
    checked-in fragment (`third_party/linux/kernel.config`) listing every symbol
    the tests need, each with a comment saying which test needs it: microvm
    and virtio-blk, serial console, initramfs, ext4/xfs/btrfs, FUSE with
    passthrough, NFS server and client, overlay of namespaces/mount API,
    device-mapper with `delay`, `log-writes`, `flakey`, `dust`, `error`,
    and what the systemd guest needs (systemd's README lists the required
    options: cgroup2, autofs, inotify, signalfd/timerfd/epoll, fhandle,
    tmpfs ACLs, devtmpfs, ...). The build fails if `olddefconfig` drops
    any symbol from the fragment (checked by comparing the final .config).
  - Changing the version or the fragment rebuilds through ordinary Bazel
    dependency tracking; CI reuses builds through Bazel's disk cache.
    Today's `kernel.bzl` repository rule (which symlinks an out-of-tree
    build) is removed.
  - Test first: a guest test that checks `/proc/config.gz` (or
    `uname -r`) shows the pinned version and fragment.
- Do NOT modify `~/Sources/linux`, `~/Sources/libfuse` or
  `~/Sources/fuse-generation-qemu`.
- Done when: the full suite passes on the stock kernel with stock libfuse on
  ext4/xfs/btrfs.
Order: third, with Phase 4 (both change how guests are built).

Status: 3a done (stock kernel 7.2.9 fetched and built by Bazel,
selectable with `--//test/qemu:kernel`). 3b done 2026-10-05: test-first run
of readonly/setattr_ext4/handles/lifecycle against the stock kernel on
unmodified code already passed (the patched libfuse degrades to stock
behavior whenever the kernel doesn't grant `FUSE_CAP_ATTR_GENERATION`, so
swapping only the kernel flag could never show the expected failure;
removing the libfuse override is what actually exercises the dependency).
Removed `third_party/libfuse/0001-attr-generation.patch` and its
directory, the `single_version_override` in `MODULE.bazel`,
`FUSE_CAP_ATTR_GENERATION` from `DirCacheFS::Init`, the generation-carrying
`FuseRequest::ReplyAttr` overload (now plain `fuse_reply_attr`), and
`dcfs/libfuse_version_test.cc` (it existed solely to assert the patch was
applied; nothing meaningful remained to check). `test/qemu:kernel`'s
default flipped from `patched` to `stock`; `qemu_cc_test.bzl` (unit tests)
now also respects the flag, where before it always hardcoded the patched
kernel. `patched` kept, deprecated, until Phase 4 removes
`build-kernel.sh`/`@kernel_image` entirely. Full suite (`bazel test
//...`) 70/70 on the stock kernel with no test rewrites needed;
`--config=asan` 70/70 (one `rename_test_xfs` flake on the first ASan run,
a pre-existing "zero backing reads" timing check unrelated to this
change -- passed on retry, and on a clean full-suite ASan rerun).
