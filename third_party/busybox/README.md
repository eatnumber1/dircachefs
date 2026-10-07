# third_party/busybox: pinned, minimal, Bazel-built busybox

Phase 4, part a (`docs/plan/phases/04-pinned-host-tools.md`). busybox is
fetched and built by Bazel -- not installed on the host, not checked in.
Used as the guest's init system and userspace for every QEMU test (see
`test/qemu/README.md`); `genrule`'d statically so it needs no dynamic
loader in the initramfs.

## Pin

- Version: **1.38.0** (released 2026-05-13; the latest stable release as
  of 2026-10-05, per <https://busybox.net/downloads/>).
- URL: `https://busybox.net/downloads/busybox-1.38.0.tar.bz2`
- sha256: `34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2`
  (matches the published
  `https://busybox.net/downloads/busybox-1.38.0.tar.bz2.sha256`).

### Updating the pin

1. Pick the new release from <https://busybox.net/downloads/>.
2. `curl -LO https://busybox.net/downloads/busybox-<version>.tar.bz2 && sha256sum busybox-<version>.tar.bz2`
   and cross-check against the published `.sha256` file next to it.
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s
   `busybox` `http_archive`.
4. `bazel test //third_party/busybox/...` -- if a new release renames or
   removes a Kconfig symbol `busybox.config.fragment` uses,
   `silentoldconfig` will silently drop it rather than failing the
   build, so also run `bazel-bin/.../busybox --list` (or just read the
   smoke test's output) and confirm every applet it checks for is still
   there.

## Build

busybox's own build system is a Kconfig tree (`Config.in` plus
per-source-file `//config:` comment blocks) driving a plain `make`, not
`configure`+`make` or Meson -- there was nothing for rules_foreign_cc's
`configure_make`/`meson` rules to attach to, so `BUILD.bazel`'s
`busybox_build` is a plain `genrule` running `build_busybox.sh`:

1. `make O=<scratch dir> allnoconfig` -- every applet and feature off.
   (busybox supports out-of-tree builds via `O=`, so the fetched source
   tree itself is never written to -- it can stay exactly as Bazel
   fetched it.)
2. Apply `busybox.config.fragment` by turning each
   `# CONFIG_X is not set` line it names into `CONFIG_X=y` directly in
   the generated `.config` (`sed`). **This is not `KCONFIG_ALLCONFIG`**:
   that mechanism exists in busybox's `scripts/kconfig/conf.c` but does
   not work for this purpose in this busybox version -- `conf -n`
   (allnoconfig) unconditionally forces every symbol to `n` in a second
   pass (needed so `select`-implied symbols end up correctly off too),
   which overwrites whatever `KCONFIG_ALLCONFIG` preloaded regardless of
   its contents. Confirmed experimentally: a `KCONFIG_ALLCONFIG` file
   setting `CONFIG_MOUNTPOINT=y` had no effect on the resulting
   `.config` when run through `allnoconfig`. The sed-based approach is
   also exactly what busybox's own `scripts/kconfig/Makefile` documents
   as the intended way to do this (its header comment: `make
   allnoconfig; sed -i -e '/CONFIG_TRUE/s/.*/CONFIG_TRUE=y/' .config;
   make`).
3. `make O=<scratch dir> silentoldconfig` -- resolves Kconfig
   dependencies (symbols a selected applet needs) non-interactively,
   using defaults for anything not explicitly set.
4. `make O=<scratch dir> -j$(nproc) busybox`.

`CONFIG_STATIC=y` in the fragment makes this a fully static binary (no
`libc.so`/loader needed in the initramfs); confirmed via `file`/
`readelf -l` in `smoke_test.sh`. The built binary currently links no
libraries at all (`crypt`/`m`/`rt` are all reported "not needed" by
busybox's own build at this applet selection).

## Applets

The full list (one `//config:` Kconfig symbol per applet, usually named
after it) is in `busybox.config.fragment`, grouped by which guest script
uses each one and why; re-derive it with:

```
grep -ohE '\b(mount|umount|stat|...)\b' test/qemu/guest/*.sh test/qemu/guest/init
```

(see the fragment file's own header for the full, current command and
reasoning). As of this pin, 63 applets are enabled (`insmod`, for Phase 24's kernel modules, is the newest; the Alpine kernel's drivers are modules):

```
[ ash awk basename cat chgrp chmod chown chroot cmp cp cut date dd diff
dirname dmesg echo fallocate false find free grep head id insmod ip kill ln ls
md5sum mdev mkdir mkfifo mknod more mount mountpoint mv printf pwd
readlink reboot rm rmdir sed sh sleep sort stat sync tail test timeout
touch tr true truncate umount uname uniq wc which
```

`[` (`CONFIG_TEST1`) is a separate Kconfig symbol from `test`
(`CONFIG_TEST`) in this busybox version (`coreutils/test.c`'s `config
TEST1` / `bool "test as ["`) -- step 4.4 (wiring this build into
`test/qemu/`) found this the hard way: every guest script's `if [ ... ];
then` failed with `[: not found` until `CONFIG_TEST1=y` was added
alongside `CONFIG_TEST=y`.

Several other applets this pin enables have the same shape of gap --
a "default y" sub-feature that is still off because its own `depends on`
was unmet at the point `allnoconfig`'s baseline `.config` was generated
(`busybox.config.fragment`'s sed-based fragment application can only flip
a symbol that already has a commented-out line to replace; see that
file's own header comment). All found the same way, by the guest test
suite actually failing once this build was wired into `test/qemu/`:
`CONFIG_FEATURE_SH_MATH`/`CONFIG_FEATURE_SH_MATH_64` (ash's own
`$((...))`, used throughout `guest/lib.sh` and `guest/*.sh`; without it,
"syntax error: support for $((arith)) is disabled"), `CONFIG_FEATURE_FANCY_SLEEP`
plus the further, separate `CONFIG_FLOAT_DURATION` (`guest/init`'s
`sleep 0.2`; without them, "sleep: invalid number"), and
`CONFIG_FEATURE_STAT_FORMAT`/`CONFIG_FEATURE_STAT_FILESYSTEM` (`stat -c`/
`stat -f`, used throughout for mode/owner checks and
`guest/lib.sh`'s `backing_fstype`; without them, "stat: invalid option").
`CONFIG_MD5SUM` is a different kind of gap -- not a sub-feature of
something else, just an applet this fragment hadn't enabled yet -- and a
more dangerous one: every `"$(md5sum a)" = "$(md5sum b)"` content check
in `passthrough.sh`/`write.sh`/`nfs.sh`/`create.sh`/`readonly.sh`/
`lifecycle.sh`/`cache_permissions.sh` compared two *empty* strings and
silently passed regardless of actual content, instead of failing loudly
the way a missing applet usually does.

`CONFIG_CHGRP`/`CONFIG_RMDIR`/`CONFIG_CMP`/`CONFIG_DIFF`/`CONFIG_MKFIFO`
are five more plain missing-applet gaps surfaced the same way (by
running the full suite against this build): `chgrp` (`credentials.sh`),
plain `rmdir` as opposed to `rm -r` (`removed.sh`, `rename.sh`,
`crash.sh`), `cmp`/`diff` (byte-for-byte and tree-diff checks in
`rename.sh`, `write.sh`, `readdir_boundary.sh`), and `mkfifo`
(`credentials.sh`) -- all failed with "not found" rather than silently
passing. Three further sub-feature gaps, same shape as the others above:
`CONFIG_FEATURE_FIND_PATH`/`CONFIG_FEATURE_FIND_TYPE`/
`CONFIG_FEATURE_FIND_MAXDEPTH`/`CONFIG_FEATURE_FIND_PAREN`/
`CONFIG_FEATURE_FIND_PRUNE` (`find -path`/`-type`/`-mindepth`/`\( \)`/
`-prune`, `create.sh`/`rename.sh`/`write.sh`/`readdir_boundary.sh`),
`CONFIG_FEATURE_TOUCH_SUSV3` (`touch -d`, an explicit mtime in
`crash.sh`/`write.sh`), `CONFIG_FEATURE_LS_RECURSIVE`/
`CONFIG_FEATURE_FANCY_HEAD` (`ls -R` in `removed.sh`, `head -c` in
`write.sh`), `CONFIG_FEATURE_DD_IBS_OBS` (`dd conv=notrunc`/`conv=fsync`
in `write.sh`; without it, plain `dd` accepts only `if`/`of`/`bs`/`count`
and any `conv=` makes it exit 1 with no message at all -- no "unrecognized
option" text to grep for, just a bare failure), and `CONFIG_ASH_CMDCMD`
(`setattr.sh`'s `verify_tree_cached()` runs `command diff ...`, the
`command` builtin, to bypass any shell function of the same name; without
it, "command: not found"). `CONFIG_FEATURE_LS_SORTFILES` is the sneakiest
of these: its name suggests it only adds `-S`/`-X`/`-r`/`-v` sort-order
*options*, but without it busybox's `ls.c` never calls `qsort()` on the
directory listing at all (`sort_and_display_files()`/`dnsort()` compile
to no-ops) -- so plain `ls` silently lists entries in raw directory
order, not alphabetical, and every `"$(ls "$dir" | tr '\n' ' ')" =
"a b c "` equality check across `guest/*.sh` (`power.sh`'s `warm_check`,
among others) can fail nondeterministically depending on directory-entry
order, with no error message at all.

Nothing else: no init system (`guest/init` is dcfs's own `/init` script,
not busybox's `init`/`linuxrc`), no network servers (`httpd`, `tftpd`,
`udhcpd`, `telnetd`, `ftpd`, ...), no login/`getty`, no editors (`vi`),
no package-manager-style tools, no syslog daemons. `allnoconfig` already
turns every one of these off, and `busybox.config.fragment` doesn't
re-enable any of them.

### `mountpoint` and `find`

Both specifically called out by the plan, for different reasons:

- **`mountpoint`**: not reliably present in every host's packaged
  busybox (the plan's `test/qemu/README.md` prerequisite list used to
  depend on whatever `/usr/bin/busybox`/`/bin/busybox-static` happened
  to be installed). Building busybox ourselves, pinned, makes this a
  non-issue: `CONFIG_MOUNTPOINT=y` always.
- **`find -ls`**: the plan asked for it on the assumption that the host
  busybox merely had it *disabled*. That assumption doesn't hold:
  **busybox's `find` applet has never implemented `-ls` at all, in any
  released version** -- checked `findutils/find.c` in this exact pin
  (1.38.0): its action-type list (`ACTS(print)`, `ACTS(name, ...)`,
  `ACTS(exec, ...)`, etc., `findutils/find.c:449-494`) and its Kconfig
  (every `FEATURE_FIND_*` option in the same file) have no `-ls` entry
  and no "ls" string appears anywhere in the file or in
  `docs/busybox.pod`. This is a real, structural gap in upstream
  busybox, not a config flag this project failed to flip. The existing
  workaround in `test/qemu/guest/readonly.sh` (`find ... -exec stat -c
  ... {} +` instead of `find ... -ls`, with a comment to that effect
  already in that file) stays as-is; `CONFIG_FEATURE_FIND_EXEC=y` is
  what this fragment enables to support it. No further action is
  possible here without switching the guest to a different `find`
  implementation (GNU findutils, e.g. via the Debian NFS-test rootfs
  that already exists for `nfs_test`), which is out of scope for this
  step.

## Test

`smoke_test.sh` (via `BUILD.bazel`'s `smoke_test` target) is a
host-side check (no root, no kernel): it confirms the built binary is
statically linked, runs `--help`, and checks that every applet from the
list above is present in `busybox --list`.

Before `@busybox` existed in `MODULE.bazel`, `bazel test
//third_party/busybox:smoke_test` failed with a "no such package
'@busybox//'" style error; see this step's commit history for the exact
failing output.

## Deliberately not done here (out of scope for this step)

Wiring this build into `test/qemu/kernel.bzl` (which currently symlinks
a *host* busybox binary in as `@kernel_image//:busybox`) and
`scripts/mkinitramfs.sh`/`run-qemu.sh` is the next step, after the
kernel lane (editing `kernel.bzl` concurrently) merges. This step only
makes `//third_party/busybox/...` build and pass its own smoke test.

## Checks (review R2)

- `build_busybox.sh` fails the build if any line of `busybox.config.fragment`
  is not in the final `.config` exactly as written (a misspelled or renamed
  symbol, or one whose dependency is off), like the kernel build's check, and
  sets `KCONFIG_NOTIMESTAMP` so the version banner carries no build time.
- `smoke_test.sh` runs each feature the guests rely on (`$((...))`, `stat -c`
  and `-f`, `find -path/-prune/...`, `head -c`, `sleep 0.01`, `dd conv=`,
  sorted `ls`, `md5sum`, `touch -d`, ...) and checks that
  `FEATURE_MOUNT_FLAGS` is compiled in.
