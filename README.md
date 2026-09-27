# Dircache FS (DCFS)

## NFS support

Inodes can never be preserved in a pass-through FUSE filesystem that supports
NFS export. The reason for this is because the NFS server is allowed to be
restarted without clients reconnecting, combined with the fact that the POSIX
filesystem API is stateful on the server side. Consider the following:

1. NFS client calls open. NFS server acknowledges with a file handle that's
   cached on the client.
2. NFS server is restarted, discarding the file handle on the server.
3. NFS client calls read on the file handle.
4. NFS server forgot about the file handle, so e.g. underlying open files have
   been closed or forgotten about.

Unfortunately, the NFS server cannot simply reject this request (although many
filesystems do, just Google for "stale file handle NFS"). If, for example, it's
an NFS server serving home directories of a large business, you can't just
restart all the machines of the business whenever the NFS server upgrades.

The fix for this is for filesystems to always remember enough information to
resuscitate a file handle just from the data that the client sends. Normally,
that is done using a file's inode number plus its "generation" number, since
filesystems are allowed to reuse inode number but not (inode,generation) tuples.

With FUSE however, when a request comes in from a client, it fails to pass
through to the FUSE userspace daemon the generation number being requested. E.g.
https://github.com/libfuse/libfuse/blob/3863da58b1f7904675ca050434d8219bc410f34a/doc/README.NFS#L30.
This means that to work around that limitation, inode numbers must never ever be
reused... and since we can't guarantee that the underlying filesystem won't
reuse its inode numbers, we have to generate our own inode numbers to pass back
to callers.
