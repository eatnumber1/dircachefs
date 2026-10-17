# What the kernel's FUSE caches can hold stale on a dcfs mount (2026-10-17)

For 26.39's attr/entry timeout default and 12.20's model. dcfs sends no
`fuse_lowlevel_notify_*` invalidation (docs/design.md, around lines
2128-2148: a synchronous notification inside a request could deadlock
against the kernel's locks for that request). Its only tools are a zero
attr timeout while a file is open for writing (`AttrTimeoutFor`) and on two
failure paths, and `keep_cache = 0` at open.

**How this was found.** A Gemini 3.8 Flash job (medium) listed 38 places
where dcfs changes or learns something the kernel caches without the
kernel's request (main 8278bcc). A Gemini 4 Argon job (medium) then checked
each against the kernel's FUSE code (`~/Sources/linux`, fs/fuse and
fs/backing-file.c). The orchestrator spot-checked the citations
(`fuse_dir_changed` at fs/fuse/dir.c:325, `fuse_invalidate_atime` in
readdir.c:429 and passthrough.c:18, no ctime update in fs/fuse/ioctl.c).

**Result.** 29 of the 38 are handled by the kernel's own FUSE code: a
parent's attributes after create, unlink, rename and link
(`fuse_dir_changed`); a renamed or unlinked inode's ctime and nlink
(`fuse_update_ctime`, `fuse_entry_unlinked`); access times after readdir,
readlink and passthrough reads (`fuse_invalidate_atime`); size and times
after passthrough writes, fallocate and copy_file_range
(`fuse_write_update_attr`); ctime and ACLs after setxattr and removexattr;
the page cache at open (`keep_cache = 0`); a negative dentry after EEXIST
(`fuse_invalidate_entry`). `security.capability` is not cached by FUSE.
Seven are changes made to the backing behind dcfs's back, which dcfs does
not support. Two are real:

1. **ctime after `FS_IOC_SETFLAGS` / `FS_IOC_FSSETXATTR`**
   (`DirCacheFS::Ioctl`): neither `vfs_fileattr_set` nor
   `fuse_fileattr_set` invalidates the cached ctime, so it stays stale
   for up to the attr timeout (unless the file is open for writing).
2. **Stores through a passthrough `MAP_SHARED` mapping after close**: the
   backing's mtime, ctime and blocks change, the kernel learns nothing,
   and dcfs refreshes its row only at FORGET (`ReconcileWritten`), so a
   stat in between is stale until the inode is evicted, whatever the
   timeout.

**What follows.** An effectively infinite attr/entry timeout is safe for
everything but these two. Item 1 needs an invalidation of the inode's
attributes after the reply (`notify_inval_inode` deferred to the point
`FinishSyncPoint` runs, outside the request's locks), or a zero attr
timeout in the reply that follows; item 2 is staleness in dcfs's own
cache, independent of the kernel's timeout.
