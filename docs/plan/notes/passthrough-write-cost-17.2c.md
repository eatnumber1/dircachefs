# Step 17.2c: why a passthrough write costs a daemon round trip

Measured 2026-10-17 in the QEMU guest (kernel 6.18.55, ext4 backing, two
vCPUs, KVM), with `bazel test //test/qemu:bench_write_test` (new: the
`Write` and `SetidWrite` cases of `dcfs_bench`, 100 iterations each, 1 MiB
in 64 KiB writes on one open, so 16 writes per iteration, and the daemons'
request log on). The host was shared with other guest runs (load average
8-10), so wall times vary by 2x between runs; each figure below says which
run it is from.

## 1. Requests by operation (measured)

`--v=2` logs every request the daemon serves. Two daemons ("dcfs", "dcfs0")
each served 100 iterations x 16 writes = 3200 writes of the plain 0666 file
(`Write` case); the counts of the whole run:

| Request | Count |
|---|---|
| `Write` | 0 |
| `Getxattr` | 3200 |
| `Open`, `Flush`, `Release` | 2 each (one open per daemon) |
| `Lookup`, `Getattr`, `Opendir`, `Readdir*`, `Releasedir` | setup: 306, 616, 52, 104, 52 |

Passthrough is engaged: no `WRITE` ever reaches dcfs. There is exactly one
`GETXATTR` per write, 3200 of 3200. (The log line does not carry the xattr
name; the identification as `security.capability` rests on the kernel
source below and on the counterfactual in section 3, where negotiating
`FUSE_CAP_HANDLE_KILLPRIV_V2` takes the count from 3200 to 2.) The daemon
answers them from its cache: only 2 of the `Getxattr` requests of the
`Write` case reached the backing filesystem (the first one per daemon).

`SetidWrite` (the 06666 file, for which dcfs declines passthrough) adds
3200 `Write` requests, each reaching the backing filesystem, and 3200
`Getxattr`, each also reaching the backing filesystem (a write marks
`security.capability` unknown, so the next answer is read from the backing
file). That case costs a different amount for different reasons (below).

## 2. The cause (kernel source, confirmed by section 3)

`backing_file_write_iter` (fs/backing-file.c) calls
`file_remove_privs(iocb->ki_filp)` before every passthrough write.
`file_remove_privs_flags` (fs/inode.c) returns at once if the inode has
`S_NOSEC`; otherwise it asks `dentry_needs_remove_privs`, whose
`security_inode_need_killpriv` reads `security.capability` with a
`getxattr` on the FUSE inode, and that is a `FUSE_GETXATTR` request.
`S_NOSEC` is set by `inode_has_no_xattr` after such a check finds nothing,
but only if the superblock has `SB_NOSEC` (and the mode has no setuid or
setgid bit). `fuse_sb_defaults` clears `SB_NOSEC` (fs/fuse/inode.c:1622);
only a server that negotiated `FUSE_HANDLE_KILLPRIV_V2` gets it back
(inode.c:1372). Without it `S_NOSEC` is never set and every write pays the
`GETXATTR`. `fuse_change_attributes` (inode.c:302) clears `S_NOSEC` again
whenever the kernel refreshes an inode's attributes from the server, so
even with V2 an inode pays one `GETXATTR` after each refresh (with the
default 3600 s timeouts: about once per inode).

FUSE has no negative-xattr cache. The one memory it has of a `GETXATTR`
answer is `fc->no_getxattr`, set when the server replies `ENOSYS`
(fs/fuse/xattr.c:84): every later `getxattr` then fails at once with
`EOPNOTSUPP`, for every xattr name on the mount, `system.posix_acl_access`
included, which dcfs needs for `FUSE_CAP_POSIX_ACL`. So that is not an
option.

## 3. Quantified (measured)

`Write` through dcfs, 5 repetitions of 100 iterations, no request log,
mean wall time per MiB (so per 16 writes); the backing filesystem direct
for comparison. Same binary except for the one prototype line (below):

| Run | backing | dcfs | dcfs0 |
|---|---|---|---|
| now, host loaded: baseline | 146 us | 5391 us | 4214 us |
| later, host calmer: baseline | 125 us | 2373 us | 2000 us |
| V2 prototype (the same minute as the calmer baseline) | 96-132 us | 126-155 us | 129-133 us |

The per-write cost of the `GETXATTR` is therefore (2373 - 126) / 16 =
about 140 us on the calmer host and (5391 - 126) / 16 = about 330 us on the
loaded one; 17.2b's 9.1 ms per MiB is 570 us per write, from a busier
moment still. With V2 the excess over the backing filesystem goes to
30 us per MiB or less (the cost of the stacked passthrough file itself,
within the noise of the backing's own spread): the `GETXATTR` explains
all of it, 94% to 99% of the whole figure (the rest being the backing's
own 0.1 ms).

What a write's `GETXATTR` costs is a round trip (two context switches on a
two-vCPU guest) plus the daemon's cache read, which is why it varies with
load and why it dominates small writes: `generic/069`'s
`append_writer 3000000` issues 3,000,000 writes of 4 bytes;
at 70 us of daemon CPU each that is the ~210 s of daemon CPU
`slowness-17.3.md` measured before the 300 s timeout. That attribution is
an inference from the numbers, not a run of generic/069.

## 4. Fix options (not implemented)

### A. Negotiate `FUSE_CAP_HANDLE_KILLPRIV_V2`

One line in `DirCacheFS::Init` (`fuse_set_feature_flag(&conn,
FUSE_CAP_HANDLE_KILLPRIV_V2)`). Measured with exactly that line as a
prototype (reverted, not committed): `Write` 126-155 us per MiB against
5400 us (loaded) or 2400 us (calmer) before; `Getxattr` requests 3200 -> 2.

It is not free: it changes what the kernel sends for privilege removal,
and it breaks one thing that 17.2b's tests did not cover. With the
prototype every existing `credentials_test_ext4` case still passed (a
mount with V2 still sends the empty SETATTR that 17.2b's rule recognises,
and fallocate, copy_file_range and truncate run as the caller), but the new
cases added to `credentials.sh` by this step failed:

    TEST sgid2b-setuid-other-write FAIL (want 0:666, native 0:666, mount 0:4666, src 0:4666)
    TEST sgid2b-setuid-owner-write FAIL (want 0:666, native 0:666, mount 0:4666, src 0:4666)

A non-owner's passthrough write to a file with only setuid (04666) keeps
setuid: a security regression. The reason is in `fuse_setattr`
(fs/fuse/dir.c:2339): with V1 or V2 negotiated the kernel drops
`ATTR_KILL_SUID`, `ATTR_KILL_SGID` and the mode it computed, so the
privilege removal before a passthrough write arrives as a SETATTR that sets
nothing. 17.2b's rule takes that for "clear setgid if a write by the caller
would" (`FUSE_SET_ATTR_KILL_SGID`) and never clears setuid, which today
arrives as a mode (the kernel's pre-6.2 rule: setuid always). The cases
pass on the current code (run against it, same script) and fail only with V2.

What A would take, so that the cost is not read off the one-line change:

- Extend the empty-SETATTR rule (`DirCacheFS::Setattr`,
  `backing::SetAttrFd`, `WriteClearsSetgid`) to clear setuid as well, where
  a write by the caller would: any caller without `CAP_FSETID`. Same
  limitation as 17.2d: a uid 0 caller without `CAP_FSETID` cannot be told
  from root.
- With that, the 06666 passthrough decline (`PassthroughFor`) can go: the
  empty SETATTR now clears both bits before the daemon's write. `SetidWrite`
  would then cost what `Write` costs (now 13.7-38 ms per MiB with the
  decline; the prototype alone did not change it: 15.4-16.5 ms, because the
  declined file still takes a `WRITE` request, a backing `GETXATTR` and a
  credential switch per write).
- The truncate case (SETATTR of size and of the mode minus setuid) becomes
  a SETATTR of the size with `FATTR_KILL_SUIDGID` (the bit libfuse hands
  over as `FUSE_SET_ATTR_KILL_SUID`): dcfs's truncate already runs as the
  caller, so the bits go; the third 17.2b bullet is then dead code (the
  prototype passed the truncate cases with it still present).
- On a kernel without V2, `fuse_set_feature_flag` does nothing and the
  17.2b workarounds are still needed. V2 is offered by every kernel that
  has FUSE passthrough (V2 is protocol 7.33; passthrough 7.40, Linux 6.9),
  so dcfs can require V2 wherever it takes passthrough and keep one path.
- `design.md` ("Caller credentials") and `README.md` text
  changes, `kernel-patches.md` row 4's "meanwhile" changes, the new
  credentials cases stay as the guard, and no model change is expected (the
  setattr's phases do not change).

Size: small to medium; touches the identity of what a chmod clears, so a
`dcfs-protocol` step with a review. Nothing in A needs a retry or a timer.
After it the per-write cost is the passthrough's own, plus one `GETXATTR`
per inode per attribute refresh.

### B. Cache the negative answer in the kernel

Not available: FUSE has no xattr cache, and `ENOSYS` (the one cached
answer) disables every xattr including the ACLs dcfs needs. A kernel patch
(keep `SB_NOSEC` without V2) would only reproduce what V2 gives, and
`kernel-patches.md` rows are for things dcfs cannot do on its own.

### C. Nothing

The cost is 140-330 us per write however small the write, from one cached
`GETXATTR`. A program that writes in 64 KiB or larger blocks pays 3% to 10%
on a 1 MiB write at the calmer host's figures (not measured at other
sizes); one that writes 4 bytes at a time pays 70 us of daemon CPU per
write, which is the generic/069 timeout. The weekly slow set already lists
generic/069.

### Recommendation

A, as its own step (dcfs-protocol, with the credentials cases in this step
as the failing-first guard), and retire the 06666 decline in the same
change if the extended rule passes the sgid2b cases without it.

## What is committed with this note

- `dcfs_bench --dcfs_flags=A,B`: extra flags for the daemons `Setup` starts.
- `//test/qemu:bench_write_test` (large, by name; `guest/bench_write.sh`,
  and a hook in `guest/bench.sh`): the request counts of section 1.
- Six `setuid-*` cases in `guest/credentials.sh` (04666: other and owner
  write, fallocate, copy_file_range, truncate, root write). They pass on
  the current code and fail with the V2 prototype (section 4A).
