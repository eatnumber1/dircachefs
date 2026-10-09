# third_party/debian_cloud: the released Debian cloud image of the systemd guest

Step 15.6 (`docs/plan/phases/15-mount-dcfs-wrapper.md`, russ, 2026-10-07: a
released image fetched by its published checksum, not an image we build).
`//test/qemu:mount_dcfs_systemd_test` boots it with systemd as PID 1 to run
`mount.dcfs` through util-linux's `mount(8)`, fstab and systemd's mount units
(`test/qemu/README.md`, "The systemd guest"). This directory holds only the
alias that names the download and this note; the download is the `http_file`
`debian_cloud_image` in `MODULE.bazel`.

## The pin

- **Image**: `debian-13-nocloud-amd64-20261001-2618.qcow2`, Debian 13
  ("trixie", the current stable release), build `20261001-2618`, from
  `https://cloud.debian.org/images/cloud/trixie/20261001-2618/`. 388 MiB
  qcow2, 3 GiB virtual, a GPT disk whose first partition is the ext4 root
  filesystem (systemd 257, util-linux 2.41, e2fsprogs 1.47; no xfsprogs,
  btrfs-progs or strace).
- **Checksum**: SHA512
  `456f244f66bdfad45be8c1378630effac0ea2a67e826c99c280edb8a40b747b73aa67057fa39130aacd53df82a662683e06238667fd1f15bbda581c02383eb46`,
  the one Debian publishes in `SHA512SUMS` beside the image (checked by hand
  on 2026-10-09 against a `curl -L` download and `sha512sum`), written in
  `MODULE.bazel` as `integrity = "sha512-<base64>"`. Its SHA256, computed
  the same way on the same day, is
  `a593e5496eb07625a115f44cda38ad68df783f4684f295067527fa74fd034239`.
  Bazel refuses a file with another hash, so a changed or truncated image
  fails the fetch, not a test.
- **Why `nocloud`**: the `genericcloud` image carries cloud-init, which
  waits for a datasource on a machine with no network and no metadata
  service (a guest here has neither). `nocloud` has no cloud-init and
  everything else the same: the same packages from the same release, systemd
  as init. The `generic` flavor adds a kernel with drivers for real
  hardware this guest does not have, and the kernel is not the image's
  anyway (below).
- **The kernel is not the image's.** The guest boots the project's test kernel
  (Alpine's `linux-virt`, `third_party/linux`) and switches root into the
  image's userland (`test/qemu/guest/init`, the `dcfs_systemd=` branch). The
  image's own kernel is 6.12; dcfs asks for `FS_IOC_GETFSUUID` (6.13), which
  every other test already runs on, and the microvm has no bootloader to
  start the image's kernel with anyway. What the test proves about systemd,
  util-linux and journald does not depend on the kernel; what it proves about
  dcfs is on the same kernel as the rest of the suite.

The image is never written: `run-qemu.sh --systemd-image` puts a qcow2
overlay (`qemu-img` from `@alpine_qemu_img`, Alpine's package like QEMU
itself) in the test's own directory, so a test run changes nothing in
Bazel's output base.

The SBOM (`tools/sbom/pins.json`, `debian_cloud_image`) carries the image as
one test-only `pkg:generic` component: its packages are not enumerated and
OSV does not scan them. The image is used only inside a guest with no
network, and nothing in it is shipped.

## What the test changes in it

`test/qemu/guest/systemd_install.sh`, run by `guest/init` on the mounted
root partition before `switch_root`: a oneshot unit that runs the test
and prints the verdict on the serial console; the getty templates masked
(the nocloud image logs root in on the serial console without a password;
root's password stays locked); networkd, resolved, timesyncd, the apt
timers, unattended-upgrades masked (nothing in the guest waits for a
network or a schedule); a machine-id; an empty `/etc/fstab` for the test to
fill; `DefaultTimeoutStopSec=15s`. dcfs, `testutil` and `fhtest` are copied to
`/usr/local/bin` and linked as `/sbin/mount.dcfs` and `/sbin/mount.fuse.dcfs`.

## Moving the pin

1. Pick a build from `https://cloud.debian.org/images/cloud/trixie/` (a
   dated directory, not `latest/`, which moves). Debian keeps only the
   latest builds: a pin goes stale within weeks to months, when its directory
   is removed, and the fetch then fails with a 404; moving the pin is the
   fix.
2. Take the `debian-13-nocloud-amd64-<build>.qcow2` line of that
   directory's `SHA512SUMS`. Convert the hex to base64 for `integrity`:
   `python3 -c 'import base64,sys; print(base64.b64encode(bytes.fromhex(sys.argv[1])).decode())' <hex>`.
3. Download it once into a fresh directory (`curl -L -O <url>`, the
   server redirects to a mirror), check `sha512sum` against the published
   line, and record the date and the SHA256 here.
4. Edit the URL and `integrity` in `MODULE.bazel` and the version in
   `tools/sbom/pins.json` (`debian_cloud_image` takes it from the URL), run
   `bazel test //test/qemu:mount_dcfs_systemd_test //tools/sbom:sbom_test`.
5. A new Debian release (14) changes more than the date: its util-linux and
   systemd are what the test then covers; read the log of the first run.

On a host whose IPv6 is unreachable, Bazel's Java downloader can time out
connecting to `cloud.debian.org` (curl falls back to IPv4, Java does not):
download the file with curl into a directory and pass
`--distdir=<directory>`, or start Bazel with
`--host_jvm_args=-Djava.net.preferIPv4Stack=true`.
