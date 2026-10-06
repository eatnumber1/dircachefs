# Review fixes (from audits/review-2026-10-06-waves-1-2.md)

Fix steps for the 2026-10-06 review of waves 1-2. Finding ids (M1, L3,
...) refer to that report, which has the evidence and suggested fixes.
Every item is test first. Commit subjects are prefixed `R1:`, `R2:`, `R3:`.

## R1 — Cache database permissions check (replaces M1)

**Decision (russ, 2026-10-06):** the hostile-cache-directory hardening
(M1: trusting the directory, link counts, `SQLITE_OPEN_NOFOLLOW`,
`-journal`) is not pursued. Instead, one startup check, and done:
- At startup dcfs verifies that the cache database's owner, group and
  mode grant no access beyond what the backing root directory's owner,
  group and mode grant (orchestrator's reading of "matches"; russ to
  correct if exact equality was meant): the database's owner is root or
  the backing root's owner; group read/write only if the group equals the
  backing root's group and that directory grants the group read/write
  respectively; other read/write only if the directory grants others the
  same. The same for existing `-wal`/`-shm` files. A violation refuses to
  start with an error naming both sets of permissions. Creation stays
  0600 (Phase 2), which always passes.
- L7: test gaps in `cache_permissions.sh` (a pre-existing `-shm`,
  symlinked or foreign-owned `-wal`/`-shm`, a non-regular file at the
  database path).
- L6: README and docs/design.md describe the Phase 2 behavior and this
  check.

## R2 — Build and test infrastructure

- M2: QEMU's `--extra-cflags`/`--extra-ldflags` cancel flags as one word
  (`-fno-sanitize=address,undefined`), plus a check (no libubsan/libasan
  in the QEMU binary under the sanitizer configs).
- M3: `CONFIG_POSIX_TIMERS=y` (and remove `bench/clock_shim.cc`, a workaround
  Phase 10 added for the missing timers; russ: fix the kernel, do not work
  around it); audit every EXPERT-gated option tinyconfig
  turns off that tests, the Debian chroot or systemd need (`KCMP`, `AIO`,
  `SYSVIPC`, `ADVISE_SYSCALLS`, `MEMBARRIER`, `RSEQ`,
  `CROSS_MEMORY_ATTACH`, ...); a guest check that timers work
  (`timeout` actually fires). L8: `CONFIG_XFS_QUOTA`, and enumerate what
  the xfstests subset (Phase 17) will need.
- L1: `build_kernel.sh` runs with `set -e` suspended inside its logged
  block: failures are silent and the fragment check can pass vacuously.
- L2: busybox: a "every fragment symbol survived" check like the
  kernel's, and a smoke test that runs each feature the guests rely on;
  `FEATURE_MOUNT_FLAGS`.
- L3: guest listing comparisons use `stat %U/%G`, which print `UNKNOWN`
  in the guest: use `%u %g` (create, rename, readonly, write, lifecycle,
  nfs scripts). Expect this to expose real mismatches: each is a bug to
  report, test first.
- L4: `boot.sh` fails on a kernel version mismatch; expected version
  comes from the build.
- L12: `rename.sh` fails if thaw fails; consider `drop_caches; quiesce;
  drop_caches`. Also the earlier follow-up: move `quiesce_backing` into
  `lib.sh` and use it before every zero-backing-reads baseline.
- L10/L11 and nits outside `run-qemu.sh`: reproducible build stamps
  (kernel `KBUILD_BUILD_*`, busybox banner, rootfs UUID/hash seed/mtimes,
  explicit `MKE2FS_CONFIG`), `mkrootfs.sh` skeleton directory modes,
  stale comments, `repository_ctx.delete`, the QEMU version compared with
  the pin.

## R3 — Pinned mkfs tools (reopens Phase 4)

- L5: scratch filesystems are made by host `mkfs.ext4/xfs/btrfs` with
  host defaults. Use the Bazel-built `mke2fs` with an explicit config, and
  Bazel-built pinned xfsprogs and btrfs-progs (`third_party/`), so the
  filesystems under test are identical everywhere.
- L9: Debian package pins enforced (`--lockfile_mode=error` or an explicit
  lock), version check covering transitive packages.
- Nits inside `run-qemu.sh` (dead placeholder checks, stale comments).
Runs after Phase 5.1 merges (both edit `run-qemu.sh`).

## R4 — Gaps the TLA+ model found (formal/findings/)

From Phase 12.1 (2026-10-06). Unreachable today (single thread, kernel
directory lock) but real under coroutines; fix now while the model is
fresh, test first (a unit or guest test where one can reach it, else the
model's configuration becomes part of the real model as the test):
Done 2026-10-06 (merged; audits/review-2026-10-06-r4.md). As implemented
(the model showed the first suggestions were insufficient):
- `sync_during_mutation`: `BeginSync` snapshots the fill-guard clock and
  the dirty set before syncfs; `ClearDirty` deletes only snapshot rows
  whose mutation ended before the snapshot (`CanFill(snapshot, id)`).
- `readdirplus_unlocked`: the listing is taken right after the
  completeness check (`ListCached`); "."/".." and per-entry refreshes
  follow.
- `rename_stale_source`: a fill snapshot before resolving; `BeginRename`
  verifies it inside phase 1 and aborts (re-resolve, 3 tries, EAGAIN).
Each finding became a `Bug*` constant with a `known_bugs/` variant.

## R4.2 — Follow-ups from the R4 review (dcfs-protocol, test first)

- (High, coroutines) Writable opens escape the sync snapshot: `keep` is
  built from `open_for_write` at ClearDirty time and Release never
  Touches the guards, so the last close during a syncfs can let
  ClearDirty drop the row before the writes are durable; a writable
  Create joins `open_for_write` only after suspension points. Fix: make
  the last writable Release a guard event (Touch, or run it as a
  Mutation), snapshot `open_for_write` in `SyncSnapshot` as a backstop,
  and have Create call `BeginWriting` right after the insert as Open does.
  Test with the forged-request harness and a `--wrap=syncfs` hook.
- (Medium, coroutines) `RemoveChild` has the rename gap: resolve, then
  `BeginRemove(parent, name, child.id)` with no check; a concurrent rename
  onto the name makes unlinkat remove a different object whose row stays
  present. Fix: `FillSnapshot` before the resolve, `BeginRemove(...,
  resolved)` verifying parent and child, retry; test as RenameStaleSource.
- (Low) The real sync counterexample (a mutation that ends during syncfs)
  is tested only by hand-calling BeginSync/ClearDirty: add a harness test
  with a `--wrap=syncfs` hook running a full MKDIR inside an FSYNCDIR.
- (Low) `ClearDirtyKeepsEverythingPastTheFloor` copies Touch's prune by
  hand: make the prune threshold a `FillGuards` setting so the test
  triggers a real prune.
- (Low, perf) ClearDirty is O(|dirty| x |keep|) with one DELETE per row:
  fast path when nothing moved since BeginSync (seq unchanged, inflight
  empty) back to the bulk delete.
- Record for the coroutine phase (history.md / future work): rename's and
  ListCached's 3-try loops are not real retries without a suspension
  point; the coroutine design needs "wait for the overlapping mutation".
  Also: Readdirplus's per-entry refresh after the listing snapshot fails
  the whole reply if a listed row vanishes (pre-existing shape).
