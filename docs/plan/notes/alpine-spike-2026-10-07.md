# Alpine series-pin spike (2026-10-07, dcfs-investigator; prototype on branch spike-alpine in lane-2)

Spike report: Alpine v3.24 packages pinned by release series. Everything below was observed in lane-2. The prototype is committed on `spike-alpine` as 1b77606 (`spike: ...`, trailer included). Nothing is wired into existing targets. The Bazel server is left running.

## Verdict
The design works end to end. Alpine's signed packages (v3.24: linux-virt 6.18.55-r0, QEMU 11.0.3, e2fsprogs 1.47.4, xfsprogs 7.0.1, btrfs-progs 6.17.1, busybox-static 1.37.0) boot our guest, `boot_test`'s checks, and the `lifecycle.sh` and `passthrough.sh` e2e scripts. The four real costs are:
- Kernel drivers become modules, so the guest needs a module initramfs.
- Alpine has no dm-dust.
- Old apks disappear from the mirror within days.
- Components for OSV scanning must be named by their origin package.

## 1. The rule (`third_party/alpine/{alpine.bzl,apk.py,keys/}`)
- **Signature checks.** `alpine_package(branch, repos, arch, package(s), series, closure, version, sha256, expires)` fetches each repo's `APKINDEX.tar.gz` and verifies it against the checked-in key (alpine-keys 2.6-r0 `-6165ee59`, sha256 `207e4696...`, identical to the copy on gitlab.alpinelinux.org/aports). It picks the newest version where the series is followed by `.`, `-` or `_`, so `6.18` does not match `6.180`. Each apk then gets three checks: the RSA/SHA-1 signature over the control stream, the index `C:` field (Q1+sha1 of that stream) and `.PKGINFO` `datahash` (sha256 of the data stream). A tampered index fails with `BAD SIGNATURE`.
- **No host tools.** The verifier is stdlib-only Python (RSA PKCS#1 v1.5 via `pow()`), run by rules_python's hermetic 3.12. No host openssl or python is needed.
- **Closure.** `closure = True` resolves `D:` dependencies through `p:` providers across main and community. It is needed for QEMU (51 packages) and the fs tools (25).
- **Fetch times** (Bazel cold, on this loaded host): linux-virt 43 MB, 8 s; QEMU closure 27 MB apks / 84 MB unpacked, 12 s; fs tools plus busybox 4.8 MB, 6.5 s; linux-virt-dev 21 MB apk / 132 MB unpacked, 26 s; gcc+binutils closure 67 MB apks / 184 MB unpacked, 44 s. Index downloads are 0.5 MB (main) and 2.5 MB (community), under 0.3 s each.
- **What Bazel loses without a sha256:**
  - The repository cache is never consulted. It does store the file after the fact, but a fresh runner re-downloads.
  - `MODULE.bazel.lock` stays untouched. I checked: `use_repo_rule` repos are not in the lockfile, and the file did not change.
  - A CI runner on a new output base picks whatever is newest that day.
  - Within one output base the repo is fetched once. A second build took 0.65 s with no refetch, and the marker file persists on disk. I did not test a server restart.
  - The rule returns `repo_metadata(reproducible = False)`.
  - The rule writes `LOCK` (name, version, origin, sha256 of every apk). `alpine.lock` is a checked-in golden copy, and the manual `drift_test` fails when Alpine has moved. Failing first: `-linux-virt 6.18.54-r0 / +linux-virt 6.18.55-r0`.
- **Escape hatch.** `version=`, `sha256=` and `expires=` skip the index and still verify the signature inside the apk. `expiry_test` is tagged `external` so a cached pass is never served. Failing first with `expires = "2026-10-01"`: `FAIL: pin expired on 2026-10-01 (today 2026-10-06)`. It passes with `2027-01-06`.
- **Pins need our own copy of the apk.** On the mirror, 6.18.54-r0 was still served (HTTP 200) but 6.18.53 and older returned 404. A pin therefore needs a mirror URL we control, or a repository-cache copy.
- **Index consistency risk.** Each repo fetches its own index, seconds apart. `linux-virt` and `linux-virt-dev` could come from different indexes if Alpine publishes in between. Use one shared index.

## 2. Kernel (linux-virt 6.18.55-0-virt)
- **Config coverage.** Of the 95 `kernel.config` options, 76 are `=y` in Alpine and 17 are `=m`. `DM_DUST` is missing. `LOCALVERSION` obviously differs.
- **Modules.** The `=m` options include `virtio_blk`, `ext4`, `btrfs`, `xfs`, `fuse`, `loop`, dm-*, nfs/nfsd and `autofs4`. Modules ship as `.ko.gz`, and the guest init needs `modprobe`.
- **busybox.** Our busybox fragment has no insmod or modprobe. I used Alpine's busybox-static in a supplementary cpio, concatenated onto the stock initramfs, with `init` patched to `modprobe` a list. Alpine's busybox handles `.ko.gz`, so adding applets to our fragment is the other option.
- **Boot result.** With `virtio_blk`+`ext4` loaded, `boot_test` gives 8 PASS and 2 FAIL. The two failures are the identity checks, `uname -r` ending `-dcfs-stock` and busybox v1.38.0, so they need loosening.
- **Other e2e scripts.** `lifecycle.sh` (29 PASS) and `passthrough.sh` (FUSE passthrough, 11 PASS) passed on both kernels. I ran them with fuse and loop loaded, and all other modules unloaded.
- **Boot times, KVM.** The host was at load 8 to 30, so numbers are noisy. Interleaved runs, n=8, min of wall:

| Variant | Wall (min) | Wall (median) |
|---|---|---|
| our kernel + our initramfs | 1.69 s | 2.77 s |
| Alpine + `virtio_blk`+`ext4` as `.ko.gz` | 1.80 s | 3.24 s |
| Alpine + `virtio_blk`+`ext4` as `.ko` | 1.59 s | 3.10 s |
| Alpine + 15 modules (29 with deps) | 4.53 s | 7.29 s |

  CPU time (user+sys) tells the same story: 1.52 s ours, 1.54 s Alpine with plain `.ko`, 4.8 s for the full set.
- **Full set.** Most of the extra time is module init, per module: xfs about 2 s, ext4 0.85 s, jbd2 0.5 s, btrfs 0.5 s, virtio_blk 0.47 s. The remaining modules sum to about 3 s. Load only the modules each test needs, and decompress the modules when building the initramfs.
- **TCG.** Ours 9.6 s, Alpine with the minimal set 11.2 s, full set 23.9 s.
- **Whole Alpine stack (Alpine kernel, Alpine QEMU, Alpine mkfs).** Under KVM it ran 2.0 to 2.2 s per boot, in line with the earlier 2.0 s vs 2.3 s figure.
- **Sizes.** The bzImage is 12.6 MB against our 3.5 MB. The initramfs is 9.9 MB for the minimal set and 15.9 MB for the full set, against our 8.4 MB.
- **dm-dust: it builds.**
  - It builds out-of-tree against linux-virt-dev and loads (`dust-loaded`, `/sys/module/dm_dust`) in 8 s of compile time.
  - Source is `dm-dust.c` from the `v6.18.55` tag on git.kernel.org. That is 1 file, so it would need an `http_file` pin.
  - The host gcc 13 fails on `-fmin-function-alignment=16`. It needed Alpine's gcc 15.2 plus binutils plus libelf, run through musl via wrappers for every musl ELF (`muslwrap.py`, 65 wrappers). That is 67 MB of toolchain download.
  - The module is unsigned. `MODULE_SIG_FORCE` is not set, so it loads.
  - `dmsetup` is separate: Alpine busybox has none.
  - Recommendation: drop dust from 11.3 and use `error` and `flakey`. A table reload (suspend/load/resume) with `error` over a sector range gives dust's per-sector read failures. That choice is the orchestrator's call, since 11.3 names dust.

## 3. Host tools on this glibc host (`musl-run.sh`, `tools_test`)
- **All work through `ld-musl-x86_64.so.1 --library-path ...`.** That covers `qemu-system-x86_64` 11.0.3, `mke2fs`, `mkfs.xfs`, `mkfs.btrfs`, `xfs_repair`, `e2fsck`, util-linux's `mkswap` and `mkfs.minix`. `busybox.static` runs natively. Running a static binary through the loader segfaults.
- **KVM works** (`-M microvm,accel=kvm -cpu host`; boots with `/dev/kvm`).
- **Wrapper needs for QEMU.** `/proc/self/exe` is the loader, so QEMU's relocation breaks. The wrapper must export `QEMU_MODULE_DIR` and pass `-L <root>/usr/share/qemu`. `run-qemu.sh` passes `-bios`, so the boot did not need `-L`, but `-M microvm` alone does.
- **qboot.** The qboot.rom shipped in the QEMU apk is byte-identical to ours (sha256 `9b9dfc6c...`).
- **Guest boot with Alpine QEMU and mkfs.** Our kernel and initramfs boot under Alpine's QEMU with Alpine's mkfs tools, 6 of 6, 10 PASS each (wall about 4.4 s on a loaded host, similar to ours).
- **Absolute symlinks.** Absolute symlinks in apks are rewritten to relative, otherwise Bazel's `glob` rejects them. The closure globs fine as a runfiles tree.
- **Alpine QEMU has about 451 devices.** Our pinned build is minimal.
- **Sizes.** The Alpine closure is 84 MB (apks 27 MB) against our 11 MB `qemu-system-x86_64`.
- **Pin differences.** Alpine's QEMU is 11.0.3 against our 11.1.2.
- `tools_test` passes (qemu version, `kvm` accelerator listed, KVM start, mke2fs, mkfs.xfs, mkfs.btrfs, busybox).

## 4. systemd
- **No systemd in Alpine.** There is no systemd package in v3.24 main/community or in edge main/community/testing. The only hits are `systemd-boot` and `systemd-efistub` (260.2, origin `systemd-boot`) and unit-file subpackages named `*-systemd`.
- **postmarketOS.** postmarketOS ships its musl-patched systemd (v26.06: 261-r0, master: 262-r4) in `mirror.postmarketos.org/postmarketos/extra-repos/systemd/`. It has a different signer, mirror layout and trust anchor. It is not Alpine, and not what a Debian user runs.
- **Conclusion.** The systemd guest stays Debian, as the plan says.

## 5. Drift and SBOM
- **Update cadence.** On aports `3.24-stable` since 2026-05-01, `main/linux-lts` has 37 commits, about 1.7 per week. `community/qemu` has 2 and `main/xfsprogs` has 2. Expect a new kernel roughly weekly, and the other tools almost never.
- **What breaks between dev and CI.**
  - They get different kernels and modules and rebuild everything downstream. That is correct but surprising.
  - Mixing a kernel from one run with modules from another is broken. I booted 6.18.55 with 6.18.54 modules: `modprobe` found no module directory and the guest failed vdb-present and vdb-mount. Kernel and modules always come from the same apk, so mixing happens only across repos.
  - `uname -r` and the module directory change per release, so anything that hard-codes them breaks.
- **OSV.**
  - The OSV v1 API's `pkg:apk/alpine/...` purl returned 0 vulns for four purl variants. The ecosystem form (`Alpine:v3.17`, name, version) returned 30 for `openssl@3.0.8-r0`.
  - `osv-scanner` v2.6.0 (downloaded into the lane) does match `pkg:apk/alpine/<origin>@<ver>?arch=x86_64&distro=alpine-3.17`. An SBOM with openssl 3.0.8-r0, busybox 1.35.0-r17 and linux-lts 5.15.50-r0 gave 40 vulns (Alpine:v3.17).
  - **Component names must be the origin package** (`o:` in the index). `linux-virt` returns "No issues found", a silent false negative, while `linux-lts` matches. Likewise `qemu` is the origin of `qemu-system-x86_64`, and `busybox` is the origin of `busybox-static`. `LOCK` records `origin:`.

## Recommended design for the real step
1. **Keep the rule.** Move the index into a shared index repo, or a module extension that resolves once and writes the picked versions, to remove the cross-repo race. Keep `alpine.lock` plus the manual `drift_test`, and have a scheduled CI job run it so Alpine updates fail as their own job. Keep `tools_test`.
2. **Kernel.** Use linux-virt plus a per-test module list, with `.ko` decompressed at initramfs build time and modules loaded in `guest/init`, after adding modprobe/insmod to our busybox fragment or switching to Alpine's busybox-static. Relax `boot.sh`'s identity checks to the series (`6.18.*-virt`).
3. **Host tools.** Use QEMU, mkfs tools and busybox from the apk closures through a small wrapper (loader, `QEMU_MODULE_DIR`, `-L`). Our own build is lighter (11 MB against 84 MB), so this swap only pays off if dropping the QEMU and fs-tools source builds matters more than size.
4. **Escape hatch.** Pair every pin with a mirror URL we control, because Alpine drops superseded files within days. Keep the expiry test.
5. **Drop dm-dust** from 11.3 in favour of `error` and `flakey`.
6. **SBOM.** Feed `alpine.lock` into the SBOM as `pkg:apk/alpine/<origin>@<ver>?distro=alpine-3.24`. Add a test that no component is a binary subpackage name.
7. **Series choice.** The series has to be a branch's current kernel. 6.18 is in v3.24, and series pins carry a branch-EOL risk.

## Deviations and loose ends
- The scripts under `third_party/alpine/spike/` use the lane's absolute paths. They are measurement harnesses, not build code.
- **Untracked scratch.** `<lane>/spike-work/` holds about 1.3 GB (indexes, kernel dev tree, gcc root, artifacts, `osv-scanner`). It is safe to delete.
- **Not tested.** I did not check refetch behaviour across a Bazel server restart. I did not run the full e2e suite on the Alpine kernel, only `boot`, `lifecycle` and `passthrough`.
- Version ordering in `apk.py` is an approximation of `apk_version_compare`.
- The host load made all absolute timings noisy. I report min and median plus CPU time.
- **Main-checkout check.** I ran no Bazel command in the main checkout, and the Bazel server in lane-2 is still running.
