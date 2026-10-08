# Access times: the decision and the alternatives (russ, 2026-10-08)

Reads go through FUSE passthrough, so dcfs sees a file's OPEN and RELEASE
but never its reads, and the backing filesystem stamps atime itself.
Until 23.8 dcfs *predicted* atime at open (relatime rules from the mount's
flags), which was wrong for an open that reads nothing, for the private
open behind lsattr/chattr, for `chattr +A`, and after a power loss (README
"Limitations", the paragraph starting "Access times are predicted").

## Decision (23.8)

- **Regular files: the truth from the held fd.** dcfs holds a backing fd
  for every open passthrough file (and an O_PATH fd for written files). An
  open file pins its dentry and inode (reference counts, not an LRU), so
  the in-core inode cannot be evicted under any memory pressure, and
  `fstat` on that fd is answered from memory by ext4, xfs and btrfs: no
  disk I/O. Rule: while dcfs holds a backing fd for an inode its
  attributes come from `fstat` (at FLUSH, at RELEASE, and for a GETATTR
  that arrives while it is open); otherwise from the cache. Every atime
  update of a regular file goes through an open file description (read
  family, mmap: the mapping holds the file open until munmap, splice,
  sendfile, copy_file_range, execve, nfsd, io_uring), so there is always a
  held fd to ask. After a passthrough read the kernel invalidates its own
  cached attributes, so a stat after a read reaches dcfs. The backing
  writes atime back lazily and never fsyncs it, so the row recorded from
  `fstat` is marked dirty and the next sync point's `syncfs` makes it
  durable (power loss is then covered by recovery like any dirty row).
  Prediction is removed.
- **Directories and symlinks: stamped in the cache, never written to the
  backing.** Their reads (readdir, readlink) are served from the cache and
  never reach the backing, so the backing's atime genuinely does not move;
  dcfs stamps the predicted relatime value in the database and keeps it
  across restarts. A cache wipe loses it. Documented as a limitation.

## Alternatives considered

1. **Serve the cached atime unchanged until the next refresh.** Honest
   (always a value the backing had at some instant) and free, but stale
   for arbitrarily long. Rejected (russ): read-modify-write cycles that use
   atime break.
2. **Require or recommend `noatime` on the backing.** Then atime never
   changes and the old prediction is exact. Kept as a last resort only.
3. **Route reads through dcfs.** dcfs would see every read and stamp
   atime itself. Rejected: passthrough is worth far more than atime.
4. **Directories: write the atime to the backing.** `utimensat` can set
   atime directly (root), but an explicit timestamp set bumps **ctime**,
   which a real readdir never does, so it is observable. The only way to
   move a directory's atime like a read is a read: a minimal `getdents` on
   the backing directory fd (a `readlinkat` for a symlink) goes through the
   relatime check and leaves ctime alone. Under relatime that is at most
   one pending read per directory per day; it could be replayed whenever
   the disk is known awake (a mutation's syscall, a passthrough open, the
   sync point), followed by an `fstat` to record the true value. Deferred:
   add it only if something real needs the on-disk directory atime.
5. **Mark attributes unknown after a read-open and refresh lazily at the
   next GETATTR.** Exact, but a GETATTR after the file is released can
   reach the disk for the inode: a spin-up for a stat, the thing dcfs
   exists to avoid. Subsumed by the decision (fstat only while held).

## Kernel facts relied on

- Inode eviction (`prune_icache_sb`) skips inodes with a nonzero reference
  count; an open `struct file` references its dentry, the dentry its inode.
- `getattr` of ext4/xfs/btrfs reads in-core fields; relatime updates the
  in-core atime at read time (`touch_atime`), written back later (lazily
  with `lazytime`, within the dirty expiry otherwise).
- FUSE passthrough reads call `fuse_invalidate_atime`, so the kernel's
  attribute cache is dropped after a read.
- `utimensat` with an explicit time updates ctime; `getdents`/`readlinkat`
  update atime only.

## Refinements while building 23.8 (orchestrator decisions, 2026-10-08)

- **Cache ahead, beyond the held fill:** any attribute record of an inode
  with an open read fd (a population's or resolve's probe, a phase-3
  refresh) can capture an atime the backing has not written back. So every
  attribute write for a held inode marks the row dirty (atime reason) and
  touches the guard; a rejected or failed held fill marks it unknown and
  dirty.
- **Cache behind (crash while a file is open for reading, before the next
  fstat):** closed rather than documented: a cold read-only open marks the
  row dirty in one write transaction without fsync, and sync points keep
  the rows of held inodes until their release records the truth. Residue:
  a power loss that loses that WAL commit leaves the old atime.
- **atime-only dirty rows do not drive sync points.** A sync point's
  `syncfs` would force the backing's lazy atime write-back (with
  `lazytime`, the write the operator deferred by a day). `ctx.dirty.any` is
  driven by mutation-dirty rows only; atime-only rows are cleared by a sync
  point that runs for another reason, by the clean shutdown's final sync,
  or by recovery after a crash, and may stay dirty across restarts until
  then. The model carries a reason bit on the dirty set.

