# Phase 15 — The mount.dcfs wrapper: one instance per backing filesystem

**Decisions (russ, 2026-10-04).**
1. One dcfs process per backing filesystem (each submount and each btrfs
   subvolume), so one filesystem's cache can be wiped or its daemon
   restarted without touching the others. Each instance needs exclusive
   access to what it serves; keeping it so (no other writers, no two
   instances over the same objects, no out-of-band changes to a network
   export) is the administrator's responsibility. dcfs does not try to
   detect overlapping instances.
2. Submounts and btrfs subvolumes are both "boundaries", detected by
   `IsBoundary` (different st_dev or different mount id).
3. A parent instance presents each directory boundary as a **stub**: a
   visible directory usable as a mount point. Anything inside it (lookup,
   readdir, open, create) gets ENOTSUP with a one-time ERROR log;
   rename/link across it gets EXDEV (what the backing filesystems return).
4. Losing a parent mount takes its children with it; that is acceptable.
   No /dev/fuse fd persistence across restarts.
5. dcfs is mounted through a wrapper, from fstab (with or without systemd)
   or the command line, the same way. fstab type `dcfs`; the backing kind
   is the option `dcfs.fstype=`.
6. Options are not shared: every option goes to the underlying mount
   except those prefixed `dcfs.`, which go to dcfs (`ro` makes the backing
   mount read-only; `dcfs.ro` makes the dcfs mount read-only).
7. A helper prints example `/etc/exports` lines; it does not edit
   `/etc/exports` or run `exportfs`.
8. The plain `dcfs --source=DIR` command line goes away;
   `dcfs.fstype=none` (below) replaces it, tests included. There is no
   recursive bind: each filesystem needs its own instance.
9. File mount points (the kernel allows a file mounted on a file,
   `graft_tree`) are rejected: at mount time if present, ENOTSUP on lookup
   if one appears later.
10. `mount -o remount` changes only the dcfs mount (e.g. `dcfs.ro` toggles
    read-only via the FUSE mount's flags). To remount the underlying
    filesystem, use `dcfs.fstype=none` on a mount the administrator manages
    (not over-mounted, so it stays reachable).
11. The FUSE mount's source (mountinfo/`df`/`findmnt`) is the original spec
    as written, e.g. `UUID=aaaa` or `nas:/export`.
12. dcfs refuses to run as non-root with an error saying why; README
    documents the reasons: FUSE passthrough needs CAP_SYS_ADMIN
    (fs/fuse/backing.c), as do the private mount namespace, `open_tree`,
    xfs bulkstat and the filesystem-identity ioctls; `open_by_handle_at`
    needs CAP_DAC_READ_SEARCH; acting with each caller's credentials needs
    setfsuid/setfsgid/setgroups. fstab's `user` option therefore fails.
13. Own daemonization, not `fuse_daemonize` (which only forks, waits for the
    child's setsid/chdir/stdio redirect and always exits 0).
14. Test under real systemd: a Debian guest that boots systemd as PID 1.

**Design.**
- **Invocation.** The dcfs binary, installed as `/sbin/mount.dcfs` (argv[0]
  dispatch); `mount(8)` runs it for type `dcfs` as `mount.dcfs SOURCE
  MOUNTPOINT [-sfnv] [-N ns] -o OPTIONS`. Examples:
  ```
  UUID=aaaa    /data       dcfs  noatime,dcfs.allow_other           0 2
  UUID=bbbb    /data/sub   dcfs  dcfs.fstype=xfs                    0 2
  UUID=cccc    /data/vol   dcfs  dcfs.fstype=btrfs,subvol=vol       0 0
  nas:/export  /srv/nas    dcfs  dcfs.fstype=nfs,vers=4.2,_netdev   0 0
  /srv/raw     /cache/raw  dcfs  dcfs.fstype=bind,dcfs.ro           0 0
  /mnt/disk    /mnt/fast   dcfs  dcfs.fstype=none                   0 0
  ```
  Command line: `mount -t dcfs -o noatime /dev/sdb1 /data`.
  `dcfs.fstype` values: absent = let `mount(8)` autodetect the native type
  (as plain `mount` does); a native type (`ext4`, `nfs`, `fuse.sshfs`,
  ...); `bind`; `none`.
  systemd does not know a `dcfs` mount of NFS is a network mount: network
  backings need `_netdev` (documented). Mount units under a path require
  and order after the mount above them, so parents mount first and
  stopping a parent stops its children; restart one instance with
  `systemctl restart`. Without systemd: `mount -a`, `umount -R /data`,
  `umount /data/sub && mount /data/sub`.
- **fsck.** `fsck.dcfs` (argv[0] dispatch too): probes the device with
  blkid and execs `fsck.<type>`, so pass number 2 keeps boot-time fsck
  (systemd-fsck and `fsck -A` both look up `fsck.<fstab type>`). Non-block
  sources: exit 0 (nothing to check).
- **Daemonization and readiness.** `mount.dcfs` forks first, before any
  threads, the database or the mount. The child does all the work (capture,
  identity check, cache, FUSE mount) and calls setsid and chdir("/").
  Logging: one path, syslog, the convention of FUSE filesystems that
  daemonize (gocryptfs, s3fs, ntfs-3g log to syslog once in the
  background). An Abseil `LogSink` sends every message to `syslog(3)`;
  Abseil's own stderr output is silenced. Under systemd, journald owns
  `/dev/log` and attributes each message to the sender's cgroup, and dcfs
  stays in its mount unit's cgroup, so `journalctl -u data.mount` shows it
  with no systemd-specific code. `dcfs.foreground` (debugging, tests) skips
  the fork and opens syslog with `LOG_PERROR`, which copies each message to
  stderr: same code, one flag. Without a syslog daemon (minimal systems),
  messages are dropped; documented. It
  reports "ready" through a pipe from the FUSE `init` callback (the kernel
  sends FUSE_INIT at mount time), so ready means dcfs is serving; any
  earlier failure is reported through the same pipe as error text plus an
  exit status. The parent prints it and exits with that status (`mount(8)`
  exit codes). Not `sd_notify`: systemd mount units do not use it (no
  NOTIFY_SOCKET for mount helpers); a mount unit is done when the helper
  exits, which the pipe makes accurate.
- **Capturing the backing tree** (`dcfs.fstype` absent, a native type, or
  `bind`; works for any type, block, network or FUSE, with no per-type
  code):
  1. Split options: `dcfs.X` → dcfs option X (each existing dcfs flag
     becomes `dcfs.<flag>`, plus `dcfs.fstype`, `dcfs.ro`,
     `dcfs.cache_dir`, `dcfs.cache_db`); unknown `dcfs.` options are an
     error. Everything else is passed through verbatim.
  2. A helper process unshares its mount namespace (`CLONE_NEWNS`, all
     mounts made private), mounts a tmpfs for a staging directory, and runs
     the native `mount [-t <native>] -o <native options> SOURCE <staging>`
     (`mount --bind` for `bind`), so `mount(8)` and the native helper
     (`mount.nfs`, `mount.cifs`, sshfs, ...) do tag resolution, flags and
     everything else.
  3. It clones the result with `open_tree(OPEN_TREE_CLONE)` (not
     recursive) and sends the fd back over a socketpair (`SCM_RIGHTS`), then
     exits; its namespace and the staging mount die with it. Nothing appears
     in the caller's namespace and nothing leaks if anything crashes. dcfs
     only walks and decodes handles through the fd (root has
     CAP_DAC_READ_SEARCH, `may_decode_fh`); it never clones it again.
  4. dcfs derives a real directory fd (`openat(fd, ".", O_RDONLY |
     O_DIRECTORY)`, history.md amendment 7), checks identity support (Phase 14 (Identity from the backing filesystem): an
     unsupported handle format is refused with a clear error), opens the
     cache, mounts FUSE at MOUNTPOINT. The detached backing mount dissolves
     when dcfs exits. Over-mounting is not a separate case: the backing is
     captured before the FUSE mount.
  - The native mount's stderr and exit status are passed through on
    failure; every failure leaves nothing mounted. `-f` mounts nothing.
  - Unmounting is plain `umount`: dcfs exits when its FUSE mount goes away.
- **`dcfs.fstype=bind`**: SOURCE is captured with a non-recursive bind, so
  submounts are not in the capture and dcfs does not hold their
  filesystems. At capture time the wrapper reads the caller's
  `/proc/self/mountinfo` and records the mounts whose parent mount is
  SOURCE's mount (statx `STATX_MNT_ID`) with a mount point under SOURCE
  (for stacked mounts the bottom one counts; deeper ones are inside an
  excluded mount). dcfs resolves each recorded path inside the capture
  (component-wise `openat`, `O_NOFOLLOW`) and records it as a stub at
  startup, so it never serves the directory a submount covers. Stubs from a
  previous run that are no longer recorded revert to unknown and are
  re-probed (tri-state rule). btrfs subvolumes are found by `IsBoundary`
  (st_dev). A child whose SOURCE is covered by a parent's dcfs must use a
  native type (refused with a clear error).
  **Documented behavior** (README): submounts present when the instance
  mounts are stubs; mounts added or removed below SOURCE later are not
  seen until remount (until then a later mount's covered directory is
  served and a removed mount's stub stays); file mount points are refused;
  dcfs does not keep submounts' filesystems busy.
- **`dcfs.fstype=none`**: no capture. dcfs opens SOURCE in the caller's
  namespace (before its FUSE mount, so SOURCE == MOUNTPOINT still works)
  and serves the live tree, like today's `--source`. The administrator
  owns SOURCE's mount: it stays in place, can be remounted with its own
  options, and is kept busy while dcfs runs. Submounts are seen live by
  `IsBoundary` (mount id) and become stubs whenever they appear; a file
  mount point found at runtime gets ENOTSUP on lookup (`IsBoundary` must
  also flag non-directories on another filesystem, which today it treats
  as ordinary entries). The mount-time file-mount check reuses the bind
  form's mountinfo scan.
- **Instance identity.** `<instance-id>` = hex of a hash of the backing
  `DeviceId` (filesystem UUID, plus subvolume id on btrfs) and the backing
  root's file handle. Cache DB default: `<dcfs.cache_dir>/<instance-id>.db`
  (default `/var/cache/dcfs`). NFS `fsid` = first 16 bytes of
  SHA-256("dcfs" || instance identity), formatted as a UUID: stable across
  restarts and (after Phase 14 (Identity from the backing filesystem)) cache wipes, distinct from the backing
  filesystem's own UUID.
- **Boundaries at startup:** the mountinfo refusal (`mounts_below`, from the original plan's step 4.8)
  becomes an INFO log listing each boundary with a suggested fstab line.
  `StartupPurge` (other filesystems' rows) is obsolete with one filesystem
  per cache DB: removed.
- **Stubs:** the persisted "refused" dentry becomes a stub (decision 3),
  with a nodeid from the reserved range at or above 2^63 (Phase 14 (Identity from the backing filesystem)),
  persisted with the dentry so it is stable across restarts.
- **Exports helper:** `dcfs exports MOUNTPOINT [--clients=SPEC]
  [--options=...]` reads mountinfo and prints one line per dcfs instance at
  or under MOUNTPOINT, for example
  ```
  /data      CLIENTS(rw,no_subtree_check,crossmnt,fsid=<uuid>)
  /data/sub  CLIENTS(rw,no_subtree_check,fsid=<uuid>)
  ```
  `crossmnt` on the top export lets NFSv4 clients walk into the children.
- `packaging/dcfs.service` is removed; README documents fstab and an
  equivalent hand-written systemd `.mount` unit.

**Further decisions (russ, 2026-10-04).**
15. The cache directory must be mounted before any dcfs instance that uses
    it. The wrapper fails clearly (naming the directory) if it is missing;
    README documents `x-systemd.requires-mounts-for=<cache_dir>` for fstab
    lines whose cache directory is on another mount.
16. Where the cache lives is the administrator's choice (dcfs does not
    check whether it is on a spinning disk or on the backing filesystem).
17. The cache is unbounded by design: no eviction, no size limit. README
    says so.
18. No installer. README gets an installation section: build, copy the
    binary, create the `mount.dcfs` and `fsck.dcfs` links, cache directory.
19. README frames dcfs for any high-latency backing (spinning disks that
    spin down, network filesystems), not only spinning disks.
20. Subtree escape through guessed NFS handles for a subdirectory root
    (`bind`/`none`) is accepted, like knfsd's `no_subtree_check`; not
    addressed.
21. Stubs keep synthetic nodeids in every form (one rule). README documents
    the caveat: after a cache wipe, NFS handles to a stub directory get
    ESTALE (clients normally hold handles to the child export, not the
    stub).

22. Kernel feature checks at startup: before anything else, dcfs probes
    every kernel feature it needs (FUSE passthrough, `open_tree`,
    `fsopen`/`fsmount`, `STATX_MNT_ID_UNIQUE`, `FS_IOC_GETFSUUID` or the
    btrfs fallback, `open_by_handle_at`) and fails with one message naming
    the missing feature and the minimum kernel. README's requirements
    section lists the minimum kernel. Tests: link-time `-Wl,--wrap` fakes
    make each probe fail in a unit test (the guest kernel has everything).

23. `allow_other` is on by default, so a dcfs mount is usable by every
    user like the native mount it replaces (permission checks stay with
    the kernel's `default_permissions`, which dcfs already sets).
    `dcfs.allow_other=0` turns it off.
24. `--version` (and `mount.dcfs -V`, the mount-helper convention) prints
    the dcfs version and git revision, stamped at build time through
    Bazel's workspace status (`--stamp`, a `workspace_status_command`
    running `git describe --dirty`); dcfs logs the same line at startup.
25. SELinux is documented, not supported specially (russ does not use it):
    a FUSE mount gets one SELinux label for every file (the policy's
    default for FUSE, usually `fusefs_t`, or one given with the `context=`
    mount option on the dcfs mount), so per-file labels on the backing
    files are not what the kernel enforces, and policy written for the
    backing paths will not apply. README says this is untested and
    points at the `context=` option; verify the option's spelling through
    the wrapper while writing it.
26. A man page, `dcfs(8)` (with `mount.dcfs` and `fsck.dcfs` as names in
    it), generated from the README's usage, fstab and operations sections
    by a pinned pandoc (its static release binary fetched by Bazel with
    `http_archive` and sha256; `third_party/pandoc/` holds only the
    BUILD overlay) in a Bazel genrule; a test renders it with
    `groff -man -ww -z` and fails on warnings. README's installation
    section says where to copy it. **Pulled forward (russ, 2026-10-07)**
    as step 15.0, independent of the wrapper: pandoc and the genrule now;
    groff for the render test comes from the Phase 24 `alpine_package`
    rule (until then, the check is pandoc reading its own man output back
    with `-f man`, plus a grep for the required sections). The page
    documents today's `dcfs` flags and gains the wrapper names later.
    Alongside it: `dcfs --help` under the installed name printed "No
    flags matched" (Abseil shows only flags from files named after the
    program); fixed with a `FlagsUsageConfig`, with `--version`.

**15.1 Tests first (QEMU; must fail on today's main; quote the output).**
fstab tests run in the Debian rootfs (util-linux `mount`); systemd tests in
a Debian guest booting systemd; the rest may use the busybox guest with
`mount.dcfs` invoked directly. Fixtures: an ext4 image with an xfs image
for `sub/`; a btrfs image with a subvolume containing a nested subvolume; a
guest-local NFS export (from the existing NFS test setup).
- Capture, per filesystem, with `dcfs.fstype` absent and explicit: the files
  are served; mountinfo shows the `fuse.dcfs` mount with the spec as its
  source and no backing mount anywhere in the caller's namespace; `umount`
  stops dcfs; after SIGKILL of dcfs and a lazy unmount, the backing
  superblock is released (the loop device detaches).
- Options: `ro` → backing read-only (writes through dcfs get EROFS, the dcfs
  mount itself is rw); `dcfs.ro` → dcfs mount read-only; `mount -o
  remount,dcfs.ro` and back; `dcfs.bogus` → non-zero exit, clear message,
  nothing mounted; a native mount failure (wrong type, missing device) →
  the native error text and its non-zero exit, nothing mounted; `-f` mounts
  nothing; `UUID=` sources resolve.
- Daemonization: `mount.dcfs` exits only after dcfs answers FUSE_INIT (a
  `stat` of the mount point right after it returns succeeds without
  waiting); a failure after the fork (cache DB locked with `testutil
  sqlite-lock`) is reported by the parent with a non-zero exit, nothing
  mounted; a daemonized dcfs logs to syslog (busybox `syslogd` in the
  guest) and its stdio is /dev/null; with `dcfs.foreground` the same
  messages also reach stderr.
- `dcfs.fstype=bind`: over the same path (over-mount) and to another path,
  same results. Submounts (each with a file planted in the directory it
  covers):
  - present at mount time: a stub, the planted file never visible; a
    submount nested inside it is not recorded;
  - unmounted elsewhere while dcfs runs: its filesystem is released, the
    stub stays until remount, then becomes a plain directory again;
  - added after mount: the covered directory is served (documented); after
    a remount it is a stub;
  - a file mounted below SOURCE: mount fails with an error naming it.
  A bind-form child under a bind-form over-mount is refused.
- `dcfs.fstype=none`: serves the live tree at another path and over SOURCE;
  a submount added while dcfs runs becomes a stub without a remount; a file
  mounted below SOURCE while running gets ENOTSUP on lookup, and one
  present at mount time fails the mount; `mount -o remount,ro SOURCE`
  makes writes through dcfs fail with EROFS.
- Stubs, on each fixture: readdir lists the boundary, `stat` shows a
  directory, `ls b/` and `touch b/x` fail with ENOTSUP, `mv f b/` and
  `ln f b/x` fail with EXDEV, unchanged after a dcfs restart.
- Trees from fstab with `mount -a`: every file reachable through the right
  instance; each instance has its own st_dev, process and cache DB; NFS
  handles from each instance work; `umount -R` leaves no dcfs mounts or
  processes; `umount /data/sub`, delete its cache DB, `mount /data/sub`: the
  parent's open files keep working and the child comes back cold.
- fsck: `fsck /dev/<ext4 image>` with a `dcfs` fstab line runs e2fsck (an
  injected inconsistency is reported); `fsck -A` covers pass-2 lines.
- Real systemd (Debian guest booting systemd): the fstab tree mounts at boot
  in order; boot-time fsck runs for the backing devices; `systemctl
  restart` of a parent restarts its children; stopping a child leaves the
  parent; dcfs's log reaches the journal attributed to the mount unit
  (`journalctl -u data.mount`); a startup failure marks the mount
  unit failed with dcfs's message in the journal; at poweroff every
  instance exits cleanly (next start reports a clean shutdown, no
  dirty-set recovery).
- Network backing: `dcfs.fstype=nfs` reaches dcfs's identity check and is
  refused cleanly (NFS handles are not a supported format yet), nothing
  left mounted.
- Non-root: `mount.dcfs` exits non-zero with the documented reason.
- `allow_other` default: an unprivileged user can read a world-readable
  file through a fresh dcfs mount; with `dcfs.allow_other=0` they cannot
  reach the mount at all.
- `--version` / `-V` prints a version and the git revision of the build;
  the startup log line matches.
- Missing cache directory: the mount fails with an error naming it,
  nothing mounted; in the systemd guest, a cache directory on its own fstab
  mount plus `x-systemd.requires-mounts-for` mounts in the right order at
  boot.
- Instance identity: two instances on different directories of one
  filesystem get different cache DBs and fsids; a cache wipe leaves the
  fsid unchanged.
- `dcfs exports`: lines parse with `exportfs -o`; an NFSv4 client reads both
  instances of a two-instance tree through one mount (crossmnt).
- Unit: option splitting, `dcfs.fstype` parsing, instance id and fsid
  derivation, mountinfo scan (parent-mount filter, stacked mounts, escaped
  paths), exports lines.

**15.2 Wrapper and daemonization** (argv dispatch, option split, capture via
private namespace and fd handoff, `none`, error pass-through, helper flags,
fork-first daemonization with pipe readiness, syslog logging), removing `--source`
and `StartupPurge`; switch `guest/lib.sh` and every QEMU test to
`dcfs.fstype=none` in the same step so the suite stays green. Owner: Opus
(namespaces, root, failure paths).
**15.3 Instance identity, cache path, fsid; mount source = spec.** Owner:
Sonnet.
**15.4 Stubs** (after Phase 14 (Identity from the backing filesystem)): stubs, ENOTSUP/EXDEV, the bind form's
recorded mount points and reverting stale ones, non-directory boundaries.
Owner: Opus.
**15.5 `fsck.dcfs` and `dcfs exports`.** Owner: Sonnet.
Refined (russ, 2026-10-09: "add support for the sixth field of fstab, so
you can do fsck via fstab on a filesystem that's set up via dcfs").
`fsck -A` and systemd's `systemd-fsck@<dev>.service` run `fsck.<type>
<device> <flags>` before mounting a line whose passno is non-zero, so
dcfs installs `fsck.dcfs` by the argv[0] dispatch like the other helpers.
It receives only the device and fsck's flags, so it finds its fstab line
(and the `dcfs.` options) through util-linux (`findmnt --fstab`, as a
child like `/bin/mount` in the capture), then: (1) for a native
`dcfs.fstype`, runs that type's fsck on the device with the flags passed
through (`-a`, `-p`, `-n`, `-y`, `-f`, `-C`) and relays its exit status,
so passno 2 on a dcfs line means what it means on a plain line; for
`none` and `bind` there is no device to check: say so, exit 0; (2) checks
dcfs's own cache database: SQLite `integrity_check`, the schema version,
that no daemon holds it, the dirty set's sanity (dirty rows name existing
inodes; a clean-shutdown flag consistent with an empty dirty set); with
`-y`/`-a`/`-p` a corrupt or unreadable cache is rebuilt (it is a cache:
deleting it costs a cold start, say so on stderr), with `-n` only
reported; exit statuses per fsck(8) (0, 1 corrected, 4 uncorrected, 8
operational error, 16 usage, 32 cancelled), combined with the backing
fsck's as fsck(8) combines them. No timers (russ's rule): a held database
is reported, not waited for, unless the caller asked to wait. README's
fstab examples get a real passno (2) where the backing is a device, the
15.7 sentence about pass 0 goes, and the systemd guest asserts: a
passno-2 line is checked at boot before its mount (journal shows
`systemd-fsck@` running `fsck.dcfs`, then the mount unit), a corrupt
cache is reported with `-n` and rebuilt with `-y`, a `none` line's fsck is
a no-op, exit statuses as fsck(8) reports them; mount_dcfs.sh covers the
busybox path. Design decisions in design.md (why the backing's fsck is
delegated, why the cache is rebuilt not repaired). `dcfs exports` stays
in 15.5 as before. Owner: dcfs-implementer, after 15.6b merges (same
wrapper code and systemd test); under the budget throttle, in the first
of the three lanes to free after that.
**15.6c The Debian image fetch fails over IPv6 (CI run 38009667624, 2026-10-10).**
The first push after 15.6 merged failed in the fast job before any test
ran: Bazel could not fetch `@debian_cloud_image` ("Connect timed out").
Verified: cloud.debian.org times out over IPv6 and answers over IPv4
(curl -6 nothing, curl -4 a 302 to a mirror); Bazel's Java downloader
prefers IPv6; the 15.6 agent saw it locally and worked around it with
`--distdir`. Fix (dispatched, dcfs-mechanical, lane-5): `startup
--host_jvm_args=-Djava.net.preferIPv4Stack=true` in .bazelrc (every server,
local and CI; check nothing we fetch is IPv6-only) and a `urls` list for
the image (cdimage.debian.org canonical, cloud.debian.org, one stable
mirror; same integrity). Every job behind fast was skipped on that push:
the coverage fix and the toolchain cache are still unverified in CI.

**15.8 `allow_other` always on (russ, 2026-10-09).** russ: "Does it ever
make sense not to pass dcfs.allow_other? If no, should we just always
pass it internally (and require it not be present in fstab)?" No: FUSE's
mounter-only default guards against an unprivileged daemon serving
fabricated data to other users; dcfs runs as root and requires
`default_permissions`, so the kernel enforces the mode bits on every
access, and `allow_other` only lets non-root users and nfsd reach the
mount at all. A root-only mount is a root-only mode on the directory.
So: dcfs passes `allow_other` and `default_permissions` to the kernel
itself, always; `dcfs.allow_other` and a bare `allow_other` in a dcfs
line are REFUSED (exit 1, usage) with a message that dcfs always allows
other users and the option should be removed (strict, so stale lines are
noticed); README examples, man page, design.md and every test fixture and
guest wrapper (15.6's fstab lines, 17.1's mount wrapper, nfs.sh) drop the
option; a test that the option is refused, failing first; mount.dcfs's
non-root refusal unchanged. Owner: dcfs-implementer, together with 15.5
or in the lane that frees after 15.6b merges (same option parser).

**15.6 systemd guest** (russ, 2026-10-07: a RELEASED cloud image fetched by
its published checksum, not an image we build: Debian 13's nocloud image
(kernel 6.12, has FUSE passthrough) or Ubuntu 26.04's if a newer kernel is
wanted; the harness extracts the image's kernel and initramfs for direct
boot, since the microvm has no bootloader) (Debian rootfs booting systemd in the QEMU harness)
(its prerequisite, a root-owned Debian image, was done in Phase 4c:
Bazel-built e2fsprogs builds the image from the root-owned package tar)
and its tests. Owner: Sonnet.
**15.6 status 2026-10-09:** merged 4fd3136 (lane-6, four commits; one
conflict in qemu_test.bzl with 26.14's `cpus`; nothing in the image resets
the quiet-kernel sysctls, a check asserts it under systemd; of 15.7's nine
systemd statements six asserted, two partly, `systemctl restart`
contradicted and the README fixed; the test's waits are event-based: a
python3 pidfd `select` with no bound, `journalctl --sync`, a fifo; two
polls left for 15.6b, lib.sh's `quiesce_daemon` and the sampler's
first-line wait in systemd_run.sh; the README's interim restart recipe is
`umount`, `flock <cache db> true` with no bound, `mount`). As built: Debian
13 nocloud amd64 20261001-2618 as an `http_file` pinned by Debian's
published SHA512; booted by the test kernel (6.18; the image's 6.12 lacks
FS_IOC_GETFSUUID and the microvm has no bootloader) from a qcow2 overlay
(Alpine's qemu-img, its own repository); `guest/init` installs dcfs and
switch_roots into systemd, a oneshot unit prints the verdict; `--boots N`
reboots the same disks; large tier, 256 MiB (768 under ASan), 55-160 s for
two boots. Covers fstab lines by UUID on ext4/xfs/btrfs, `mount -a`,
systemd's generated units, remount, exit statuses, journald; no NFS
backing (no client in the image, no guest network), no `umount.dcfs`
(none exists). Findings: (1) RESTART RACE, a real deployment bug, see
15.6b; (2) `mount(8)` hands the helper the resolved device, so a `UUID=`
line's FUSE source is `/dev/vda`, not the spec as written (plan decision
11 cannot be honoured; README corrected, test pins it); (3) util-linux
reads `-t nosuchfs` as "not suchfs". Deviations: `install_dcfs_into` wraps
dcfs in the sanitizer loader only when `dcfs_sanitizer` is set (nfs_test's
path changed, still green); no coverage from this guest; microvm sees
four virtio disks so the test's disks start at vda; Bazel's downloader
times out over IPv6 for cloud.debian.org on this host (`--distdir` note in
third_party/debian_cloud/README.md).

**15.6b Restart race (found by 15.6, 2026-10-09).** `systemctl restart
<mount unit>` fails in most restarts: systemd calls the unit stopped when
`umount` returns, but the old daemon is still closing the cache database,
so the new daemon starts with "Cache database ... is in use by another
dcfs process" (exit 32); for a mount `local-fs.target` requires (any
fstab line without `nofail`) that sends the machine to emergency mode.
Kept as `DISABLED_systemd-restart-parent-restarts-child` on a nofail pair
of units plus a README Limitations entry. Two fixes, not exclusive:
(a) the start waits, bounded (a few seconds, logged at INFO after the
first second, ERROR with the holder's pid on giving up), for a cache
database whose holder is a dcfs process that is exiting, which also
covers `umount X && mount X` in scripts; (b) a `umount.fuse.dcfs` helper
(util-linux calls `umount.<type>` for the mountinfo type) that unmounts and
then waits for the daemon's exit, so "unmounted" means "stopped", which is
what systemd assumes. Decision (russ, 2026-10-09): (b), not (a). "I don't like timers in our
code. They're necessary sometimes, but for the restart race, make an
umount.fuse.dcfs if that's a feasible alternative. Timers are inherently
brittle. If things are slow, the system breaks. This system should work on
everything ranging from an idle 256 core supercomputer to a 1 core
raspberry pi under 40 loadavg." So: a `umount.fuse.dcfs` helper (the same
binary, argv[0] dispatch like mount.dcfs; check which helper name
util-linux looks for given the mountinfo type `fuse.dcfs`, and install
`umount.dcfs` too if mount(8) ever records the type as `dcfs`) that
performs the unmount and then waits, with no timeout, for the daemon that
served that mount to exit (find it by the mount's FUSE connection or the
pid the daemon records; wait on the pid with pidfd_open, not polling), so
"unmounted" means "stopped" and systemd's restart starts the new daemon
after the old one released the cache database. If the daemon is already
gone (a crash), the helper returns at once. `umount -l` keeps working. Also
a style rule in docs/style.md from this decision: no timers or timeouts
in dcfs's own logic as a way to wait for another process or the kernel;
wait on the event (pidfd, inotify, a read that blocks, a lock) and let the
caller cancel; a timeout is permitted only where the thing waited for
cannot signal, and then it is named and justified in a comment. Tests
first in the systemd guest (the DISABLED_ restart check becomes the
test) and in mount_dcfs.sh (umount helper waits; crash case returns at
once). Owner: dcfs-implementer, lane-6 after 15.6 merges. Russ (2026-10-09,
"Agreed with all") on the rule's mechanical side, in the same step:
`tools/banned_symbols.txt` bans `sleep`, `usleep`, `nanosleep`, `alarm`,
`timer_create`, `timerfd_create` and `sqlite3_busy_timeout` in the daemon
(allow-with-reason for the legitimate exceptions, each naming the event
that cannot signal); `tools/repo_shape.py` refuses a bare `sleep` in
test/qemu/guest scripts outside one justified helper in lib.sh (the 6.5
scrub is removing the rest: coordinate by rebasing after it merges, or
list the remaining call sites as findings for 6.5 if it has not). Also
check whether the daemon sets SQLite's busy handler anywhere (a timer in
disguise: a blocking lock or an immediate clear error instead) and say
what it found. Follow-ups, not this step: the I/O-error fault window
(fault_lib.sh "up 6 s / down 20 s") becomes event-driven (an operation
count or the test driving dm-flakey's state), as the 11.3 review asked
(11.3c); the 11.6 backing-stall finding is fixed by the concurrency
design, never by a watchdog.

Status 2026-10-09: built (lane-6, one commit 7e3e42a; Opus review
running). DEVIATION NEEDING RUSS: the helper is `umount.fuse`, not
`umount.fuse.dcfs`: libmount drops the subtype when it looks for an
unmount helper (LIBMOUNT_DEBUG on util-linux 2.41 tries /sbin/umount.fuse,
fs.d/umount.fuse, fs/umount.fuse and nothing else), so dcfs's helper runs
for EVERY FUSE mount on the system and the administrator installs it as a
link to the dcfs binary. For any FUSE mount it runs `umount -i` as a child
with the same flags (umount(8)'s messages and exit status); for a dcfs
mount, unless lazy, it then waits on a lock: the daemon holds an exclusive
flock on `/run/dcfs/<major>_<minor>.lock` (the FUSE mount's device number)
from right after mounting to exit and removes the file as its last act
still holding it; the helper opens the file before unmounting (so never
waits for a later daemon reusing the number) and takes a shared lock
after `umount -i`, which it gets when the daemon is gone; a crashed daemon
releases it through the kernel; no pid file. `umount -l` detaches and does
not wait (documented, tested). FUSE_DESTROY was tried and dropped (none
for a plain FUSE mount). busy_timeout removed: only FinishRun's shutdown
checkpoint relied on it (an administrator's concurrent reader); it now
sees SQLITE_BUSY at once, logs "clean shutdown incomplete", and the next
start recovers. Enforcement: banned_symbols bans the sleep/timer family
and sqlite3_busy_timeout (allows: nanosleep from Abseil, SQLite, libfuse;
sleep from libfuse); repo_shape refuses bare `sleep` in guest scripts
outside `tools/repo_shape_sleeps.txt` (77 sites with reasons, only
shrinks); style.md 1.11 has the rule in russ's words. `quiesce_daemon`
and memory.sh's settle still poll (no userspace event for the FORGET
queue); the init sampler's cadence is its definition. The three DISABLED_
systemd checks are real and pass (restart of a parent, of a
local-fs.target mount, umount-then-mount at once, every daemon clean after
reboot); the busybox guest's umount lacks `-i`, so mount_dcfs.sh wraps it.
Fast 231 + 2, presubmit dcfs+qemu 181 pass, asan on the wrapper tests
green.

Opus review 2026-10-09: not yet mergeable; the idea is sound. The libmount
premise was half wrong: `mnt_context_prepare_helper` tries
`umount.fuse.dcfs` FIRST and strips the subtype only if that is missing
(the debug line prints after the strip); the subtype is absent only on
the statfs shortcut (root, absolute directory target, none of -f -l -c -r
-d, no utab entry); systemd always unmounts with `umount <where> -c`, the
mountinfo path. DECISION (orchestrator, per the reviewer's
recommendation; the question to russ is moot): ship `umount.fuse.dcfs` by
default (covers systemd restart and reboot and `umount -c`), `umount.fuse`
as a documented opt-in affecting every FUSE filesystem; the helper
accepts both names. Blocker: the helper hangs forever when the unmount
does not end the superblock (a bind of a dcfs subtree, an rbind, a copy
in another mount namespace such as a container volume or an `unshare -m`
shell, `umount -r` on a busy mount, which libmount answers 0 after a ro
remount); under systemd a 90 s stop timeout then SIGTERMs a daemon still
serving; fix: open `/sys/fs/fuse/connections/<minor>` before unmounting
and wait only if its st_nlink is 0 after (fuse_ctl_remove_conn runs
inside the umount). HIGH: anonymous device numbers are reallocated
lowest-first during the umount, so `systemctl restart a.mount b.mount`
can make a's new daemon open D_b's lock file, get EWOULDBLOCK and refuse
(emergency mode); fix: a blocking LOCK_EX with the unlink-while-locked
re-check, or key the lock by the unique mount id (6.8+). MEDIUM: `umount
-N` rejected for every FUSE filesystem; `umask(0)` and RaiseFileLimit run
before the argv[0] dispatch (an ordinary user's sshfs unmount logs a dcfs
WARNING); the systemd test's `systemctl start` after a restart can hide a
failed child restart; systemd's 90 s stop timeout now kills a long
FinishRun (document x-systemd.mount-timeout, Ctrl-C, umount -l); coverage
gaps in the helper's error paths. LOW: unlink order vs the db lock, /run
creation, a needless realpath walk, NotFoundError as a sentinel, stale
busy_timeout comments, banned_symbols reasons (sysinfo's nanosleep is TSC
calibration; add AbslInternalSleepFor), repo_shape's sleep regex (per
file, misses `sleep .5`, usleep, `timeout N`, `read -t`), no test of the
one real busy_timeout change. Q3: the busy_timeout argument holds; the
next start's recovery is cheap because SyncBacking has already emptied
the dirty set. Q6: no model change; design.md's clean_shutdown text still
matches. Sent back 2026-10-09 with items 1-8, 13, 14 required and the
cheap LOWs.

**15.7 Docs:** README (fstab with and without systemd, `dcfs.fstype`
values, `_netdev`, fsck, trees, over-mounting, remount, NFS exports,
administrator responsibilities, why root, the bind-form submount behavior,
and an operations section: wiping one instance's cache, what to do after
an accidental out-of-band change, upgrading dcfs (needs a remount), what
the log messages mean),
design.md (wrapper, daemonization, boundaries, instance identity); remove
`packaging/dcfs.service`.
Status 2026-10-09: done, merged 50fee50 (two commits, docs only). README:
fstab with and without systemd, `dcfs.fstype` values, options not shared,
pass 0 (no fsck.dcfs yet), Remounting, Logging, Instances/trees/boundaries,
Responsibilities of the administrator, an Operations chapter (wiping the
cache of one instance, after an out-of-band change, upgrading, what the
log messages mean); design.md: the wrapper, Startup as built (StartupPurge
until 15.3), daemonization, SyslogSink, boundaries as they stand, `--source`
gone; `packaging/` removed; dcfs.8 carries the new sections (a `##` section
contributes nothing of its own to extract_sections: its `###` children are
listed one by one). Left out as not built: 15.5 (fsck.dcfs, `dcfs exports`),
15.3 (default cache path, `dcfs.cache_dir`), 15.4 (stubs: `none` and
`bind` refuse a mount below SOURCE today). Stated from standard behaviour,
not verified in the tree, for 15.6 to assert: fstab-generator units finish
when mount.dcfs exits; parent/child mount-unit ordering; `_netdev`,
`nofail`, `noauto`; `x-systemd.requires-mounts-for=` for none/bind sources
and the cache's filesystem; `journalctl -t dcfs` finds the daemon's lines;
`systemctl restart <unit>.mount`; libmount merging fstab options into a
remount and `mount /data` taking the line's options; `umount -R` order;
after SIGKILL `umount -l` before remounting.
Review each step against the race/crash/tri-state rules before merge.

## Status 2026-10-08 (15.1/15.2 in review fixes)

As built on the branch (lane-6): argv[0] dispatch; `dcfs.` option split;
capture of native/bind mounts in a private namespace via mount(8) +
`open_tree(OPEN_TREE_CLONE)` over a socketpair; own daemonisation with a
readiness report after the first FUSE_INIT; `SyslogSink` as an
`absl::LogSink`; remount of the dcfs mount only; `--source` gone. Review
(Opus): fix first: the 11.5 forced-read-only refusal cannot see a captured
mount (anonymous namespace: run it in the helper on the staging mount);
relative paths after the daemon's chdir; staging in /tmp instead of a
tmpfs in the private namespace; readiness untestable; exit statuses 1 for
usage/non-root, 32 for a failed start, native status passed through; the
syslog sink takes the configured threshold (one knob); also install as
`mount.fuse.dcfs` for libmount's remount lookup. Drift to record:
StartupPurge is not removed (15.2's text said it would be; decide in
15.3 with the cache path), `LOG_PERROR` not used, the systemd unit kept
until 15.7. design.md's `--source` text and startup section are 15.7's.
Second pass (2026-10-09): the fixes introduced three util-linux breakers
the busybox guest cannot show (the `none` form refused the `rw` libmount
always passes; a remount refused a native `ro` merged in from fstab; the
absolute-path fix rewrote non-path native specs such as ZFS datasets and
virtiofs tags): one more round. Deviations recorded: the capture's
staging is a tmpfs over `/proc/sys/vm` inside the helper's private
namespace (harmless there; a tmpfs over /run or /tmp could hide SOURCE,
an mkdtemp'd directory leaks on SIGKILL); foreground runs keep stderr
without a syslog sink instead of `LOG_PERROR`; `StartupPurge` stays
until 15.3 decides the cache path. 15.6's util-linux guest is where the
helper protocol gets its real test.

