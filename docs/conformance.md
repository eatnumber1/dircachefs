# POSIX conformance (pjdfstest)

Step 4.5. Measures dcfs's POSIX filesystem-call conformance with
[pjdfstest](https://github.com/pjd/pjdfstest) (Pawel Jakub Dawidek's
filesystem test suite from FreeBSD -- ~8800 checks of chmod/chown/link/
mkdir/open/rename/unlink/... semantics and errnos, used by ZFS, gVisor and
FUSE filesystems), run against a dcfs mount over ext4.

- Suite pinned at commit `85a8aea9e685999ef0540392fd80535f873d7ff7`
  (github.com/pjd/pjdfstest, the tip of `master` on 2026-09-27).
- Measured against dircachefs commit `2a4f672` (step 4.6) plus this step's
  own commits, on 2026-09-27/28.

## How to run

```
sg kvm -c 'bazel test //test/qemu:pjdfstest_test'
```

See `test/qemu/guest/pjdfstest.sh` for the mechanics and
`third_party/pjdfstest/BUILD.pjdfstest` for how the (autoconf-less) binary
is built. Serial log:
`bazel-testlogs/test/qemu/pjdfstest_test/test.outputs/serial.log`.

### How it decides pass/fail

The full suite (238 `.t` files, 8826 individual TAP checks) is run twice
against two directories on the *same* backing ext4 filesystem (`/dev/vdb`
in the test VM):

- `/mnt/dcfs-work`, through the dcfs mount -- the thing under test.
- `/src/ext4-work`, directly on the backing ext4 filesystem, bypassing
  dcfs entirely.

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
checked against the baseline in `test/qemu/guest/pjdfstest.expected_failures`:
any dcfs-specific failure not already in that baseline fails the test (a
regression); any baseline entry that no longer fails is printed as an
`info:` line so the baseline can be tightened. `test/qemu/guest/
pjdfstest.ext4_failures` is a parallel record of the ext4-only failures,
kept for this document; the guest script does not read it back (it always
recomputes the live ext4 failure set for the regression diff, so a
kernel/ext4 version drift can never hide a real dcfs regression behind a
stale snapshot).

The guest script also greps dcfs's own stderr for "out-of-band change"
warnings (step 4.6): pjdfstest's dcfs run only ever goes through the
mount, so any such warning would itself be a dcfs bug (a false positive in
`ReconcileAttrs`/`VerifyBackingIdentity`). None were observed.

## Counts (this run)

| | checks | failed |
|---|---:|---:|
| dcfs mount | 8826 | 178 |
| raw ext4 (no dcfs) | 8826 | 43 |
| dcfs-specific (dcfs-only failures) | -- | **154** |

Timings (KVM, this host, `-smp 2 -m 1024`): dcfs run ~216s, ext4-direct run
~204s, ~420s total wall time for the whole `pjdfstest_test` target --
comfortably under the `enormous`/`eternal` size/timeout (see "Judgment
calls" below), so no parallelization across `.t` directories was needed.

## Fixes made in this step

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

Re-running the full suite after both fixes dropped the dcfs-specific count
from the original 175 to 154 (21 checks fixed net of the two groups above
-- a few checks downstream of the same root causes, e.g. later assertions
in the same `.t` file that only run once an earlier one in the same
sequence passes, were fixed incidentally).

## The remaining 154: one known, unfixed limitation

Every one of the 154 remaining dcfs-specific failures is a symptom of the
**same** underlying, unfixed limitation, not 154 separate bugs:

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

Fixing this properly needs per-request credential impersonation around
every backing write (`setfsuid(2)`/`setfsgid(2)`, thread-local on Linux,
matching how e.g. `nfsd` does it) plus `setgroups(2)` for the
multi-group `-g g1,g2` test cases -- a real feature with real
thread-safety and threading-model questions of its own, not a
"two-attempt" bug fix. It is left as a known limitation, tracked by the
baseline file, for a future step.

## The 43 ext4-only failures

Recorded in `test/qemu/guest/pjdfstest.ext4_failures` for reference; not
dcfs's concern (they reproduce identically with dcfs entirely out of the
picture). Two groups, both permission-check edge cases for a non-root,
non-owning-but-group-related user, observed on this project's minimal
QEMU test kernel:

- `chown/00.t` (27 checks, lines 650-690 and 1054-1144): setuid/setgid-bit
  clearing behavior on `chown` by a non-root user with specific group
  membership combinations.
- `unlink/08.t:2`: a sticky-directory unlink permission edge case.
- `utimensat/06.t`, `utimensat/07.t` (14 checks): `UTIME_NOW`/explicit-
  timestamp permission checks for a non-owner with write access granted
  only via group permissions.

## Judgment calls

- **`pjdfstest_test` is a hand-written `sh_test`, not a `qemu_test(...)`
  instance.** `qemu_test` (`test/qemu/qemu_test.bzl`) hard-codes
  `size = "large"` / `timeout = "long"` (900s) with no way to override
  either, and `qemu_test.bzl` was off limits for this step (a concurrent
  agent was editing it, `test/qemu/run-qemu.sh`, `test/qemu/guest/init`
  and `test/qemu/kernel.bzl` on another branch for an NFS test). Since two
  ~8800-check suites plus a directory-complete FUSE round trip for every
  check comfortably exceeds 900s in the worst case, `pjdfstest_test` is
  instead a direct `sh_test` in `test/qemu/BUILD.bazel` that mirrors what
  the macro generates (same `srcs`/`data`/`tags`/`args` shape, same
  `scripts/run-qemu.sh` entry point) with `size = "enormous"` /
  `timeout = "eternal"` (3600s) instead. In practice the run finishes in
  ~420s, so no `.t`-directory parallelization (also contemplated by the
  plan) was needed.
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
  (`third_party/pjdfstest/0001-linux-portability.patch`), the same
  pattern this repo already uses for the libfuse BCR overlay
  (`third_party/libfuse/0001-attr-generation.patch`), rather than
  maintaining a full local copy of pjdfstest's `tests/` tree.
- **The ext4-only baseline file is not read back at runtime.** Only
  `pjdfstest.expected_failures` gates pass/fail; `pjdfstest.ext4_failures`
  is written once here as a record for this document and is otherwise
  inert, so a future kernel/ext4 version change can never cause a real
  dcfs regression to be silently absorbed into a stale "that's just an
  ext4 quirk" snapshot.
