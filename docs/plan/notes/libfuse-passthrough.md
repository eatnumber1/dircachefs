# The LKML discussion and amir73il's libfuse_passthrough

Analysis from 2026-10-04 of the review thread on russ's kernel patch
(https://lore.kernel.org/r/20260927141437.1432584-1-russ@har.mn) and of
Amir Goldstein's libfuse_passthrough
(https://github.com/amir73il/libfuse/tree/libfuse_passthrough, commit
1660c22, directory `passthrough/`).

## What libfuse_passthrough does about identity

- nodeid = backing inode number; generation = the backing generation,
  decoded from ext4/xfs file handles. Inodes live in an in-memory table.
- `LOOKUP(nodeid, ".")` for an unknown nodeid (NFS reconnect, restart):
  it rebuilds a file handle from the inode number (generation from xfs
  bulkstat, or 0 on ext4, which accepts any generation), opens it, and
  replies with the generation it actually found. The kernel's check in
  `fuse_get_dentry` turns a mismatch into ESTALE.
- Missing inodes return ESTALE; connectable handles
  (`AT_HANDLE_CONNECTABLE`) and a reconnect step serve path-based modules.
- Multi-threaded by default; modules are chained; refuses mount points
  below the source (ENOTSUP) and backing inode 1.

## Consequences for dcfs

- The kernel patch adds nothing for a filesystem that never changes a
  nodeid's generation: dropped (Phase 3).
- dcfs adopts the same identity scheme (Phase 14), on top of its cache.
- Rewriting dcfs on libfuse_passthrough was rejected (see
  `../decisions.md`).
