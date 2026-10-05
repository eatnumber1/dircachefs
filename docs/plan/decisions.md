# Decision record (2026-10-04 to 2026-10-05)

Why the plan in `phases/` looks the way it does: what was decided, what
was considered and rejected, and why. Earlier decisions (the original plan
and amendments 1-20) are in `history.md`. "russ" is the maintainer.

## Identity and the kernel patch

- **The FUSE_ATTR_GENERATION kernel patch is dropped** (Phase 3). dcfs
  never changes a nodeid's generation, so generations in attribute
  replies give it nothing. The kernel already rejects stale NFS handles:
  `fuse_get_dentry` (fs/fuse/inode.c) sends `LOOKUP(nodeid, ".")` and
  compares the reply's generation with the handle's, returning ESTALE on
  a mismatch.
- **Identity comes from the backing filesystem** (Phase 14): nodeid =
  backing inode number, generation decoded from the backing file handle,
  as amir73il's libfuse_passthrough does (see `notes/libfuse-passthrough.md`).
  NFS handles then survive a cache wipe. Only ext4, xfs and btrfs handle
  formats are supported; anything else is refused at startup. The cache
  schema may change freely (never in production).
- **Rewriting dcfs on top of libfuse_passthrough: rejected.** It is
  passthrough-first and path-based, touches the disk on restart, is
  multi-threaded, and supports only ext4/xfs.

## Connected backing fds (Phase 13)

- Objects with a current name are opened through their parent directory,
  so the kernel knows their path (diagnostics via `/proc/<pid>/fd`,
  path-based security tools, audit). Cheap, so done even though the
  original motivation (backing-side fanotify watches) is moot: dcfs has
  exclusive access, so nothing watches the backing filesystem.
- Tests check the property (real paths in `/proc/*/fd`), not watchers,
  which would test an unsupported setup.

## Submounts and btrfs subvolumes (Phase 15)

- **One dcfs process per backing filesystem** (russ): one filesystem's
  cache can be wiped or restarted alone. Rejected: one process serving
  every filesystem (simpler lifecycle, but no independent restarts).
- **Boundaries are stubs**: a visible directory usable as a mount point;
  ENOTSUP inside; EXDEV only for rename/link across (what the backing
  filesystems return). EXDEV inside was rejected: `ls` would print
  "Invalid cross-device link" and `mv` would try to copy.
- **Stubs use synthetic nodeids** (at or above 2^63) in every form: a
  btrfs subvolume entry has no inode of its own in the parent, and a live
  submount's covered directory is unreachable. Cost accepted: NFS handles
  to a stub go stale after a cache wipe.
- **Losing a parent mount takes its children with it**: acceptable.
- **Rejected:**
  - Keeping `/dev/fuse` open across restarts (systemd fd store): the new
    daemon would have to reconstruct kernel state (lookup counts, open
    handles) it never saw. Too complicated.
  - Re-attaching children with `move_mount` after a parent restart.
  - A non-recursive `open_tree` clone as every instance's backing root:
    it hides submounts but not subvolumes, breaking the symmetry.
  - A child asking its parent instance over a unix socket for an fd of
    the covered tree (`SCM_RIGHTS`): works without hacks
    (`may_copy_tree` allows cloning a detached mount from the same mount
    namespace), but too complicated.
  - A shared "parked" view of the backing tree under `/run/dcfs`.
  - Stacked fstab lines (native filesystem, then dcfs over it):
    systemd's fstab generator rejects two lines with one mount point.
  - Detecting overlapping instances: the administrator's responsibility.
  - A recursive bind form: it cannot serve submounts' contents anyway.

## The `mount.dcfs` wrapper (Phase 15)

- dcfs is its own mount helper; fstab type `dcfs`; the backing kind is
  `dcfs.fstype=` (absent: autodetect; a native type; `bind`; `none`).
  Chosen over dotted types (`dcfs.ext4`): no reliance on `mount(8)`'s
  dotted-type fallback, and one `fsck.dcfs` helper instead of per-type
  symlinks.
- Options are not shared: `dcfs.` options go to dcfs, the rest to the
  native mount (`ro` vs `dcfs.ro`). Sharing would need special cases.
- Native mounts run in a throwaway mount namespace and dcfs keeps a
  detached clone, so the backing never appears in the caller's namespace
  and nothing leaks on a crash. Running the native `mount` (rather than
  `fsopen` ourselves) supports NFS, CIFS and FUSE helpers with no
  per-type code and leaves flag handling to `mount(8)`.
- `bind` captures non-recursively and records submounts from mountinfo
  as stubs (so dcfs does not keep their filesystems busy); `none` serves
  the live tree (the administrator owns and can remount it). File mount
  points are rejected.
- Remount changes only the dcfs mount.
- Own daemonization, not `fuse_daemonize` (which always exits 0): fork
  first, report readiness or the real error through a pipe. Not
  `sd_notify`: mount units do not use it.
- Logging: one path, syslog (journald attributes it to the mount unit);
  foreground adds `LOG_PERROR`. Rejected: keeping stderr when it is a
  journal stream (`$JOURNAL_STREAM`): two code paths.
- `allow_other` on by default (a dcfs mount replaces a native mount that
  every user can use).
- Network backings are allowed in principle (the administrator promises
  exclusive access) but need an identity scheme first: NFS handles carry
  no generation (future work).

## Testing

- Tests that need root or control over the kernel run in QEMU (all tests
  of dcfs itself); checks that need neither, such as clang-tidy or a
  model checker, may run on the host (russ, 2026-10-05, refining the
  earlier "all tests in QEMU"). Test first, fakes not mocks. All in
  `/AGENTS.md`.
- clang-tidy with findings as errors (Phase 7).
- googletest for C++ (GUnit was briefly chosen by mistake, then dropped).
- Test tiers are Bazel's native `size`; QEMU tests declare real resources.
- Quality over speed: no optimization may weaken a test.
- Emulation fallback when KVM is absent must be reasonably fast; slowness
  is a bug to investigate (Phase 5).
- Power-loss testing with `dm-log-writes` replay, modelled on xfstests;
  fsstress, fsx and an xfstests subset; I/O error injection, ENOSPC,
  backing shutdown ioctl, fsfreeze; EDQUOT last.
- Coverage close to 100% early, with syscall and SQLite failure sweeps.
- Filename tests from hazard classes plus xfstests generic/453 and 454,
  not every byte value. Names are bytes; WTF-8 rejected (it round-trips
  ill-formed UTF-16, not arbitrary bytes).
- Soak test: manual only, never in CI.
- Fuzzing: not wanted.
- Formal methods (Phase 12): TLA+ with the TLC model checker for the
  write-through protocol, plus trace validation so the model cannot
  silently drift from the code. Chosen over Quint (same ideas, newer,
  smaller community), P (built for message-passing distributed systems),
  Alloy (better at snapshot invariants than crash sequences) and proof
  assistants (prove a re-written copy, months to learn). CBMC and ESBMC
  handle C++20 and Abseil poorly. KLEE was planned as a one-function trial
  and dropped: it supports only LLVM 16 (partially up to 19), and moving
  the whole build to that old a compiler was rejected. Exhaustive
  small-input tests cover pure functions.

## Other

- Unbounded cache by design; cache location is the administrator's choice.
- Subtree escape through guessed NFS handles for subdirectory roots:
  accepted (like knfsd's `no_subtree_check`).
- Case-insensitive and encrypted directories: rejected when detection is
  free (it is), no support.
- Pinned everything: LLVM toolchain, kernel, QEMU, busybox, Debian image,
  pandoc. Bazel downloads, builds and includes every third-party
  dependency (BCR first, else `http_archive` with sha256 and our rules);
  third-party source is not checked in unless unavoidable.
  `third_party/<name>/` holds only overlays, rules, patches and config.
  The Debian image comes from `rules_distroless` and a snapshot lock file
  instead of mmdebstrap; the kernel and QEMU are Bazel-built, replacing
  the out-of-tree builds.
- CI: GitHub Actions on GitHub-hosted runners.
- License: MIT (`LICENSE`, the text downloaded from GitHub's
  choosealicense.com and checked word for word against SPDX's).
- `-Weverything -Werror` for our code.
- No installer; README installation section and a generated man page.
- Memory per referenced inode is bounded by the kernel's inode cache
  (FORGET); measured, not changed.
