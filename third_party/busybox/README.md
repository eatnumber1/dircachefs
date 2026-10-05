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
reasoning). As of this pin, 55 applets are enabled:

```
ash awk basename cat chmod chown chroot cp cut date dd dirname dmesg
echo fallocate false find free grep head id ip kill ln ls mdev mkdir
mknod more mount mountpoint mv printf pwd readlink reboot rm sed sh
sleep sort stat sync tail test timeout touch tr true truncate umount
uname uniq wc which
```

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
