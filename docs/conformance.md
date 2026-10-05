# POSIX conformance (pjdfstest)

Step 4.5. Measures dcfs's POSIX filesystem-call conformance with
[pjdfstest](https://github.com/pjd/pjdfstest) (Pawel Jakub Dawidek's
filesystem test suite from FreeBSD -- ~8800 checks of chmod/chown/link/
mkdir/open/rename/unlink/... semantics and errnos, used by ZFS, gVisor and
FUSE filesystems), run against a dcfs mount over ext4 (step 5.2: also xfs
and btrfs -- see "Step 5.2: ext4, xfs and btrfs" near the end).

- Suite pinned at commit `85a8aea9e685999ef0540392fd80535f873d7ff7`
  (github.com/pjd/pjdfstest, the tip of `master` on 2026-09-27).
- Measured against dircachefs commit `2a4f672` (step 4.6) plus this step's
  own commits, on 2026-09-27/28; re-measured in step 4.7 (caller
  credentials, see "Step 4.7" below) on 2026-10-02; extended to xfs and
  btrfs in step 5.2.

## How to run

```
sg kvm -c 'bazel test //test/qemu:pjdfstest_test_ext4'
sg kvm -c 'bazel test //test/qemu:pjdfstest_test_xfs'
sg kvm -c 'bazel test //test/qemu:pjdfstest_test_btrfs'
```

(`pjdfstest_test`, with no suffix, is an alias for `pjdfstest_test_ext4`;
see "Step 5.2" below.)

See `test/qemu/guest/pjdfstest.sh` for the mechanics and
`third_party/pjdfstest/BUILD.pjdfstest` for how the (autoconf-less) binary
is built. Serial log:
`bazel-testlogs/test/qemu/pjdfstest_test/test.outputs/serial.log`.

### How it decides pass/fail

The full suite (238 `.t` files, 8827 individual TAP checks) is run twice
against two directories on the *same* backing ext4 filesystem (`/dev/vdb`
in the test VM):

- `/mnt/dcfs-work`, through the dcfs mount -- the thing under test.
- `/src/ext4-work`, directly on the backing ext4 filesystem, bypassing
  dcfs entirely.

(Until step 4.7 the guest script never actually mounted `/dev/vdb` on
`/src`, so both runs went to the initramfs's tmpfs root instead of ext4;
see "Step 4.7" below.)

`tests/conf` is patched (see
`third_party/pjdfstest/0001-linux-portability.patch`) to hard-code
`fs="EXT4"` for both runs, rather than let pjdfstest's own `df`-based
detection report the dcfs run as some `FUSE.*` filesystem type: dcfs over
ext4 is supposed to *behave* like ext4, so both runs must make identical
`supported()`/`todo()` decisions in `tests/misc.sh`, and any remaining
difference between the two failure sets is then a real behavioral
difference, not a detection artifact. The same patch also swaps
`tests/misc.sh`'s `namegen()`/`namegen_len()` from `openssl md5` (not
present in this project's busybox-only guest image) to busybox's own
`md5sum`.

A failure that reproduces on **both** runs is ext4/Linux/pjdfstest
semantics (or a pjdfstest quirk on an OS/fs combination it doesn't
specifically know about), not dcfs's fault, and is filtered out
automatically by diffing the two failure sets. What's left -- failing
against dcfs but not against raw ext4 -- is the **dcfs-specific** set,
checked against the baseline in `test/qemu/guest/pjdfstest.expected_failures`
(step 5.2: renamed `pjdfstest.ext4.expected_failures`, alongside new
`pjdfstest.xfs.expected_failures`/`pjdfstest.btrfs.expected_failures` --
every mention of either bare filename in this historical section is the
step 4.5-7 name, still accurate for what was true then): any dcfs-specific
failure not already in that baseline fails the test (a regression); any
baseline entry that no longer fails is printed as an `info:` line so the
baseline can be tightened. `test/qemu/guest/pjdfstest.ext4_failures`
(step 5.2: renamed `pjdfstest.ext4.backing_failures`) is a parallel record
of the ext4-only failures, kept for this document; the guest script does
not read it back (it always recomputes the live ext4 failure set for the
regression diff, so a
kernel/ext4 version drift can never hide a real dcfs regression behind a
stale snapshot).

The guest script also greps dcfs's own stderr for "out-of-band change"
warnings (step 4.6): pjdfstest's dcfs run only ever goes through the
mount, so any such warning would itself be a dcfs bug (a false positive in
`ReconcileAttrs`/`VerifyBackingIdentity`). None were observed.

(Step 5.2: the description above is written for ext4, the original and
still the default backing filesystem; `guest/pjdfstest.sh` now detects
the actual backing filesystem at runtime -- see `backing_fstype` in
`guest/lib.sh` -- and the same mechanics apply verbatim to xfs and btrfs,
substituting the detected name everywhere this section says "ext4": the
work directory, the result files, and which checked-in baseline file
(`pjdfstest.<fstype>.expected_failures`) gates pass/fail. `tests/conf`
still hard-codes `fs="EXT4"` regardless of the real backing filesystem --
see `0001-linux-portability.patch` -- which does not undermine the
diffing logic: both the dcfs run and the direct-backing run make the
*same* `fs="EXT4"`-driven `supported()`/`todo()` decisions either way, so
a difference between their two failure sets is still a real behavioral
difference and not a detection artifact.)

## Counts (step 4.7)

| | checks | failed |
|---|---:|---:|
| dcfs mount | 8827 | 28 |
| raw ext4 (no dcfs) | 8827 | 28 |
| dcfs-specific (dcfs-only failures) | -- | **0** |
| backing-only (fail on ext4, pass through dcfs) | -- | 0 |

dcfs fails exactly the checks raw ext4 fails, all of them pjdfstest TODOs
for documented Linux behavior (see "The 28 ext4 failures" below), and
`pjdfstest.expected_failures` is empty. The guest script also lists the
reverse set -- checks that fail directly on the backing filesystem but
pass through dcfs (`note:` lines) -- since passing *more* than the backing
filesystem is a divergence from it too, not better conformance.

Timings (KVM, this host, `-smp 2 -m 1024`): dcfs run ~199s, ext4-direct run
~188s, ~400s total wall time for the whole `pjdfstest_test` target --
comfortably under the `enormous`/`eternal` size/timeout (see "Judgment
calls" below), so no parallelization across `.t` directories was needed.

Step 4.5's numbers, for the record (then measured on tmpfs, see below):
dcfs 178 failed, backing 43, dcfs-specific 154.

## Fixes made in step 4.5

Two dcfs bugs were found and fixed (each a small, targeted change, tested
in isolation before folding back into the full run):

1. **ENAMETOOLONG lost as ENOENT for a name past NAME_MAX, once the parent
   directory's listing is cached** (`dcfs/backing.cc`,
   `LookupOrPopulate`). Once a directory's listing is known complete,
   looking up a name not in it short-circuits straight to a cached
   negative entry -- which the kernel turns into plain ENOENT -- without
   ever giving the real backing filesystem a chance to reject a
   syntactically invalid (too long) name with ENAMETOOLONG. A name that
   reaches an actual backing syscall (create, mkdir, link, ...) already
   got ENAMETOOLONG for free from the real filesystem; only this
   pure-lookup fast path needed an explicit `name.size() > NAME_MAX`
   check. Found via `chmod/02.t:5` and the identical pattern in
   `chown/02.t`, `ftruncate/02.t`, `link/02.t`, `rename/01.t`,
   `rmdir/02.t`, `truncate/02.t`, `unlink/02.t` (9 checks fixed).
2. **The daemon's own process umask silently re-masked already-final
   create modes** (`dcfs/main.cc`). The kernel already applies the
   *calling* process's umask to a CREATE/MKDIR/MKNOD/OPEN(O_CREAT) mode
   before it ever reaches dcfs (documented at `backing.h`'s
   MkdirAt/MknodAt/SymlinkAt declarations) -- but dcfs performs the
   matching real syscall on the backing filesystem as its *own* process,
   which applies *its own* inherited umask (typically 022) a second time,
   clearing bits from a mode that was already final. `umask(0)` at the
   top of `main()` fixes it. Found via `open/02.t:2` and `open/03.t:2`
   (`open(path, 0642)` landing as 0640 on the backing file).
   (Since step 6.3 the kernel no longer applies the umask itself: dcfs
   requests `FUSE_CAP_DONT_MASK`, needed for POSIX ACL inheritance, and
   switches to the caller's umask around the backing syscall; the daemon's
   own umask stays 0 outside that.)

Re-running the full suite after both fixes dropped the dcfs-specific count
from the original 175 to 154 (21 checks fixed net of the two groups above
-- a few checks downstream of the same root causes, e.g. later assertions
in the same `.t` file that only run once an earlier one in the same
sequence passes, were fixed incidentally).

## The 154 of step 4.5: one limitation (fixed in step 4.7)

(As written in step 4.5, describing the code before step 4.7.) Every one
of the 154 remaining dcfs-specific failures is a symptom of the
**same** underlying limitation, not 154 separate bugs:

> **dcfs runs as root and performs every backing-filesystem write as
> root, with no per-request uid/gid (or supplementary-group)
> impersonation of the FUSE caller.**

Concretely: `dcfs/dir_cache_fs.cc` and `dcfs/backing.cc` never look at the
FUSE request's calling uid/gid (`fuse_req_ctx`) anywhere. A file or
directory created by a non-root user *through* dcfs is created by the
*daemon* on the real backing filesystem, so it ends up owned by root
regardless of who asked (confirmed directly: `mkdir/00.t:19`, testing "the
directory's group ID shall be set to ... the effective group ID of the
process" after a `-u 65534 -g 65534 mkdir`, gets `uid,gid = 0,0` instead
of `65534,65534`). Every later check that assumes the creator is the
owner -- a `chmod`/`chown`/`truncate`/`open` "as the owner, should
succeed" or "as a non-owner, should be denied" -- then sees the wrong
owner and gets the wrong answer.

This explains the failure groups pjdfstest turned up (see
`test/qemu/guest/pjdfstest.expected_failures` for the exact list):

| Group | Files | Symptom |
|---|---|---|
| Owner not set on create | `mkdir/00.t`, `mkfifo/00.t`, `mknod/00.t`, `open/00.t` (`:19,22,26`) | `lstat` after a non-root create reports uid/gid `0,0` instead of the creator's. |
| chmod/chown as the (nominal) owner | `chmod/05.t`, `chmod/07.t`, `chown/05.t` | A non-root user who created (and so, correctly, should own) a file gets EPERM chmod-ing or chown-ing it, since dcfs still thinks root owns it. |
| truncate/ftruncate as the (nominal) owner | `truncate/05.t`, `ftruncate/05.t` | Same shape: EACCES truncating a file the acting user actually created. |
| open() permission matrix | `open/06.t`, `open/07.t` | The single largest group (89 checks): a full owner/group/other x mode-bit matrix, all downstream of dcfs enforcing "owned by root" instead of the real creator -- both spurious EACCES (denied to the real owner) and spurious success (granted to an unrelated user who happens to share `other` bits with what should have been the *owner*'s bits). |
| rename as the (nominal) owner, sticky-style checks | `rename/09.t`, `rename/10.t` | `rename/10.t` in particular cascades: an EACCES/EPERM on the rename itself leaves later `lstat`/inode-number assertions checking a rename that never happened. |
| rmdir permission | `rmdir/11.t:6` | Same root cause, rmdir side. |

Step 4.7 fixed it; see below.

## Step 4.7: caller credentials, and two harness fixes

**The fix.** Every backing syscall whose outcome depends on who makes it
now runs with the thread's filesystem credentials switched to the FUSE
caller's (`setfsgid`, the caller's supplementary groups via the raw
per-thread `SYS_setgroups`, then `setfsuid`; root restored right after;
`AsCaller` in `dcfs/backing.cc`, credentials from
`FuseRequest::Caller()`): `mkdirat`, `mknodat`, `symlinkat`,
`openat(O_CREAT)`, `unlinkat`, `renameat2`, `fchownat`, `futimens`,
`ftruncate`, `fsetxattr`/`fremovexattr` (and their `/proc` path forms).
Reaching objects by handle, `linkat(AT_EMPTY_PATH)`, chmod and all probes
stay root; the reasons are at `AsCaller` and in `backing.h` (`LinkAt`,
`SetAttr`). All 154 entries pass; `test/qemu/guest/credentials.sh` checks
the same rules directly with two unprivileged users.

**chown(path, -1, -1).** With the 154 gone, the tmpfs-based measurement
showed dcfs failing *fewer* checks than the backing filesystem (24 vs
43). 19 of the difference were chown/00.t's "If both owner and group are
-1, the times need not be updated" TODO checks: Linux updates ctime, and
the backing filesystem did, but dcfs silently did nothing -- the kernel
forwards that chown as an otherwise empty SETATTR (`FATTR_CTIME` is only
sent with writeback caching), which `backing::SetAttr` ignored. That was a
real divergence from the backing filesystem, hidden only because POSIX
also allows it; dcfs now applies such a request as the same
`fchownat(-1, -1)`, as the caller (setattr.sh `chown-noop-ctime`), and
fails those 19 checks exactly like ext4.

**Harness: tmpfs, not ext4.** `guest/pjdfstest.sh` never mounted
`/dev/vdb` on `/src`, so both runs of step 4.5 (and dcfs's backing
filesystem) were the initramfs's tmpfs root, despite this document. It
now mounts vdb and prints the filesystem type (`ext2/ext3` = ext4's magic).
The same 28 checks fail on both.

**Harness: no passwd database.** utimensat/06.t and 07.t look up
`nobody` and `root` with `id -u`; the busybox guest had no `/etc/passwd`,
so those checks ran with an empty uid and failed on both runs (the
"expected EACCES, got" lines with nothing after "got"). The script now
writes minimal `/etc/passwd`/`/etc/group` entries; those 15 checks now
run for real and pass on both -- and, being UTIME_NOW/explicit-time
permission checks for a non-owner, exercise the caller-credential path
too.

## The 28 ext4 failures

Recorded in `test/qemu/guest/pjdfstest.ext4_failures` for reference; not
dcfs's concern (they reproduce identically with dcfs entirely out of the
picture, and dcfs fails the identical set). All are pjdfstest `todo Linux`
checks:

- `chown/00.t` 650-690 (8 checks): Linux does not clear setuid/setgid on
  a directory when an unprivileged owner chowns it.
- `chown/00.t` 1054-1144 (19 checks): `chown(path, -1, -1)` updates
  ctime on Linux (POSIX permits not updating it).
- `unlink/08.t:2`: `unlink(2)` on a directory is EISDIR on Linux rather
  than EPERM.

Step 4.5 recorded 43 here: these 28 (measured on tmpfs, see above) plus
15 utimensat/06.t and 07.t checks that only failed for lack of a
`nobody` user in the guest.

## Judgment calls

- **`pjdfstest_test` was a hand-written `sh_test`, not a `qemu_test(...)`
  instance, as of step 4.5.** `qemu_test` (`test/qemu/qemu_test.bzl`) then
  hard-coded `size = "large"` / `timeout = "long"` (900s) with no way to
  override either, and `qemu_test.bzl` was off limits for this step (a
  concurrent agent was editing it, `test/qemu/run-qemu.sh`,
  `test/qemu/guest/init` and `test/qemu/kernel.bzl` on another branch for
  an NFS test). Since two ~8800-check suites plus a directory-complete FUSE
  round trip for every check comfortably exceeds 900s in the worst case,
  `pjdfstest_test` was instead a direct `sh_test` in
  `test/qemu/BUILD.bazel` that mirrored what the macro generates (same
  `srcs`/`data`/`tags`/`args` shape, same `scripts/run-qemu.sh` entry
  point) with `size = "enormous"` / `timeout = "eternal"` (3600s) instead.
  In practice the run finished in ~420s, so no `.t`-directory
  parallelization (also contemplated by the plan) was needed. `qemu_test`
  gained `size`/`timeout` parameters before step 5.2, so `pjdfstest_test`
  is a normal `qemu_test_matrix(...)` instance (`size = "enormous"`,
  `timeout = "eternal"`, same as before) like every other step 5.2 target;
  see "Step 5.2" below.
- **`config.h` is hand-written, not autoconf-generated.** pjdfstest is
  normally built via `./configure && make`; this project has no autoconf
  in its Bazel graph, so `third_party/pjdfstest/BUILD.pjdfstest` writes
  the `HAVE_*`/`HAVE_STRUCT_STAT_ST_*` defines a Linux/glibc host would
  get from `configure.ac`'s `AC_CHECK_FUNC`/`AC_CHECK_MEMBERS` probes by
  hand (every `*at()` syscall, `posix_fallocate`, `utimensat`,
  `<sys/sysmacros.h>`, POSIX `st_atim`/`st_ctim`/`st_mtim`). BSD-only
  facilities pjdfstest also knows about (`lchmod`, `chflags` and friends,
  `bindat`/`connectat`, `lpathconf`, NFSv4 ACLs) have no Linux equivalent
  and are deliberately left undefined -- every `.t` file that needs one of
  those calls `tests/misc.sh`'s `require()`, which quick-exits with a
  trivial single-check pass on any OS/fs it isn't gated to, so this loses
  no real coverage.
- **The `tests/conf`/`tests/misc.sh` patch is a repo patch on the
  `http_archive`, not a fork.** Kept as a single small unified diff
  (`third_party/pjdfstest/0001-linux-portability.patch`) rather than
  maintaining a full local copy of pjdfstest's `tests/` tree.
- **The ext4-only baseline file is not read back at runtime.** Only
  `pjdfstest.expected_failures` gates pass/fail; `pjdfstest.ext4_failures`
  is written once here as a record for this document and is otherwise
  inert, so a future kernel/ext4 version change can never cause a real
  dcfs regression to be silently absorbed into a stale "that's just an
  ext4 quirk" snapshot.

## Step 5.2: ext4, xfs and btrfs

`pjdfstest_test` is now `qemu_test_matrix(...)`-generated
(`pjdfstest_test_ext4`/`_xfs`/`_btrfs`, plus a `pjdfstest_test` alias to
the ext4 variant): the plan always called for all three backing
filesystems, and until this step only ext4 was exercised.
`guest/pjdfstest.sh` needed no change to its actual test logic -- only to
stop hard-coding "ext4" in directory/file names and in which checked-in
baseline file it reads (see `backing_fstype` in `guest/lib.sh`, and the
per-filesystem `pjdfstest.<fstype>.expected_failures` /
`pjdfstest.<fstype>.backing_failures` files, renamed from the old
unqualified `pjdfstest.expected_failures`/`pjdfstest.ext4_failures`). The
mechanics above (two runs on the same backing filesystem, diffed, checked
against a baseline) are otherwise identical for all three.

| | ext4 | xfs | btrfs |
|---|---:|---:|---:|
| checks | 8827 | 8827 | 8827 |
| failed (dcfs mount) | 28 | 30 | 32 |
| failed (raw backing, no dcfs) | 28 | 30 | 32 |
| dcfs-specific (dcfs-only failures) | **0** | **0** | **0** |
| backing-only (fail on backing, pass through dcfs) | 0 | 0 | 0 |
| suite wall time (dcfs run + backing run) | ~390s | ~386s | ~393s |

`dcfs-specific` is **0 on all three filesystems**: dcfs introduces no
POSIX conformance regressions of its own on ext4, xfs or btrfs. Every
failure dcfs exhibits is a failure the backing filesystem exhibits too,
with nothing left over once the known backing-only noise (below) is
subtracted.

**xfs: 2 extra backing-only failures, not dcfs's fault.**
`symlink/03.t:1-2` fail directly against raw xfs (confirmed independent of
dcfs: dcfs fails the identical set, nothing more) -- see
`pjdfstest.xfs.backing_failures` for the detailed note. In short: the test
builds a path exactly `PATH_MAX` (4096) bytes long via deeply nested
directories to check `ENAMETOOLONG` one byte past it, and xfs hits some
other path-construction limit slightly before reaching `PATH_MAX`, failing
checks 1-2 (which are not themselves the `ENAMETOOLONG` assertions -- those,
checks 5-6, are never reached). This is upstream xfs/pjdfstest interaction
having nothing to do with dcfs, exactly the same way the 28 ext4 failures
(`chown/00.t`, `unlink/08.t`) are upstream Linux/ext4/pjdfstest
interactions: both are filtered out of the dcfs-specific count by the
diffing logic because they reproduce identically with dcfs entirely absent.

**btrfs: 4 extra backing-only failures, also not dcfs's fault -- and a
harness interaction worth calling out.** `rename/24.t:4,5,8,9` fail
directly against raw btrfs. `tests/rename/24.t` checks a directory's
`nlink` after a rename that moves it to a new parent, and pjdfstest
*already* ships a `case "$fs" in btrfs|BTRFS) todo Linux "Btrfs uses CoW;
link count semantics differ from POSIX." ...` branch for exactly this --
real btrfs's CoW design gives directories different nlink semantics after
a cross-directory rename than ext4/xfs/POSIX do, and pjdfstest's own
authors already knew it. But this project's `tests/conf` patch
(`third_party/pjdfstest/0001-linux-portability.patch`) hard-codes
`fs="EXT4"` for both runs -- deliberately, so the dcfs run and the
backing run make identical `supported()`/`todo()` decisions everywhere
else and a difference between their failure sets stays meaningful (see
"How it decides pass/fail" above) -- which means pjdfstest's own
btrfs-aware branch in this one file never triggers on the btrfs variant:
it runs the ext4/POSIX assertions instead, and real btrfs fails them.
Confirmed independent of dcfs (fails identically with dcfs entirely out of
the picture, directly against backing btrfs): this is upstream btrfs
CoW-vs-POSIX nlink behavior pjdfstest itself already documents, surfaced
by this harness's (correct, for its purpose) choice to pin `fs="EXT4"`
rather than report the real backing filesystem to pjdfstest.

**`dcfs-specific` is 0 on all three filesystems**, as the table above
shows: dcfs introduces no POSIX conformance regressions of its own on
ext4, xfs or btrfs.

**Reproducing:**

```
sg kvm -c 'bazel test //test/qemu:pjdfstest_test_ext4'
sg kvm -c 'bazel test //test/qemu:pjdfstest_test_xfs'
sg kvm -c 'bazel test //test/qemu:pjdfstest_test_btrfs'
```

Serial logs:
`bazel-testlogs/test/qemu/pjdfstest_test_<fstype>/test.outputs/serial.log`.
