# third_party/qemu: pinned, minimal, Bazel-built QEMU

Phase 4, part a (`docs/plan/phases/04-pinned-host-tools.md`). QEMU is
fetched and built by Bazel like any other dependency -- not installed on
the host, not checked in. This document is also this build's audit trail:
every enabled feature, dependency and device is listed here with the
reason it's needed, and `smoke_test.sh` fails if QEMU's own `-device
help`/`-machine help` output ever lists something this file doesn't.

## Pin

- Version: **11.1.2** (released 2026-09-28; the latest stable release as
  of 2026-10-05, confirmed from <https://www.qemu.org/download/>).
- URL: `https://download.qemu.org/qemu-11.1.2.tar.xz`
- Version marker read by smoke_test.sh (keep equal to MODULE.bazel's pin):
  <!-- qemu-version: 11.1.2 -->
- sha256: `731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016`
  (computed locally from the downloaded tarball; cross-checked that
  `qemu-11.1.2.tar.xz.sig` exists on the download server as a GPG
  signature of the same file -- this repository does not verify GPG
  signatures at fetch time, only the sha256 Bazel itself enforces).

### Updating the pin

1. Pick the new release from <https://www.qemu.org/download/>.
2. `curl -LO https://download.qemu.org/qemu-<version>.tar.xz && sha256sum qemu-<version>.tar.xz`
3. Update `url`, `strip_prefix` and `sha256` in `MODULE.bazel`'s `qemu`
   `http_archive`, and the `qemu-version` marker above (the smoke test
   compares the built binary's `--version` with it).
4. `bazel test //third_party/qemu/...` -- the smoke test will fail
   immediately if `-device help`/`-machine help` grew something new (a
   changed default, a Kconfig dependency the new release added); update
   the device/machine lists below and justify any addition before
   relaxing the test.
5. Re-run `bazel test //...` (the full suite) and, once Phase 4's wiring
   step lands, the manual QEMU boot check this step's commit log
   describes.

## Configure line

```
./configure \
    --without-default-features --without-default-devices \
    --target-list=x86_64-softmmu \
    --enable-kvm --enable-tcg \
    --enable-fdt=internal \
    --with-devices-x86_64=dcfs \
    --block-drv-ro-whitelist=raw --block-drv-rw-whitelist=raw \
    --disable-tools --disable-docs --disable-guest-agent \
    --disable-install-blobs \
    --extra-cflags=-Wno-error
```

(`--extra-cflags=-Wno-error` is not a QEMU feature flag -- see "Build
system" below for why it's there.)

(`third_party/qemu/BUILD.qemu`'s `QEMU_CONFIGURE_OPTIONS` is the
source of truth; this block is kept in sync with it by eye at each pin
update.)

Every flag was chosen by actually configuring and building this exact
release with each candidate flag set, not by assumption -- see
"Findings" below for the two places the obvious approach didn't work.

## Dependencies

QEMU's meson.build probes some dependencies unconditionally (regardless
of `--without-default-features`), so "what does this release actually
require" was established empirically: configuring with every optional
feature off and inspecting `ninja -v qemu-system-x86_64`'s link line and
the built binary's `ldd` output.

- **glib-2.0**: always required ("Run-time dependency glib-2.0 found"
  appears in the configure log regardless of any `--disable-*` flag; QEMU
  has no build mode without it). Supplied from the BCR (`@glib//glib`),
  bridged to QEMU's pkg-config-only lookup by a generated `.pc` file --
  see "glib's pkg-config bridge" below, since getting this working was
  most of this build's actual difficulty.
- **zlib**: likewise always required (used by migration, block layer
  compression support code, and more, independent of which optional
  features are enabled). Supplied from the BCR (`@zlib`).
- **pixman**: *not* required with this configuration. QEMU's display/UI
  code needs it, but with every UI backend disabled
  (`--without-default-features` turns off gtk/sdl/vnc/curses/spice/etc.),
  `meson` reports `pixman: NO` and does not link it
  (confirmed: the built `qemu-system-x86_64` has no `libpixman` in
  `ldd` output). Not added as a dependency.
- **dtc (device tree compiler) / libfdt**: required indirectly. The
  `microvm` machine's own Kconfig (`hw/i386/Kconfig`: `config MICROVM
  ... depends on I386 && FDT`) depends on `CONFIG_FDT`, which this
  release only sets when `--enable-fdt` is not `disabled` --
  `--enable-fdt=internal` builds libfdt from the `dtc` subproject instead
  of requiring a system libfdt. This is not about dcfs using device
  trees (it never does, on x86_64 -- nothing in `run-qemu.sh`'s command
  line touches `-dtb` or any DTB path); it's a structural requirement of
  the `microvm` machine type itself in this QEMU release's Kconfig
  graph. Confirmed: the resulting binary links no external libfdt; the
  whole thing is statically folded into `qemu-system-x86_64` via QEMU's
  own build.
  **Where this subproject's source actually comes from is not the
  obvious "it's vendored in the release tarball" answer** -- see "The
  dtc subproject" below; short version: it's fetched separately, pinned
  to the exact commit QEMU's own `subprojects/dtc.wrap` names, because
  the copy inside the QEMU tarball turned out to be unusable from inside
  a Bazel sandbox.
- **Nothing else**. glib, gmodule, pcre2 (glib's own transitive
  dependency, not something this project adds directly) and zlib are all
  linked in *statically* -- see "Hermetic linking" below -- so the built
  binary's only `NEEDED` entries at all are `libc.so.6` and `libm.so.6`
  (`readelf -d`; `smoke_test.sh` checks this directly). No pixman, no
  libusb, no libgnutls, no libcurl, no libnuma, no capstone, none of the
  dozens of optional libraries `./configure --help` lists.

### glib's pkg-config bridge

QEMU's `meson.build` looks up glib with `dependency('glib-2.0', method:
'pkg-config', required: true)` -- hard-coded to pkg-config, no fallback.
`@glib//glib` (the BCR's hand-written `cc_library` overlay, not a
foreign_cc target with its own install tree) has no `.pc` file of its
own. rules_foreign_cc's `configure_make` does turn `deps = [...]`
cc_library targets into correct `-I`/`-L` compiler/linker search-path
flags for the underlying `./configure` (confirmed in the build log), but
deliberately goes no further: it never also guesses a `-l<name>` or
synthesizes a `.pc` file, since picking the right one of those is exactly
what the downstream project's own dependency discovery is supposed to
do. With no `.pc` file on `PKG_CONFIG_PATH`, meson's probe reports
glib-2.0 not found at all, even though every file it needs is already on
the search path.

`pkgconfig_shim.bzl`'s `pkgconfig_shim` rule (`:glib_pc` in
`BUILD.bazel`, and `:zlib_pc` alongside it -- see below) closes that one
gap: it reads `@glib//glib`'s (and, for a related but separate reason
below, `@glib//gmodule`'s) `CcInfo` provider directly -- its
`compilation_context`'s include directories and `linking_context`'s
*static* library archives -- and writes a `glib-2.0.pc` whose
`Cflags:`/`Libs:` point at them, wired in via `configure_make`'s `env`:
`PKG_CONFIG_LIBDIR = $(execpath :glib_pc):$(execpath :zlib_pc)`. Four
things about this needed to be exactly right, each found from a real
build failure, not assumed:

- **Paths must use pkg-config's own `${pcfiledir}` variable, not an
  absolute path.** Each Bazel sandboxed action gets its own private
  sandbox root; an absolute path captured while writing `:glib_pc`'s
  content would not exist in a *different* action's sandbox (the one
  that actually reads the `.pc` file later). `${pcfiledir}` is resolved
  by pkg-config itself, at the point something actually reads the file,
  to wherever that specific sandbox instance actually put it -- the one
  thing that's genuinely stable. (The number of `../` hops back to the
  execroot from `:glib_pc`'s own output directory is computed once, from
  that directory's own execroot-relative path length, and reused for
  every include/lib path -- see `pkgconfig_shim.bzl`'s comments.)
- **`:glib_pc` must be given to `configure_make` as `data`, not
  `build_data`.** `build_data` transitions its target to the *exec*
  configuration; since `:glib_pc`'s own action only reads `File.path`
  *strings* out of `@glib//glib`'s CcInfo (to embed as text) rather than
  depending on the files themselves, nothing in that configuration
  forces an exec-config copy of glib to actually be built -- the first
  attempt failed with `glib.h: No such file or directory` even though
  the embedded path "looked" right, because
  `bazel-out/k8-opt-exec/bin/external/glib+/...` genuinely did not exist
  on disk. `data` keeps `:glib_pc` in the same (target) configuration
  `deps` above already uses, so the files its embedded paths point to are
  already guaranteed present via that existing dependency -- no second
  glib build needed.
- **`@glib//gmodule` has to be a real, separate `deps` entry on the main
  `configure_make` target too, not only read by `:glib_pc`.** A target's
  CcInfo only describes where its outputs *would* be; it doesn't make
  Bazel build them. `qemu/transactions.h` unconditionally does `#include
  <gmodule.h>` (regardless of whether anything downstream ever links
  GModule -- modules and plugins are both disabled here, so meson's own
  `dependency('gmodule...')` call is never even reached), so `:glib_pc`
  folds `@glib//gmodule`'s include directory into the same
  `glib-2.0.pc` (something a *real* system install's glib-2.0.pc would
  never do -- upstream keeps them as two separate pkg-config packages --
  but nothing here ever consults a `gmodule*.pc` file either, so it's
  the simplest correct fix for this one header). That alone wasn't
  enough until `@glib//gmodule` was also added directly to the main
  `deps` list: without it, nothing in the build graph ever caused
  `@glib//gmodule` to actually be compiled, so the embedded `-I
  .../gmodule/include_glib-2.0` pointed at a directory that, again,
  genuinely did not exist.
- **The `Libs:` field must be a literal path to each dependency's
  *static* archive, never a bare `-l<name>`.** glib itself has only a
  static artifact, so this didn't show up until an orchestrator review
  actually ran `ldd`/`readelf -d` on the finished binary: pcre2 (a
  transitive dependency, reached through glib) has *both* a static and a
  shared library built by Bazel in the same directory, and a plain
  `-lpcre2` let the linker pick -- which turned out to be the shared one,
  with an absolute path into this one build's own Bazel output_base
  baked into both the `NEEDED` entry and the `RUNPATH`. See "Hermetic
  linking" below and `pkgconfig_shim.bzl`'s "Fully static, on purpose"
  section for the full story, including the same problem (plus a
  different one) for zlib.

zlib *also* needs a `:zlib_pc` shim, for a reason that wasn't obvious
until the finished binary was actually inspected: `dependency('zlib',
required: true)` has no `method:` restriction, and meson's built-in
zlib-specific probe did find *something* with no shim at all -- just the
**host's** zlib (via pkg-config's own default search path, which
`PKG_CONFIG_PATH` alone doesn't suppress), not `@zlib`. See "Hermetic
linking" below for the full account and the `PKG_CONFIG_LIBDIR` fix.

### A dependency that is *not* linked, and why it briefly appeared

While developing this build (manually, outside Bazel, on a host that
happens to have `libx11-dev` installed), `./configure`'s summary showed
`x11: YES`. This looked alarming (no X11 anywhere should be needed for a
headless build) until traced to `meson.build`: `x11 = dependency('x11',
method: 'pkg-config', required: gtkx11.found())` -- an *optional* probe
with `required: false` (since `gtkx11` is never found when GTK is
disabled), meaning meson looks for X11 opportunistically and uses it if
present, but never requires it. Confirmed via the built binary's `ldd`
output: no `libX11` is linked, regardless of what the configure summary
printed. Bazel's hermetic build environment has no X11 development
package in its sandbox at all, so this probe will simply report `NO`
there and this is moot in practice -- documented here only so a future
"why does configure mention X11" question doesn't cause alarm.

### The dtc subproject

`--enable-fdt=internal` needs the `dtc` meson subproject
(`subprojects/dtc`). The QEMU release tarball ships it as a real git
checkout, already at the exact commit `subprojects/dtc.wrap` names
(`b6910bec11614980a21e46fbccc35934b671bd81`) -- in principle meaning no
network access should ever be needed for this. It doesn't work that way
inside Bazel:

- Bazel's own repository-fetching tar extraction (the same mechanism a
  plain `http_archive` uses) drops the *entire* `subprojects/dtc`
  directory, not just its `.git` -- confirmed by comparing a plain `tar
  xf` of the identical, same-sha256 tarball (which has 311 ordinary,
  non-dotfile files under `subprojects/dtc/`, plus `.git`) against what
  actually lands in Bazel's fetched copy of the same archive (nothing
  under `subprojects/dtc` at all -- not even those 311 files). Every
  sibling `subprojects/*` entry without a `.git` survives extraction
  fine; only the one directory containing a real git checkout does not.
- Separately (and this would still have mattered even if the directory
  had survived), Bazel's BUILD-file `glob()` can never match a
  dot-prefixed path segment under any pattern at all -- not even an
  explicit, non-wildcard `"subprojects/dtc/.git/**"` (`glob pattern ...
  didn't match anything`). So even a *present* `.git` directory could
  never be added to this package's `all_srcs` filegroup by globbing it
  back in.

With no `subprojects/dtc` directory and no `.git`, meson would try (and,
sandboxed, fail) to `git fetch` the subproject from gitlab.com.

The fix, in `qemu_repo.bzl` (a small custom `repository_rule`, replacing
what would otherwise be a plain `http_archive`): fetch `dtc` as a
*second*, separately sha256-pinned download, from the exact commit
`subprojects/dtc.wrap` already names (same bytes QEMU's release process
itself used -- this is not a different pin, just a different transport),
placed directly into `subprojects/dtc` via `download_and_extract`'s own
`output=` parameter. That's a repository-rule-level filesystem operation,
not a BUILD-file glob, so neither dotfile-dropping behavior applies to
it. `subprojects/dtc.wrap` is then deleted (`rm -f`): with no `.wrap`
file present, meson uses the directory that's already there as a plain,
unmanaged subproject (meson's own documented behavior for a subproject
with no corresponding `.wrap`), with no fetch or git-revision validation
attempted at all.

- dtc pin: commit `b6910bec11614980a21e46fbccc35934b671bd81` (unchanged
  from what this QEMU release's own `subprojects/dtc.wrap` names).
- URL: `https://gitlab.com/qemu-project/dtc/-/archive/b6910bec11614980a21e46fbccc35934b671bd81/dtc-b6910bec11614980a21e46fbccc35934b671bd81.tar.gz`
- sha256: `e115f987eec23a1ba25150a46ced1675de3716072d3b4905afb3a9cda0f007c7`

Updating this pin is not a separate, independent decision: it should
only ever change to track whatever commit a new QEMU release's own
`subprojects/dtc.wrap` names, as part of the normal QEMU version bump
procedure above (check the new release's `subprojects/dtc.wrap` for its
`revision` and update `dtc_url`/`dtc_sha256`/`dtc_strip_prefix` in
`MODULE.bazel`'s `qemu_repo` call to match).

## Firmware: qboot

`pc-bios/qboot.rom` ships prebuilt inside the release tarball; it is not
built from source by this project's Bazel rule. The plan allowed either
approach ("build it from its pinned source with Bazel if that is
simple, otherwise take `qboot.rom` from the pinned QEMU tarball"). Building
it from source (`roms/qboot/`) was not simple: qboot is a from-scratch
16-bit/32-bit x86 firmware image built with its own cross-compilation
flags (`-m16`/`-m32`, no standard library, a custom linker script) that
has nothing in common with the host-targeting toolchain this repository's
Bazel build otherwise uses, and would need its own hermetic 32-bit-capable
C toolchain just for this one ~100 KB ROM. Taking the prebuilt ROM
straight from the sha256-pinned, signature-published QEMU release tarball
is exactly as reproducible as building it would be (it's the same bytes
either way, since upstream builds qboot.rom from that same `roms/qboot/`
source as part of preparing the release), for a fraction of the
complexity. `BUILD.qemu` exposes it as `@qemu//:pc-bios/qboot.rom`.

No other firmware blob is installed or used (`--disable-install-blobs`
and the lack of any `-bios`/`-pflash` other than qboot in
`run-qemu.sh`).

## Build system

QEMU's own `./configure` is not plain Meson: it's a wrapper that creates
a Python venv from wheels vendored inside the release tarball
(`python/wheels/*.whl` -- `meson`, `pycotap`, `qemu_qmp`; no network
access needed at configure time) and invokes Meson from there, then
writes a `Makefile` that forwards `make` to `ninja`. This is close enough
to rules_foreign_cc's `configure_make` pattern (configure, then make) to
use directly -- no hand-written rule was needed for the build itself, and
rules_foreign_cc's own `meson` rule (which runs `meson setup` itself)
would fight with QEMU's own venv-based Meson invocation instead of
helping -- but two things about `configure_make`'s own defaults needed
overriding, both found by actually watching what got built, not assumed:

- **`targets = ["qemu-system-x86_64"]`, not the default `["", "install"]`
  (plain `make`, i.e. ninja's default target, then `make install`).**
  Ninja's default target also builds QEMU's own ~250 qtest/unit test
  binaries under `tests/` (confirmed: `ninja qemu-system-x86_64` alone is
  ~1075 build steps; the bare default is ~1400+ and visibly compiles
  things like `tests/audio/test-audio.c`, which drags in `gio`/`gobject`
  headers this build has no other use for at all) -- all useless weight
  here. The natural-looking fix, keeping `"install"` as a second target
  to get the binary into `$INSTALLDIR` afterwards, does not actually
  avoid that cost: meson's generated `ninja install` target itself
  depends on the default (`all`) target first, so requesting
  `["qemu-system-x86_64", "install"]` still built the entire test suite
  in practice (confirmed by watching it happen). The fix is to not run
  `install` at all: build only the one target we want, then copy that
  one binary into place ourselves.
- **`postfix_script` places the binary**, instead: `configure_make`'s own
  final step always copies `$BUILD_TMPDIR/$INSTALL_PREFIX` (wherever
  `./configure --prefix=...` pointed) into the rule's real output
  directory -- with no `install` target ever run, nothing is ever written
  there, so `out_binaries = ["qemu-system-x86_64"]` would find nothing.
  `postfix_script` (runs once, right after the `targets` list, before
  that final copy) does the one copy meson's "install" would otherwise
  have done: `mkdir -p $$INSTALLDIR/bin && cp
  $$BUILD_TMPDIR/build/qemu-system-x86_64 $$INSTALLDIR/bin/...`. Getting
  this one line right took three tries, each a real, distinct error, not
  a guess fixed blind: a bare `$INSTALLDIR` is rejected outright by
  Bazel's own attribute validator (**"use `$(INSTALLDIR)` ... or escape
  the `$` as `$$` if you intended this for the shell"**); `$(INSTALLDIR)`
  is rejected too (**"not defined"** -- `INSTALLDIR`/`BUILD_TMPDIR` are
  plain shell environment variables by this point in the generated
  script, not registered Bazel Make variables); and the first attempt at
  the shell-escape route, mirroring rules_foreign_cc's own *internal*
  `$$FOO$$` template spelling, is also wrong for a caller's own
  attribute value -- each `$$` collapses to one literal `$` during
  attribute expansion, so `$$BUILD_TMPDIR$$` becomes the literal text
  `$BUILD_TMPDIR$`, which bash reads as the correct variable reference
  immediately followed by a stray extra `$` (the first real build's `cp`
  failed with a source path one character off:
  `.../qemu.build_tmpdir$/build/qemu-system-x86_64`). The one correct
  spelling is a single opening `$$` with **no** matching close:
  `$$BUILD_TMPDIR` (collapses to plain `$BUILD_TMPDIR`; bash's own
  variable-name parsing then stops cleanly at the following `/`, no
  terminator needed).
- One more thing fixed the same way (confirmed from the actual error, not
  assumed): with `deps = [...]` cc_library targets, rules_foreign_cc
  computes CPPFLAGS/LDFLAGS from the **current Bazel C++ toolchain's own
  configured copts** -- which, in this repository, includes this
  project's own `build --copt -Werror` (`.bazelrc`). That's the exact
  same problem the top-level `.bazelrc`'s
  `--per_file_copt=external/.*@-Wno-error` already exists to solve for
  ordinary Bazel-native `cc_library`/`cc_binary` compilation (its own
  comment: "scope -Werror back off for code under external/ only"), but
  `per_file_copt` has no effect here (this whole build is one opaque
  shell action invoking QEMU's own `./configure`/`ninja`, not Bazel's own
  compile actions, so no per-file pattern matching ever applies to it).
  `--extra-cflags=-Wno-error` is QEMU's own supported mechanism for the
  same end result. The actual failure this fixed: GCC 13 flagging an
  upstream `-Wimplicit-fallthrough` case in `qapi/opts-visitor.c` that
  upstream's own (older, in-tree-tested) default toolchain apparently
  never warned on -- not a real bug worth patching around, just this
  project's own stricter warning policy reaching somewhere it was never
  meant to.

## Devices

From the checked-in `configs/devices/x86_64-softmmu/dcfs.mak`
(added to the fetched source by `0001-add-dcfs-device-config.patch`;
see that file's own comments) plus `--without-default-devices`, QEMU's
`-device help` lists exactly:

<!-- devices-start -->
- `virtio-blk-device`: `run-qemu.sh`'s only disk device (`-device
  virtio-blk-device,drive=...`, once per test disk). Explicitly selected
  (`CONFIG_VIRTIO_BLK=y` in `dcfs.mak`) -- not pulled in automatically by
  `CONFIG_MICROVM` or `CONFIG_VIRTIO_MMIO`, which only provide the
  generic virtio transport, not any particular device model.
- `isa-serial`: the serial console `run-qemu.sh` uses
  (`isa-serial=on` on the `-M microvm,...` line, `-serial stdio`).
  Pulled in by `CONFIG_MICROVM`'s Kconfig (`select SERIAL_ISA`).
- `mc146818rtc`: the CMOS RTC `run-qemu.sh` explicitly enables
  (`rtc=on` -- see `test/qemu/README.md`'s "Fast boot" section for why
  turning this *on* is the fast choice, not the slow one). Pulled in by
  `CONFIG_MICROVM`'s Kconfig (`select MC146818RTC`).
- `virtio-serial-device`: **not used by run-qemu.sh at all**, and not
  something dcfs's build asked for. It appears because
  `hw/char/meson.build` unconditionally includes `virtio-serial-bus.c`
  whenever `CONFIG_VIRTIO` is set (`system_ss.add(when: 'CONFIG_VIRTIO',
  if_true: files('virtio-serial-bus.c'))`) -- every other virtio device
  type in this codebase is gated behind its own specific Kconfig symbol
  (e.g. the PCI transport variant, `virtio-serial-pci.c`, correctly
  checks `CONFIG_VIRTIO_SERIAL`), so this looks like an upstream
  oversight rather than an intentional "always include the base serial
  device" design. Nothing in this project's harness ever instantiates
  one; it is listed here, rather than patched out, because patching
  QEMU's own device wiring is a bigger and riskier change than
  documenting one harmless extra qdev type that nothing ever uses.
<!-- devices-end -->

Everything else `--without-default-features --without-default-devices`
would otherwise include (every PCI device, every USB device, every
display/audio/network backend's qdev types, TPM, every other machine's
devices) is absent.

### Kconfig closure forced by `CONFIG_MICROVM`, but not independently selectable

`hw/i386/Kconfig`'s `config MICROVM` entry `select`s a number of other
Kconfig symbols as a hard dependency of the machine type itself:
`ACPI_HW_REDUCED`, `ACPI_PCI`, `ACPI_MEMORY_HOTPLUG`, `ACPI_NVDIMM`,
`APIC`, `IOAPIC`, `I8259`, `I8254`, `ISA_BUS`, `SERIAL`, `DEVICE_TREE`,
`MEM_DEVICE`, `MSI_NONBROKEN`, `PCI`, `PCI_EXPRESS`,
`PCI_EXPRESS_GENERIC_BRIDGE`, `USB`, `USB_XHCI`, `USB_XHCI_SYSBUS`,
`VIRTIO`, `VIRTIO_MMIO` (the full resolved list is in
`x86_64-softmmu-config-devices.mak` inside the build directory after
configuring). This means the built binary does contain PCI, USB and
expanded ACPI support code, compiled in because `--without-default-devices`
only controls which devices get *selected by default*, not what a
selected machine's own Kconfig graph structurally requires.

This does **not** contradict "no PCI devices, USB, ... nothing else" from
the plan: none of this compiled-in support code registers an
independently instantiable `-device` type (confirmed by the exhaustive
`-device help` list above -- no `bus PCI` or `bus USB` entries appear at
all), and nothing on `run-qemu.sh`'s command line (`-nodefaults`, no
`-device` beyond `virtio-blk-device`) ever instantiates a PCI bridge, a
USB controller, or an ACPI table beyond what the fixed `microvm` machine
topology always builds. It is simply not possible to select the
`microvm` machine type in this QEMU release without this code being
compiled in; the alternative would be not using `microvm` at all, which
contradicts the rest of the plan (direct-boot, PVH, no PCI bus to
enumerate).

## Machines

<!-- machines-start -->
- `microvm`: the machine `run-qemu.sh` boots.
- `none`: QEMU's always-present empty machine (exists in every QEMU
  build regardless of configuration; not something this project's config
  adds or could remove).
<!-- machines-end -->

## Accelerators

- **KVM** (`--enable-kvm`): the primary accelerator (`run-qemu.sh`'s
  `-accel kvm -cpu host` when `/dev/kvm` is writable).
- **TCG** (`--enable-tcg`): the software-emulation fallback (`-accel tcg
  -cpu max`) `run-qemu.sh` uses when `/dev/kvm` isn't writable (phase
  5.1 measures and budgets for how much slower this is).

No other accelerator (`--enable-hvf`, `--enable-whpx`, `--enable-xen`,
`--enable-nvmm`, `--enable-mshv`, `--enable-nitro`) applies to this
Linux/KVM host and none is enabled.

**Manual boot check, KVM: works.** Booting `test/qemu:boot_test`'s own
kernel/initramfs by hand with this QEMU and qboot in place of the host
ones (`-accel kvm -cpu host`, plus the one fixed finding below) printed
`ALL-TESTS-PASSED` in ~2s wall time -- same ballpark as the host QEMU
this project used before this step.

**Manual boot check, TCG: inconclusive, not confirmed working.** The
same guest under `-accel tcg -cpu max` (both `-smp 2`, matching
`run-qemu.sh`'s e2e setting, and `-smp 1`) produced **no console output
at all** -- not even the kernel's own first boot line -- within a 600s
timeout, on both attempts. The QEMU process itself was not deadlocked
(confirmed via `ps`: a steady ~99% CPU the whole time, consistent with
real computation, not a blocked wait), and this was on a host under
heavy, uncontrolled contention from other concurrent lanes' own full
test suites and builds (load average 5.5-6.5 on 4 cores, several GB
swapped) -- the same kind of contention `test/qemu/README.md`'s own KVM
timing notes already call out as making numbers "worse than a quiet
machine would show." Whether 600s of silence is "just" that contention
taken to an extreme, or a real TCG-specific problem with this minimal
build's configuration (an untested code path, given every other check in
this step used KVM), was not resolved -- two attempts with no
qualitatively different result is this step's stop-and-report line.
**This needs a retest on a quiet host before TCG can be considered
confirmed working with this build**, ideally before Phase 5.1 (which
depends on the TCG fallback path) is scheduled.

## Block layer

`--block-drv-ro-whitelist=raw --block-drv-rw-whitelist=raw`: raw disk
images only (this QEMU release renamed the single
`--block-drv-whitelist` flag the plan mentioned into separate read-only
and read-write whitelists; both are set to the same one-entry list,
since `run-qemu.sh` only ever uses raw images for both directions). No
qcow2, no vmdk, no network block drivers (nbd/iscsi/ceph/gluster/etc.).

## Off

Everything `--without-default-features` already disables by default,
confirmed still off in the final configure summary for this exact
configuration: every user interface (SDL, GTK, VNC, Spice, curses,
D-Bus display), every network backend (slirp, vhost-net, vhost-user,
af-xdp, passt, netmap, l2tpv3), audio (ALSA/OSS/PulseAudio/PipeWire/
JACK/sndio), TPM, the guest agent, `tools` (`qemu-img` and friends --
confirmed: the build produces no binary other than
`qemu-system-x86_64`), docs, plugins, every tracing backend, seccomp,
every crypto/compression library (gnutls, gcrypt, nettle, lzo, snappy,
zstd, bzip2, lzfse), rdma, numa, capstone, valgrind support, every
optional disk-image format (qcow1/qcow2 is handled by the block-driver
whitelist above, independent of these library-level feature flags; vdi,
vhdx, vpc, vvfat, parallels, dmg, cloop, bochs, qed are all off at the
feature level too, so they couldn't be selected even if the whitelist
allowed them).

## Test

`smoke_test.sh` (via `BUILD.bazel`'s `smoke_test` target) is a host-side
check (no root, no kernel, no guest boot -- just running the built
binary with `--help`-style flags): it runs `qemu-system-x86_64
--version` and checks the output, then runs `-device help` and
`-machine help` and fails if either lists anything the "Devices" or
"Machines" sections above (the `<!-- ...-start/-end -->` blocks the
script parses) do not. This is the test the plan asked for: "a check
records `-device help` and `-machine help` output and fails if a device
or machine appears that the README does not list."

Before `@qemu` existed in `MODULE.bazel`, `bazel test
//third_party/qemu:smoke_test` failed with a "no such package '@qemu//'"
style error (there was nothing to build yet); see this step's commit
history for the exact failing output.

`:qemu_system_x86_64` (`BUILD.bazel`) is what `smoke_test` (and anything
else wanting the built binary) actually depends on: `configure_make`'s
declared outputs are not individually addressable by their on-disk path
as a label (`@qemu//:qemu/bin/qemu-system-x86_64` fails with "not
declared in package ''", confirmed, even once that exact file exists and
is built) -- what *is* addressable is the rule's own `OutputGroupInfo`,
which `out_binaries = ["qemu-system-x86_64"]` (`BUILD.qemu`) gives a
matching group for. `:qemu_system_x86_64` is a plain `filegroup` pulling
that one output group out as an ordinary single-file target.

Binary: 11 MB, not stripped. `readelf -d`'s `NEEDED` entries: `libc.so.6`
and `libm.so.6` only (glib/gmodule/pcre2/zlib are all linked in
statically -- see "Hermetic linking" below); no `RUNPATH`/`RPATH`.
`smoke_test.sh` checks this directly. First build from a cold Bazel
disk-cache (including rules_foreign_cc's own one-time toolchain bootstrap
-- hermetic `autoconf`/`automake`/`m4`/`make`/`meson`/`pkgconf`, all built
from source the first time any target in this build graph needs them,
then disk-cached for every subsequent build in this or any other lane
sharing the same disk cache): a little over 500s wall time on this host.
A no-op rebuild (nothing changed) is a few seconds; a change to
`QEMU_CONFIGURE_OPTIONS` or the device config re-runs `./configure` and
the affected subset of the ~1075-step ninja build, not the whole thing
(ninja's own incremental tracking).

### Hermetic linking

The smoke test's `readelf -d` check exists because the first working
build wasn't actually hermetic, in two ways an orchestrator review
caught (`ldd` on the built binary):

- `libz.so.1 => /lib/x86_64-linux-gnu/libz.so.1` -- the **host's** zlib,
  not `@zlib`. Cause: `PKG_CONFIG_PATH` only *adds* a search directory;
  it does not stop pkg-config from also finding a real system `zlib.pc`
  in its own compiled-in default path, which meson's unrestricted
  `dependency('zlib', ...)` happily preferred (confirmed by the version
  it reported: the host's 1.3, not this project's pinned 1.3.2). Fixed
  with a `:zlib_pc` shim (same mechanism as `:glib_pc`) plus switching
  `env`'s `PKG_CONFIG_PATH` to `PKG_CONFIG_LIBDIR`, which *replaces*
  pkg-config's default search path instead of adding to it -- the only
  `.pc` files visible to this build are now the two this project writes
  itself.
- An **absolute path straight into this one build's own Bazel
  output_base**, baked into pcre2's `NEEDED` entry and `RUNPATH`
  (`/home/.../89619d3d.../bazel-out/.../external/pcre2+/libpcre2.so`).
  This would have broken in any other checkout, in CI, from a shared
  cache hit, or the moment that output_base is cleaned. Cause:
  `pkgconfig_shim.bzl`'s `Libs:` field used to emit a bare `-l<name>`
  (e.g. `-lpcre2`) plus a `-L<dir>` search path -- but pcre2 (like zlib,
  unlike glib itself) has *both* a static `.a` and a shared `.so` built
  by Bazel in the very same directory, and a bare `-l` flag lets the
  linker choose; it prefers the shared one whenever both are present,
  regardless of which artifact the shim actually meant to point at.
  Fixed by having the shim emit the literal path to each dependency's
  *static* archive instead of a search-by-name flag -- ld links exactly
  that file, full stop -- and by failing the Bazel build loudly if a
  dependency turns out to have no static archive at all, rather than
  silently falling back to a shared one. See `pkgconfig_shim.bzl`'s
  "Fully static, on purpose" section for the full account.

## Deliberately not done here (out of scope for this step)

- **Wiring into the test harness** (`run-qemu.sh`, `kernel.bzl`, any
  `BUILD.bazel` test rule) is the next step, after the kernel lane
  (editing `kernel.bzl` concurrently) merges. This step only makes
  `//third_party/qemu/...` build and pass its own smoke test.
- A manual boot of an existing guest's kernel/initramfs under this QEMU
  (both KVM and TCG) was done as a one-off check, not as an automated
  Bazel target; results are in this step's commit log, not here.
