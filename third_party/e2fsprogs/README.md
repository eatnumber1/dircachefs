# e2fsprogs: the mke2fs profile

mke2fs and debugfs are Alpine's e2fsprogs and e2fsprogs-extra packages
(`@alpine_fstools`, `third_party/alpine/README.md`), run through musl's
loader by the wrappers that repository writes. Until Phase 24 this directory
built e2fsprogs and libarchive from source; only the profile stays.

## `mke2fs.conf`

Every filesystem the tests make with mke2fs (the scratch disks by
`run-qemu.sh`, the Debian rootfs by `third_party/debian/scripts/mkrootfs.py`)
runs with `MKE2FS_CONFIG` pointing at this file, so the feature set never
depends on the host's `/etc/mke2fs.conf` or on a default of the build. It
is e2fsprogs 1.47's own profile written out, with 4 KiB blocks at every size
(the built-in `small` and `floppy` types would give 1 KiB below 512 MiB and
3 MiB; a test filesystem should resemble a real disk). The ext4 features are
`has_journal,extent,huge_file,flex_bg,metadata_csum,metadata_csum_seed,
64bit,dir_nlink,extra_isize,orphan_file`.

`//third_party/e2fsprogs:mke2fs_conf_test` makes images with it and checks
the features (so changing the profile needs the test's expectation changed
too), the block size at 2 MiB, 64 MiB and 256 MiB, and that the environment
variable, not a baked-in path, decides.

**Licence.** The profile is e2fsprogs's `misc/mke2fs.conf.in` apart from a header
comment of ours and one change (the 4 KiB `small` and `floppy` blocks), so it is derived from
e2fsprogs, not ours to license as MIT. Debian's `copyright` file for
e2fsprogs 1.47 gives `GPL-2` for `Files: *`; the file has no header of its own
and is not code the SPDX gate covers, so `tools/third_party_files.txt` lists
it as `GPL-2.0-only`, test-only. It is read by `mke2fs` in the test guests and
never shipped (`docs/licenses.md`).

## Why the Debian image is not made with `mke2fs -d <tarball>`

e2fsprogs 1.47.1 added tarball support to `mke2fs -d` through libarchive;
Alpine builds without it (`mke2fs -d x.tar` says "you need to compile
e2fsprogs without --without-libarchive"). `mkrootfs.py` therefore unpacks the
tar itself, uses `mke2fs -d <directory>` and corrects ownership and modes
with `debugfs` (`third_party/debian/README.md`, "Ownership").
