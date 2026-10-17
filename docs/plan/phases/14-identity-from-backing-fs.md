# Phase 14 — Identity from the backing filesystem

**Problem.** dcfs's FUSE nodeid is its own row id and its generation is a
random number stored in the cache DB, so NFS handles do not survive deleting
the DB, and identity needs durability machinery (random generations,
reuse-after-rollback analysis). Since history.md amendment 12 there is exactly one
backing filesystem, so backing inode numbers are unique and can be the
nodeids, as libfuse_passthrough does. The kernel already rejects stale
handles: after `LOOKUP(nodeid, ".")` it compares the reply's generation with
the handle's (`fuse_get_dentry`, fs/fuse/inode.c) and returns ESTALE on a
mismatch.

**Design.**
- nodeid = backing inode number; the source root is nodeid 1; refuse a tree
  that exposes backing inode 1 (as libfuse_passthrough does).
- FUSE generation = backing generation, decoded from the backing file handle
  (works for every file type, unlike FS_IOC_GETVERSION). Decoders for the
  tested filesystems: ext4 and xfs `FILEID_INO32_GEN` (1) / `FILEID_INO64_GEN`
  (0x81) (ino, gen); btrfs `FILEID_BTRFS_WITHOUT_PARENT` (0x4d) and
  `_WITH_PARENT` (0x4e) (`struct btrfs_fid`: objectid, root_objectid, gen u32).
- `LOOKUP(nodeid, ".")` / any request for a nodeid with no cached row
  (restart, cache wipe, NFS reconnect): build a backing handle from the inode
  number and open it: ext4 and btrfs accept generation 0 as "any"
  (`ext4_nfs_get_inode`, `btrfs_get_dentry`); xfs needs the exact generation:
  get it with `XFS_IOC_FSBULKSTAT_SINGLE` (CAP_SYS_ADMIN; dcfs is root). Reply
  with the generation actually found; the kernel enforces the match.
- The cache DB is keyed by backing (ino, gen); losing it costs only a cold
  cache. Removed: random generations, `inodes.id AUTOINCREMENT` as identity,
  the power-loss nodeid-reuse analysis (history.md amendment 14).
- Recycling: with exclusive access a backing inode number cannot be reused
  while dcfs still holds the object (the removed-object records added in the original
  plan's step 6.3, audit crash F10, keep an fd until the kernel's last
  FORGET), so the kernel never sees one nodeid for two live objects.
  Out-of-band recycling stays unsupported (this is where the kernel
  FUSE_ATTR_GENERATION patch would add protection).

**Decisions (russ, 2026-10-04).**
1. Backing filesystems without a known handle format are not supported:
   refuse to start with a clear error naming the filesystem type.
2. Boundaries (submounts and btrfs subvolumes): the entry is a visible stub
   directory, anything inside returns ENOTSUP, rename/link across returns
   EXDEV; access comes from mounting another dcfs instance over it
   (Phase 15 (The `mount.dcfs` wrapper)). Backing inode numbers at or above 2^63 are refused like inode
   1: that range is reserved for stub nodeids (ext4 is 32-bit, xfs < 2^56,
   btrfs objectids grow up from 256).
3. dcfs was never in production: change the schema freely, no migration.
   Delete the migration code; a cache with another schema version is
   rebuilt with a log line.

**14.1 Tests first (must fail on today's main; quote the output).**
- Invert `handles.sh` `db-wipe-*`: take handles of a file, a directory, a
  symlink and a file in a subdirectory; stop dcfs; delete the cache DB;
  restart; every `fhtest open` must SUCCEED (today: ERR ESTALE) on
  ext4/xfs/btrfs (handles_test matrix).
- `nfs.sh`: hold a file open on the NFS client, wipe the DB, restart dcfs
  (with `exportfs -f`), keep reading without ESTALE (today: ESTALE).
- Recycled inode after a wipe: take a handle, stop dcfs, wipe the DB, recycle
  the inode number on the backing fs (rm + create until reused, with dcfs
  stopped), restart: the old handle gets ERR ESTALE (kernel generation
  check); the new file's handle works. Per filesystem.
- `stat -c %i` through the mount equals the nodeid seen in `fhtest handle`
  (nodeid = backing ino) and the backing `st_ino`.
- Unit: handle decoders for each format, using real handles taken in the
  guest on ext4, xfs (inode32 and inode64 mounts) and btrfs; generation for
  symlinks and devices non-zero where the filesystem provides one.
- Unit: `LOOKUP(".")` resolution without a cache row returns the backing
  generation; xfs path uses bulkstat; ext4/btrfs path uses generation 0.

**14.2 Handle decoders** (`dcfs/handle_identity.{h,cc}`): parse ino and
generation from backing handles; build an "any generation" handle from an
inode number; xfs bulkstat helper. Unit tests.

**14.3 Schema and cache identity:** `inodes.id` = backing ino (no
AUTOINCREMENT), `fuse_gen` = backing generation, identity check by (ino, gen,
handle bytes, btime) unchanged; delete Migrate's upgrade paths (decision 3);
update `UpsertInode`, population, recovery, removed-object records.

**14.4 Request path:** entry/attr replies use the new nodeid/generation; any
request for an uncached nodeid resolves through 14.2 (open by inode number,
statx, upsert row) instead of ESTALE; `ParentOf` and `..` resolution use it
too.

**14.5 Cleanup and docs:** remove random generations and their tests, update
design.md identity model, README Limitations (remove "handles do not survive
a cache wipe"), docs/conformance.md if pjdfstest changes.

**14.6 Review:** a read-only audit of the new identity code against the race,
crash and tri-state rules before merge.

Owner: Opus (identity invariants), one step at a time; test-first per item.
Order: Phase 13 (Connected backing fds) first (small, independent), then Phase 14 (Identity from the backing filesystem) (both touch
`OpenNode`).

## Finding from the identity model (12.5, 2026-10-08)

Under out-of-band inode recycling, a nodeid the kernel still holds whose
row went (14.2 resolves uncached nodeids by inode number) would be
reopened by inode number and reach the NEW object: worse than "out-of-band
recycling unsupported" suggests. In mainline FUSE only entry replies and
handle decoding (`fuse_get_dentry` → LOOKUP(".")) make the kernel compare
generations; a GETATTR/OPEN/READ on a held inode carries none. So 14.4
must, for a nodeid held in this mount without a row, reopen with (or
compare against) the generation it handed out in this mount, else answer
ESTALE. russ's FUSE_ATTR_GENERATION kernel patch (protocol 7.47) makes
attribute replies carry the generation and would close this kernel-side;
the Phase 14 design should say which it relies on. The identity model's
`IdentTrace.tla` encodes today's AUTOINCREMENT nodeids ("no reply after
the row went") and `T_IdReply` needs relaxing when nodeid = inode number.

## Decision: no filesystem-UUID requirement, handles stay (russ, 2026-10-08)

Context: dcfs refuses a source without a filesystem UUID (FUSE mounts,
tmpfs, NFS clients; `FS_IOC_GETFSUUID` is UNIMPLEMENTED there), which 8.4
hit when it tried a dcfs-over-dcfs test.
- dcfs keeps reaching objects **only by handle** (`name_to_handle_at` /
  `open_by_handle_at`); a handle-free access path is a large complexity
  increase and is not wanted. A backing whose root cannot give a handle is
  refused at start with a message saying so (FUSE filesystems without
  export support stay unsupported; NFS clients and tmpfs, which export
  handles, become possible).
- Tying the cache to the backing by UUID is not required: if the operator
  mounts a different filesystem under dcfs, that is garbage in, garbage
  out. No new identity marker (no xattr or file written into the backing).
  Use the filesystem UUID when the backing has one (as today); when
  `FS_IOC_GETFSUUID` is unsupported, fall back to something cheap and
  always available, the mount point (the source spec as written) plus
  `statfs` `f_fsid`, recorded in `cache_state` and compared at start as a
  sanity check with a clear error, never as a guarantee (russ, 2026-10-08:
  "use the uuid if it is available").
  One abstraction over the two (a `SourceIdentity` value built once at
  start from whichever source is available, compared and printed through
  one interface); the rest of the code never branches on "UUID or
  fallback" (russ, 2026-10-08; style rule 25.5).
- The NFS export side (handle classes, generation, `ident.tla` from 12.5)
  is unchanged by this; it keys on the backing's handles, not the UUID.


## 14.9 The NFS handle is a compatibility surface: a nodeid epoch and a handle corpus (russ, 2026-10-11)

russ, after the trial: "Node IDs already is something that requires
backwards compatibility, or at least robust detection if the NFS server
asks for a node ID generated in a dcfs version that's no longer
compatible." The kernel's FUSE export operations put dcfs's nodeid and
generation into every NFS file handle; those handles live on clients
across server reboots and dcfs upgrades, and knfsd asks dcfs about them
without a LOOKUP first. Today (14.3): the nodeid is the backing inode
number and the generation the backing inode's generation (0 where the
filesystem has none; stubs take nodeids above a high-water mark), so a
handle already survives a cache wipe and a reboot by construction, and
what remains undetected is (a) a generation-0 filesystem recycling an
inode number, (b) a stub nodeid reassigned after a wipe, and (c) a dcfs
version that changes what either field means.
1. **An epoch that makes the past unresolvable.** The fields are the
   kernel's: 64-bit nodeid, 32-bit generation. Where to carry a
   per-database epoch (stored in `cache_state`, bumped by a wipe or by a
   migration that reinterprets either field) is a measurement first:
   ext4 inode numbers are 32-bit, xfs and btrfs use up to 64, so the
   nodeid's high bits are free only where the backing says so; the
   generation's bits are the alternative for generation-0 filesystems and
   stubs. The requirement is the fixed point: a handle minted under
   another epoch answers ESTALE deterministically, never another object.
   The root nodeid (FUSE_ROOT_ID = 1) is the one constant.
2. **The rule, from the first release**: the meaning of (nodeid,
   generation) is a public interface. A change to it bumps the epoch; a
   handle from an unknown epoch is ESTALE; the README's NFS section says
   so; the identity model (12.5) gains the epoch.
3. **A handle corpus test**: for each released version (starting with the
   first), a fixture holding handles it minted for a known tree (nodeid,
   generation, backing identity); the current build must resolve each to
   the same object or answer ESTALE, never another object. Generated by a
   Bazel target from the pinned previous release's binary when there is
   one; until then the fixture is the current build's own, so the
   mechanism exists on day one.
4. The reboot shape is the one that matters (russ: "the most important
   thing that they survive is a server reboot"): 15.12's test becomes a
   systemd-guest reboot with an NFS export of a dcfs fstab mount and a
   client holding handles and an open file, mount and export restored by
   fstab, the client's handles resolving afterwards; the daemon-restart
   and bind-export cases are the smaller variants.
Owner: dcfs-protocol (identity). After the Queue's items 3-5; before the
first release in any case. Related: keeping the /dev/fuse descriptor
across restarts (notes/trial-2026-10-11.md) is demoted to "probably
never" (russ: the in-process state handed between versions would be a
second compatibility surface on top of the database's; the reboot case
is what matters and this design does nothing for it).

Addendum (russ, 2026-10-11, on stubs: "Why can't we use the underlying
filesystem's inode+generation? I know it's hidden, but there's a way to
access it anyway, right?"). Today a stub's nodeid is a counter from 2^63
(`cache_state.last_stub_id`), never reused, which is bookkeeping that
exists only because the stub's identity was not derived from the
backing. A derivation exists for both kinds of boundary:
- A real mount: the covered directory is an inode of the backing
  filesystem, unique in our nodeid space, and its inode number is
  already in dcfs's hands: the parent's `getdents` returns the directory
  entry's own `d_ino`, not the mounted root's. Its generation and handle
  are reachable only by opening it, which path lookup will not do (it
  follows the mount); a private copy of the mount namespace with the
  submount detached would, at a cost per probe. So: nodeid = 2^63 |
  covered d_ino, generation 0 (the generation-0 rules apply).
- A btrfs subvolume: there is no covered inode (the dentry is the
  subvolume's root, ino 256 in its own tree, so `d_ino` collides across
  subvolumes), but the subvolume id (BTRFS_IOC_GET_SUBVOL_INFO on the
  root, which is openable; dcfs refuses its contents by policy, not for
  lack of access) is unique in the filesystem and never reused. So:
  nodeid = 2^63 | subvolume id, generation 0.
Both are stable across cache wipes and reboots, need no counter and no
"never handed out again" rule, and keep the reserved range's property
that no regular object collides. Semantics match: a boundary at a
directory keeps its identity whatever is mounted there; a subvolume
deleted and recreated under the same name is a new object. To settle in
14.9 together with the epoch (the stub range is the high bit the epoch
cannot have; the generation carries it for stubs as for generation-0
files). Cost: a schema change to `stubs` (no migration before release).
russ, same day: "When you start investigating replacing the stub files,
do more in-depth investigation, and maybe a little prototyping. I'm not
convinced yet that this is a good idea, nor that it will work." So this
is an investigation with a prototype before it is a design: in a guest,
measure what `getdents` actually returns as `d_ino` for a covered
directory on ext4, xfs and btrfs (and for a bind mount, an overmount of
the same directory, an autofs point, an NFS submount); what
`BTRFS_IOC_GET_SUBVOL_INFO` returns for a subvolume reached through
dcfs's O_PATH descriptors and whether the id is stable across snapshot,
rename and send/receive; how a stub's nodeid behaves across a cache
wipe, a reboot and a dcfs upgrade in the identity model (12.5) with the
derivation substituted; whether the generation-0 rules give the ESTALE
semantics the NFS handle needs; and the per-probe cost if the covered
directory's handle is wanted (a private mount namespace, measured). A
small prototype on a branch (the derivation in `cache::SetRefused`, the
existing stub tests, the identity oracle after a crash) and a written
comparison with the counter before any decision; the decision is
russ's.

## 14.10 Identity for backing filesystems without a UUID (network filesystems first) (russ, 2026-10-16)

The review of 15.11 found that NFS has never worked as a backing:
`GetDeviceId` (dcfs/device_id.cc) refuses any filesystem whose
FS_IOC_GETFSUUID answers ENOTTY, and NFS never sets a superblock UUID. russ
wants network backings supported for administrators who can guarantee
exclusive access, so this is a design step, protocol agent, after the
current queue. Questions to settle, with the identity model (12.5) updated
in the same change: what identifies the backing across mounts and reboots
when the kernel offers no UUID (for NFS: the server, the export path and
the filesystem id NFS reports in statfs/fsid and in its file handles; for
SMB/CIFS: server and share; an administrator-supplied identity in the
mount options as the general fallback, with the cache refusing a mismatch);
whether file handles (`name_to_handle_at`) are stable enough on the network
filesystem to key rows by (NFS handles are; check what the client returns
and whether open_by_handle_at works on an NFS client mount); what happens
when the server restarts or the export moves (stale handles: ESTALE
handling and the identity oracle); what the generation-0 rules give where
the filesystem reports no generation. Deliverables: the design in
docs/design.md, the identity model change and its known-bug variants, a
QEMU guest test with an in-guest NFS server as the backing (the nfs test
already runs knfsd), README's network-backing example restored as
supported, and the systemd guest's `_netdev` line backed by a real NFS
mount. Until then README marks network backings unsupported (15.11c).

**Design half done (2026-10-17, lane-4, dcfs-protocol).** Split: 14.10 is
the evidence, the design and the model; the code, the dcfs guest tests and
the README go to 14.10b below. What was settled:
- Evidence: `//test/qemu:nfs_backing_test` (no dcfs; the guest's knfsd
  exporting ext4, a loopback NFSv4.2 client) pins what the design rests on:
  FS_IOC_GETFSUUID `ENOTTY`, `f_fsid` 0, `st_dev` per client superblock
  (shared by mounts of one server filesystem, whose mountinfo source is the
  first mount's), handles = fileid + type + the server's handle (which
  carries the server's inode number and generation), `open_by_handle_at`
  works, FS_IOC_GETVERSION `ENOTTY`, the server's birth time, a recycled
  inode `ESTALE`, a server restart invisible (byte-identical handles, a held
  descriptor reads on), an export moved with the same fsid invisible, a new
  fsid `ESTALE` for everything including the mount's root, one fsid number
  on two filesystems giving identical root handles.
- SMB/CIFS cannot be a backing whatever its identity: no export operations
  (`CONFIG_CIFS_NFSD_EXPORT` depends on `BROKEN`, and has no `fh_to_dentry`),
  so no `open_by_handle_at`, and dcfs reaches objects only by handle. Not a
  follow-up for identity; it would need the handle-free access path russ
  rejected on 2026-10-08. No samba or ksmbd-tools in the guests either.
- The design: docs/design.md "Backings without a filesystem UUID (network
  filesystems)": `SourceIdentity` (a UUID, or only `f_type`), with the root
  row's handle check as what tells two exports apart, `f_fsid` dropped from
  2026-10-08's plan (it is 0), the administrator's guarantees (exclusive
  access, a unique and fixed fsid per export, no replacement while dcfs
  runs; neither a hard mount nor a sync export is required), the handle and
  generation rules (today's matching
  unchanged; Phase 14 needs a digest generation and no open by number on
  NFS), and what the cache does on a server restart, an outage, a changed
  fsid, a moved export and volatile handles.
- The model: ident.tla gains `Swap`/`StartRefused`, `ServerDown`/`ServerUp`
  with `VolatileHandles`, and `OpenByIno`; `MC_ident_net.cfg`,
  `MC_ident_target_net.cfg`, two known bugs (`ident_net_no_source_check`,
  `ident_net_forget_on_error`) and four limitations (formal/README.md).
- russ's rulings (2026-10-17): a soft mount is neither refused nor
  recommended against (its timeout is a backing `EIO`, which the tri-state
  protocol already handles), and README says nothing about hard or soft; a
  sync export is not required, and README says instead that after a server
  crash with an `async` export the cache may list changes the server lost,
  so the administrator probably wants to delete the cache database; Phase
  14's generation on NFS is a digest of the server's handle bytes (for
  Phase 14's own steps, not 14.10b); no backing name at all (neither a
  `dcfs.backing_id` option nor the mountinfo source): the identity of a
  backing without a UUID is `f_type` plus the root row's check, and README
  requires a unique, fixed fsid per export (as exports(5) does), which also
  gives persistent handles. Two filesystems under one fsid number are a
  documented limitation (`limitations/ident_net_same_identity`), not a
  guarded case.
- Still open for russ: whether "no replacement while dcfs runs" stays a
  documented guarantee.

## 14.10b Network backings: the implementation

Owner: dcfs-protocol. After the error-handling rework of dcfs/ that was in
flight on 2026-10-16 (it touches device_id.cc and main.cc). Test first
throughout; the design is docs/design.md "Backings without a filesystem
UUID". russ's rulings of 2026-10-17 are in the brief: no backing name, no
hard-mount or sync-export requirement. If russ drops the "no replacement
while dcfs runs" guarantee (still open), only README's wording changes.

Code:
1. `SourceIdentity` (dcfs/device_id.{h,cc}, renamed or not) replaces
   `DeviceId` as the source's identity, one type with one interface
   (russ, 2026-10-08; style 25.5): a UUID with btrfs's subvolume id, or
   only `f_type` for a backing without one. `Serialize`/`Parse` become
   variable-length with a kind byte; `ToString` prints the filesystem type
   by name (`FstypeName`). `GetDeviceId`'s `ENOTTY` branch builds the
   `f_type` kind instead of `Unimplemented`; ZFS and the like keep failing
   later, at their first handle. Decide in the step whether `FileHandle` and
   the `filesystems` table keep a per-handle device at all (one backing per
   database since amendment 12; the schema may change without a migration
   before release): one way to do it, and the report says which.
2. The start compares the stored identity with the current one in main.cc's
   existing check and refuses a mismatch naming both and the remedy (delete
   the database). The root row's check stays as it is; for a backing
   without a UUID it is what tells two exports apart, and its message says
   so (an export re-made with another fsid, or another export).
3. `name_to_handle_at` of SOURCE failing with `EOVERFLOW` at start gets an
   error naming the server's handle size as the cause.
4. No change to `OpenNode`/`StaleOrUnreadable`, and nothing that looks at
   hard or soft mounts (russ, 2026-10-17): the rule the design relies on
   (only a confirmed `ESTALE` forgets a row; an `EIO` or `ETIMEDOUT` from a
   soft mount's timeout is a backing error like any other) already holds;
   the tests below pin it for NFS.

Tests (failing first; the guests chrooted into the Debian rootfs with the
guest's own knfsd, as nfs_backing_test sets it up):
- Unit: `SourceIdentity` round trips, comparison and printing of both
  kinds; device_id_fault_test's `NfsIsRefusedBecauseItHasNoFilesystemUuid`
  becomes the `f_type` case; a database made on one `f_type` refused on
  another; the `EOVERFLOW` refusal.
- A guest test of dcfs on a loopback NFS mount of ext4: listing and content
  match the export; a warm metadata pass makes no NFS round trip (the
  client's `/proc/self/mountstats` GETATTR/LOOKUP counts unchanged) and
  reads nothing on the server's disk; write-through lands on the server; a
  dcfs restart keeps rows and FUSE handles; a file recycled on the server
  between dcfs runs leaves dcfs's old FUSE handle `ESTALE`.
- The same guest: a server restart while dcfs runs (hard mount) resumes,
  a held descriptor reads on, no row is forgotten (`testutil sql` row
  counts before and after) and no request gets `ESTALE`; the same outage on
  a soft mount (a short `timeo`): requests that reach the backing during it
  fail with `EIO` or `ETIMEDOUT`, a mutation's records are unknown and
  re-probed after it, no row is forgotten, and everything is served again
  once the server is back; the export's fsid
  changed while dcfs runs: requests fail, no row is forgotten, and after
  remounting the export and restarting dcfs the start refuses with the
  identity message; deleting the database starts cold.
- Identity at start: the export moved to another path with the same fsid
  starts warm; vdb re-exported under another fsid is refused (root handle);
  vdc exported under a UUID fsid in vdb's place is refused (root handle);
  vdc exported under vdb's fsid number is not detected, the documented
  limitation, which the test pins so that a change in it is seen (and
  README's unique-fsid requirement names).
- The systemd guest's `_netdev` line on a real NFS mount: the Debian cloud
  image (third_party/debian_cloud) has no NFS server; either add
  nfs-kernel-server to that guest's image or export from a second guest, the
  cheaper one measured first. Assert that the NFS line and the dcfs line on
  it mount at boot in order after the network, and unmount before it.
- formal: nothing new expected (the model has the swap, the outage and the
  refusal); if the code's start check emits a trace event, IdentTrace
  validates it.

Docs: README's network-backing paragraph and fstab example restored as
supported, with the administrator's guarantees (a unique, fixed fsid per
export among them), and the
`async`-export note (after a server crash the cache may list changes the
server lost; delete the cache database), and nothing about hard or soft
mounts; the Requirements list says what a backing without a UUID needs;
design.md's section loses its "until then" sentence.
