# Review of waves 1-2 (2026-10-06, dcfs-reviewer, read-only)

Commits e5df2a7..711b055. Triage and fix steps: `../log.md`.

## Review: what merged to main since e5df2a7 (phases 2, 3.1/3.2, 4.1–4.4, 6.2, 6.3)

**Overall:** I found no critical bugs. The two most significant findings:
- **M2:** `-fno-sanitize=undefined` never reaches QEMU, so `--config=ubsan` still builds an instrumented QEMU.
- **M1:** The phase 2 protection against a hostile cache directory can be bypassed, because the directory itself is never treated as the trust boundary.

The stock kernel also silently lost POSIX timers, which disables `nfs.sh`'s `timeout` guards (M3).

**How I checked:** read-only. I did not run Bazel and edited nothing. Besides reading the code, I looked at outputs already in the lane output bases (the generated `build_script.sh`, `strings` on the built `vmlinux` and busybox, `debugfs` on the rootfs image). I also ran a small dash/bash script in my scratchpad to confirm how the shell treats `set -e`.

### Medium

**M1. Phase 2: the hostile-directory protection can be bypassed.** `dcfs/main.cc:254-276, 130-167, 312-322`
- **The directory is never checked for trust.**
  - The parent check only looks at mode bits and only warns; it never looks at the owner.
  - So a directory an attacker pre-created as 0700 (for example `/tmp/dcfs/`) passes with no warning at all.
  - A group- or world-writable directory only produces a warning.
- **Race between the check and SQLite's open (time-of-check to time-of-use).**
  - `OpenHardenedCacheFile` checks the inode at the path. SQLite then reopens the database by path (`dcfs/sqlite.cc:471`, flags without `SQLITE_OPEN_NOFOLLOW`).
  - SQLite 3.53.4 follows symlinks in the database path before its own `O_NOFOLLOW` open (I confirmed this in its source).
  - So an attacker who can write the directory can swap `dcfs.db` for a symlink or a file they own between line 293 and line 322.
  - The flock stays on the original inode while SQLite uses the substitute. Result: the attacker reads or changes the cache, or dcfs migrates some other root-owned SQLite database.
  - The same applies to `-wal`/`-shm`: they are checked at lines 312-319, but SQLite creates or opens them later, by path. SQLite's own `O_NOFOLLOW` stops symlinks there, but not hard links or files the attacker owns.
- **No `st_nlink == 1` check.**
  - A hard link to a root-owned file passes the regular-file and uid-0 checks. This needs `fs.protected_hardlinks=0`, or read-write access to the target.
  - Line 160 then runs `fchmod` on the real file (e.g. `/etc/passwd` becomes 0600).
  - As `-wal`, SQLite would then write a WAL header into it and truncate it at checkpoint.
- **Suspected, not reproduced:** `dcfs.db-journal` is not checked. SQLite plays back a "hot" journal before it detects WAL mode, so an attacker-owned journal could be rolled into the database.
- **Suggested fix:**
  - Create the directory 0700, open it `O_DIRECTORY`, `fstat` it, and refuse to start unless it is owned by uid 0 and not writable by group or others. Keep only a warning for merely readable.
  - With a trusted directory the per-file checks become race-free. Then add `st_nlink == 1`, pass `SQLITE_OPEN_NOFOLLOW`, and add `-journal` to the list of checked companion files.
  - Tests to add: a 0700 parent owned by uid 1000, a group-writable parent, a hard-linked db or `-wal`, a symlinked `-wal`, and a FIFO at the db path.

**M2. 6.3: `-fno-sanitize=undefined` never reaches QEMU (verified).** `third_party/qemu/BUILD.qemu:145-146`
- rules_foreign_cc joins `configure_options` unquoted (`configure_script.bzl`: `" ".join(user_options)`).
- The generated `build_script.sh` in lane-1's output therefore contains `--extra-cflags=-fno-sanitize=address -fno-sanitize=undefined --extra-ldflags=-fno-sanitize=address -fno-sanitize=undefined`.
- QEMU's configure has no case for a bare `-f…` word and silently ignores it.
- So under `--config=ubsan`, QEMU is still compiled with `-fsanitize=undefined -fno-sanitize-recover=all` and linked with `-fsanitize=undefined`. The emulator is slower, and any UB QEMU hits aborts the guest, which would show up as dcfs test failures.
- ASan is unaffected, because the address flag is the first word.
- **Fix:** use one word each: `--extra-cflags=-fno-sanitize=address,undefined` and the same for `--extra-ldflags`. Add a ubsan check that `readelf -d` shows no libubsan.

**M3. The stock kernel has `CONFIG_POSIX_TIMERS=n`, so `timeout` cannot fire in `nfs.sh` (verified).**
- **Why it is off:** `POSIX_TIMERS` is `bool … if EXPERT`. `tiny-base.config` sets `EXPERT=y`, `allnoconfig` then turns it off, and `third_party/linux/kernel.config` does not turn it back on.
- **Evidence:** the built `vmlinux` has no `posix_timers_cache` slab, while `eventpoll_epi` is present.
- **Effect:** `timer_create`, `alarm` and `setitimer` all return ENOSYS.
  - `test/qemu/guest/nfs.sh:359,563` run GNU `timeout 10 mount -t nfs4` inside the Debian chroot. GNU timeout tries `timer_create`, then falls back to `alarm`; both fail, so it never times out.
  - A hung hard NFS mount (the failure mode documented at `nfs.sh:297`) now waits for `run-qemu.sh`'s 1200 s limit instead of failing after 10 s with `nfs.sh`'s own diagnostics.
  - This is a behavior change from the old defconfig-based kernel, which had the option on.
- **Fix:** add `CONFIG_POSIX_TIMERS=y`. Also audit the other EXPERT-gated options `tinyconfig` turns off and nothing turns back on: `ADVISE_SYSCALLS`, `MEMBARRIER`, `RSEQ`, `KCMP` (systemd lists it), `AIO`, `SYSVIPC`, `CROSS_MEMORY_ATTACH`. Consider adding a timer check to `boot.sh`.

### Low-medium

**L1. `build_kernel.sh`: `set -e` is suspended inside the redirected block (verified with a dash/bash script).** `third_party/linux/build_kernel.sh:49-103`
- The block is `{ … } >"$LOG" 2>&1 || {…}`; when a block is followed by `||`, the shell ignores `-e` inside it. Consequences:
  - The `||` error handler never runs, because the block's status is that of its last `echo`.
  - When the fragment check fails — the case it exists for — `exit 1` (line 88) leaves the whole script while all output is in `$LOG`. The EXIT trap (line 38) then deletes it, so the genrule fails with no message at all.
  - A failing `make olddefconfig` (line 68) does not stop the script. `.config` is then `merge_config`'s raw output, which contains every fragment line, so the check passes without testing anything; `make bzImage`'s own config sync then drops symbols silently.
- **Fix:** run the body in a fresh shell (`sh -eu -c …` or a separate script) with output to the log. Print the log to stderr on failure before the trap deletes it.

**L2. The busybox fragment has no "did every symbol survive" check, and the smoke test checks applets only.** `third_party/busybox/build_busybox.sh:45-55`, `smoke_test.sh`
- The `sed` only rewrites lines that already exist as `# CONFIG_X is not set`. A misspelled or renamed symbol (for example after a busybox version bump) is silently ignored.
- `silentoldconfig` also resets symbols whose dependencies are off, without saying so.
- The smoke test only checks `--list`, so lost features are not caught on the host. Several fail silently in the guest:
  - `dd conv=` exits 1 with no message;
  - `LS_SORTFILES` off changes `ls` order without an error;
  - `$(…) = $(…)` comparisons pass vacuously when both sides are empty, as happened with md5sum.
- **Fix:** copy the kernel build's survival check, and have `smoke_test.sh` actually run each feature: `$((…))`, `stat -c`/`-f`, `find -path`/`-prune`, `head -c`, `sleep 0.01`, `[`, `dd conv=notrunc`, sorted `ls`.
- **Note:** `FEATURE_MOUNT_FLAGS` is off (the built binary's option table contains only ro/rw/remount). `guest/init:135-137`'s `mount -o bind` works only because busybox auto-detects a directory source as a bind mount. `rbind`, `--make-private` and `noatime` will not work, and init ignores those mount failures.

**L3. `%U`/`%G` in the listing-comparison checks compare nothing (pre-existing, not from these commits).**
- **Where:** `test/qemu/guest/` `create.sh:132,374-375`, `rename.sh:198,477-478`, `readonly.sh:130,248-249`, `write.sh:124,628-629`, `lifecycle.sh:128`, `nfs.sh:216-217`.
- busybox `stat` prints `UNKNOWN` when it cannot look up the name (`coreutils/stat.c:357,363`), and the initramfs has no `/etc/passwd` or `/etc/group`.
- So a wrong uid/gid served by dcfs passes every "listing matches" check.
- **Fix:** use `%u %g`.

**L4. `boot.sh` silently skips the kernel checks on a version mismatch.** `test/qemu/guest/boot.sh:24-48`
- The stock kernel is now the only kernel, but the `*)` branch only echoes "skipping".
- Bumping the pin without editing `boot.sh` makes the three `stock-kernel-*` checks disappear with no FAIL line.
- **Fix:** fail on mismatch, and take the expected version from the build rather than hard-coding it. The comment about the "patched kernel" is also stale.

**L5. Backing filesystems are still made by host mkfs tools.** `test/qemu/scripts/run-qemu.sh:69,181-183`
- They use the host's `/etc/mke2fs.conf` and the host's xfsprogs/btrfs-progs defaults.
- Concretely, this host's ext4 profile lacks `orphan_file` and `metadata_csum_seed`, which e2fsprogs 1.47.4's built-in profile enables. The ext4 under test therefore differs between hosts and CI.
- This is recorded as a follow-up in `log.md:270-271`, but Phase 4 is marked done while its plan says "everything the tests run is pinned".
- **Fix:** use `//third_party/e2fsprogs:mke2fs` with an explicit `MKE2FS_CONFIG`, plus pinned xfsprogs and btrfs-progs.

### Low and nits

- **L6. Phase 2 docs are out of date.** `README.md:114` and `docs/design.md` startup step 5 do not describe what bf3d78d added: refusing a symlink, a non-root owner or a non-regular file, the fchmod tightening, and the `-wal`/`-shm` pre-check. AGENTS.md wants user-facing behavior in the README.
- **L7. Gaps in `cache_permissions.sh`:**
  - Case 3 creates no `-shm`, so removing `"-shm"` from `main.cc:312` would not be caught.
  - No case covers a symlinked or foreign-owned `-wal`/`-shm`.
  - The regular-file check (`main.cc:149`) is untested.
  - `case4-target-untouched` cannot detect "opened but not modified"; the pre-fix code passes it too.
- **L8. Kernel config gaps against the "kernel config once" rule** (`docs/plan/execution.md:45-49`): `CONFIG_XFS_QUOTA` is missing (needed for Phase 18 quotas on xfs). Phase 17's xfstests needs have not been enumerated.
- **L9. Debian package pins live only in `MODULE.bazel.lock`.** There is no `apt.install(lock=…)` and no `--lockfile_mode=error`, so a re-resolution can rewrite them silently. `version_check_test` covers only the 20 named packages, not the transitive ones.
- **L10. Builds are not reproducible:**
  - The kernel build sets no `KBUILD_BUILD_TIMESTAMP`/`USER`/`HOST`.
  - The busybox banner embeds a timestamp.
  - The rootfs image gets a random UUID and hash seed, the current time, and build-time directory mtimes.
  - The Bazel-built mke2fs has an absolute sandbox path baked in as its config file. It is harmless today, since that path doesn't exist and the image matches the built-in profile, but set `MKE2FS_CONFIG` explicitly in `mkrootfs.sh`.
  - `build_kernel.sh` prints its whole ~1500-line log on every successful build.
- **L11. Latent issue in the `mkrootfs.sh` directory skeleton.** Ancestor directories it creates are 0755 and would override a package's own mode. Today no non-0755 directory in the tarball has children (`/root`, `/tmp` and `/var/local` keep 0700/01777/02775 in the image), so there is no effect yet.
- **L12. 6.2 leftover risks:**
  - `rename.sh:107` ignores a failed thaw (`|| true`); the filesystem would stay frozen and the next mutation would hang until the harness timeout.
  - Suspected: an inode released during `drop_caches` (FORGET, then dcfs closes a backing fd) is cleaned up after the quiesce, inside the measured window. Consider `drop_caches; quiesce; drop_caches`, and fail if thaw fails.
- **Nits:**
  - `build_kernel.sh:17-20` claims flex/bison come from the BCR; the code at lines 40-43 uses the host's.
  - `run-qemu.sh:117-140` keeps dead placeholder checks for `kernel.bzl` and comments about `build-kernel.sh`, both removed in 4.4.
  - The phase 3 plan file still says flex/bison/elfutils/zlib come from the BCR; that deviation is recorded only in `log.md`.
  - `qemu_repo.bzl` calls the host `rm`; `repository_ctx.delete` would do.
  - The QEMU version is only logged and substring-checked, not compared with the pin as the phase 4 test-first asked.
- **Info (3.2):** dropping the libfuse patch also lowers libfuse's advertised FUSE protocol minor from 47 to 45. Nothing in dcfs reads `proto_minor` except an INFO log line, so I found no behavior change.

### Where I found nothing
- **3.2:** the `Getattr`/`Setattr`/`ReplyAttr`/`Init` changes are correct, and the generation still goes out through `fuse_entry_param`.
- **6.3:**
  - None of our own code is left unsanitized: the `per_file_copt` pattern matches only glib, zlib and pcre2.
  - abseil, libfuse and sqlite stay instrumented.
  - e2fsprogs/libarchive flags are quoted correctly in the generated script.
- **6.2:** the quiesce does not weaken the check. It happens before the baseline, freeze/thaw do not read, and reads during the check are still counted.
- **QEMU:**
  - It is pinned to pkg-config shims via `PKG_CONFIG_LIBDIR`.
  - The smoke test enforces that only libc/libm are linked and there is no RUNPATH, and the runner passes `-no-user-config`.
  - `run-qemu.sh` cannot pick up a host QEMU: `--qemu`/`--qboot` are required and always given as paths, never looked up on PATH. Its `/usr`/`/bin` guard is incomplete but cannot be reached.
- sha256 pins are present for linux, bc, busybox, qemu+dtc, and e2fsprogs+libarchive.
- Apart from M1, `OpenHardenedCacheFile` and its error paths are sound. The companion fds close before SQLite opens the files, so no SQLite locks are lost, and the lock fd outlives the SQLite connection.

### Verified vs suspected
- **Verified:**
  - M2, from the generated `build_script.sh` and QEMU's configure.
  - M3, from Kconfig, the built `vmlinux` and coreutils' timeout behavior.
  - L1, with a shell repro.
  - L2's `FEATURE_MOUNT_FLAGS` gap, from the built busybox.
  - L3, from busybox's source and the initramfs contents.
  - L5's ext4 profile difference.
  - L11's current modes.
  - M1's code paths and SQLite's open flags.
- **Suspected (reasoned, not reproduced):** the M1 race and `-journal` playback, the L12 window, and the L8 needs of later phases.
