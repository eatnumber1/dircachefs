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
