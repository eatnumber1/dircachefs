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
    section says where to copy it.

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
**15.6 systemd guest** (Debian rootfs booting systemd in the QEMU harness)
(its prerequisite, a root-owned Debian image, was done in Phase 4c:
Bazel-built e2fsprogs builds the image from the root-owned package tar)
and its tests. Owner: Sonnet.
**15.7 Docs:** README (fstab with and without systemd, `dcfs.fstype`
values, `_netdev`, fsck, trees, over-mounting, remount, NFS exports,
administrator responsibilities, why root, the bind-form submount behavior,
and an operations section: wiping one instance's cache, what to do after
an accidental out-of-band change, upgrading dcfs (needs a remount), what
the log messages mean),
design.md (wrapper, daemonization, boundaries, instance identity); remove
`packaging/dcfs.service`.
Review each step against the race/crash/tri-state rules before merge.
