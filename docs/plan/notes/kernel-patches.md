# Kernel patches we want (russ sends; dcfs keeps a fallback until each ships)

The running list of upstream kernel changes that dcfs would benefit from,
with the dcfs step that owns each and what dcfs does meanwhile. Started
2026-10-10 at russ's request ("Add that to the list of kernel patches we
want to do").

| # | Change | Why dcfs wants it | Owning step | Meanwhile |
|---|---|---|---|---|
| 1 | FUSE: attribute replies carry the generation (`FUSE_ATTR_GENERATION`, protocol 7.47) | a held nodeid whose row went could otherwise be reopened by inode number and reach a recycled object; closes the Phase 14 finding kernel-side | russ's patch series (in flight; `~/Sources/fuse-generation-qemu` is its test suite) | 14.4 compares against the generation handed out in this mount, else ESTALE |
| 2 | ext4: `EXT4_IOC_SET_TUNE_SB_PARAM` enabling casefold on a mounted filesystem without loading `sb->s_encoding`; the next readdir of a `+F` directory dereferences NULL in `utf8nlookup` | found by the suite; present in mainline (7.3-rc6) | 26.15 (report drafted, reproducer in `tools/kernel_bugs/ext4_casefold_tune/`) | the DISABLED_ check `casefold_tune_oops_test`; Phase 16 rejects casefold directories anyway |
| 3 | btrfs: `WARN_ON(inode->csum_bytes)` in `btrfs_destroy_inode` after a failed inode read of a directory whose item was updated only in memory (`index_cnt`/`csum_bytes` share storage since d9891ae28b0d) | spurious diagnostic; a reply to the open syzbot report | 26.15 (reply drafted, reproducer in `tools/kernel_bugs/btrfs_failed_inode_read/`) | `fault_recover`'s btrfs variant pins inodes; run-qemu.sh tolerates exactly that WARNING when the guest reports it |
| 4 | FUSE: `fuse_setattr` drops `ATTR_KILL_SGID` and re-derives with the pre-6.2 rule (S_ISGID only with S_IXGRP); it should call `setattr_should_drop_sgid()` after refreshing `i_mode` | passthrough writes never reach dcfs, so only the kernel can clear S_ISGID for a writer outside the file's group (generic/683-685) | 17.2 (dcfs's own paths fixed by running them inside `AsCaller`) | dcfs declines nothing yet; the xfstests list carries 683-685 until 17.2 |
| 5 | dcache: `__d_obtain_alias` prefers a connected alias and allocates an anonymous dentry only when none exists (as `d_find_alias` already prefers) | lets a handle open after an O_PATH name lookup return a connected fd with no identity change and no freeze protection: the shelved Phase 13 done right | 13.5 (much later) | handle opens stay disconnected; `/proc/<pid>/fd` shows `/` |
| 6 | FUSE: an INIT opt-in to receive `FUSE_DESTROY` synchronously at unmount for plain `fuse` mounts, as `fuseblk` and virtiofs do, so a daemon that completes its shutdown in the DESTROY reply makes `umount` return after it has finished | removes the need for `umount.fuse.dcfs` and the per-mount lock file; closes the restart race at the source (russ, 2026-10-10: "During unmount, does FUSE not give the daemon a callback that blocks the unmount?") | 15.6d (new) | the 15.6b helper and lock; the helper becomes the fallback for kernels without the flag; also removes the no-fusectl limitation (a mount right after an unmount can meet the earlier daemon still finishing: README, russ 2026-10-11 chose to document rather than reorder startup) |

Rules for the list: a reproducer in the tree for each bug (2, 3 have one;
4 gets one with 17.2; 5 and 6 are features and get a test that detects
the kernel's support and exercises the dcfs path both ways); the draft
mail or patch in `docs/plan/notes/` before russ sends; after it ships,
the dcfs step that negotiates or detects it, keeping the fallback.
