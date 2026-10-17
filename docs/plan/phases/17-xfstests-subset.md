# Phase 17 — xfstests subset

After Phase 15 (The `mount.dcfs` wrapper), which gives xfstests a mount helper:
`FSTYP=fuse`, `FUSE_SUBTYP=.dcfs`): run the generic tests
that apply, with per-filesystem expected-failure lists in the style of
pjdfstest's; triage every failure as a dcfs bug (test first, then fix) or
a documented limitation.
Owner: Sonnet. Order: after Phase 15 (The `mount.dcfs` wrapper), which gives xfstests a mount helper.

## 17.1 The generic group against dcfs (merged 2026-10-10, 5ea0719)

Built in lane-4 (eight commits; two Opus review rounds, then a rebase over
15.6b/15.8/15.9 and a merge without further review). As merged: an Alpine
tools repository (`@alpine_xfstests_tools`), about 125 helper programs
from xfstests' src/ built static from the pinned @xfstests, a two-hunk
guest patch (musl bash's echo and oom_score_adj; `_fs_type` reporting
`fuse.dcfs`), a musl-to-glibc strerror LD_PRELOAD shim (interim; 17.1b
replaces it), a Python rootfs builder (`mke2fs -d`), guest scripts with
per-backing expected-failure lists, per-backing notrun lists (about 572
entries each, with reasons) and an exclusion list of six, a 25-fixture
gate self-check run under the pinned busybox and gawk that fails on
unlisted or changed notruns and on listed timeouts, a `mount` wrapper
giving each FUSE mount its cache_db/suid/dev (no allow_other: 15.8 always
passes it), `check` in batches of ten under a 200 s per-test limit with a
blocking `read -t` watchdog, raw-device tests ending their batch (the
gate caught generic/740 mkfs'ing the scratch device under generic/770),
six shards per backing, 1024 MiB guests, `/cache` on tmpfs, daemon CPU
ticks per test in the results line, 18 of 18 shards green. The image's
`umount` wrapper went when 15.6b's `umount.fuse.dcfs` helper covered it,
and the mount retry went with 15.6b's mount-namespace fix. Results per
backing: 152 pass, 14 listed failures, about 572 not run (reflink,
dmsetup, godown, quota, fuse-sized mkfs). The 40 slow tests sit in a
manual `xfstests_slow_<fstype>` set (eight shards, 1500 s limit, lists
empty until 17.3). All xfstests targets are incompatible under asan/ubsan
(the shim's `__asan_report_load1` relocation failure measured; 17.1c).
A dedicated `xfstests` CI job runs one runner per backing (25-45 min of
tests, 70-90 with a cold build); test.sh's default selection excludes the
xfstests and manual tags, with a unit test. Two justified timers remain
in the guest scripts and are on the sleep allowlist. Findings: 17.2
(setgid), 17.3 (the slow set).

## 17.2 Setgid kept after an unprivileged write (bug, from 17.1)

generic/683-685: fallocate, punch and zero by a caller outside the file's
group leave S_ISGID set where the backing clears it; write(2) and
truncate have the same gap. Why (review): the kernel's VFS computes
ATTR_KILL_SGID with today's rule (`setattr_should_drop_sgid`: drop if
S_IXGRP, or the caller is neither in the group nor CAP_FSETID), but
`fuse_setattr` throws ATTR_KILL_* away because dcfs negotiates neither
HANDLE_KILLPRIV nor _V2 and re-derives with the pre-6.2 rule (S_ISUID
always, S_ISGID only with S_IXGRP); and dcfs's `FallocateFd`, `WriteFile`
(fallback) and `CopyFileRangeFd` run as root with CAP_FSETID, outside
`AsCaller` (only chown, truncate and utimes are inside it), so the
backing's own `file_remove_privs` keeps the bit too. Fix: run those three
inside `AsCaller` (a non-root fsuid drops CAP_FSETID; the backing applies
the current rule with the caller's fsgid and groups); no protocol or model
change (`BeginAttrChange` marks the record unknown, phase 3 reads the mode
back); a design.md note under "Caller credentials". Not HANDLE_KILLPRIV_V2
alone (the kernel then sends nothing for fallocate or passthrough writes).
Passthrough writes never reach dcfs, so the kernel's legacy rule decides:
either decline passthrough for writable opens of S_ISGID files, or the
one-line kernel fix (`fuse_setattr` calling `setattr_should_drop_sgid`
after refreshing i_mode), worth sending upstream alongside the FUSE
generation patch (russ sends). Tests first: harness
`FallocateByCallerOutsideGroupClearsSetgid` (forged uid/gid 1000 on a
root:root 02666 file; expect 0666 on the backing and in the cache) with
group-member and root controls, the same for Write and CopyFileRange;
guest `credentials.sh` checks for fallocate/punch/zero/write/
copy_file_range/truncate comparing against the same operation on a native
directory, rechecked after a cold cache; then 683-685 leave the lists.
Owner: dcfs-implementer (Opus review of the credentials change), after
17.1 merges and 23.11's code half lands (backing.cc).
Status 2026-10-11: done, merged bc010b9 (lane-2). As built: FallocateFd,
WriteFile (fallback) and CopyFileRangeFd run under AsCaller(req.Caller(), ...);
unit tests in dir_cache_fs_test.cc verify setgid dropped (02666 -> 0666) for
outside-group callers while root and group members retain it; guest
credentials.sh tests all write-family operations against native filesystem and
cold cache; generic/683-685 removed from expected failure lists.

## 17.2b Setgid through passthrough, and a non-owner's EPERM (from the 2026-10-16 review of 15.11; merged 2026-10-17, 1ed1280)

The review of Gemini's week found two suspected setgid bugs that 17.2's
tests do not reach. Reproduce each first (test failing on today's code,
quoted), then fix.

1. **Passthrough writes keep S_ISGID.** A caller outside the file's group
   writes a 06666 (or 02666 group-executable) file opened with FUSE
   passthrough: the write never reaches dcfs, and the kernel's FUSE path
   applies the pre-6.2 rule (`fuse_setattr` drops ATTR_KILL_SGID;
   notes/kernel-patches.md row 4), and passthrough I/O runs with the
   backing file's saved credentials (root, CAP_FSETID), so the backing's
   `file_remove_privs` keeps the bit too. The workaround-first rule
   applies: dcfs declines passthrough for a writable open of a file with
   S_ISGID (or S_ISUID) set, so such writes come through dcfs's own write
   path, which 17.2 already runs inside `AsCaller`; the kernel patch later
   retires the workaround (recorded in kernel-patches.md row 4). Measure
   the cost (a guest bench line for writes to a setgid file) and say it in
   design.md.
2. **A non-owner's write to a world-writable setgid file fails EPERM**
   through dcfs where the native filesystem allows it and clears the bit.
   Find the cause (likely a mode change dcfs issues on the caller's behalf
   that only the owner may make) and fix it so dcfs answers what the
   backing answers.

Tests: harness tests for both with forged credentials (outside group,
group member and root controls), and guest `credentials.sh` checks against
the same operations on a native directory, warm and after a cold cache;
xfstests lists updated only if a listed test changes outcome. No protocol
change is expected (`BeginAttrChange` already marks the record unknown);
if one appears, the model changes in the same step. Protocol agent.

## 17.2c Passthrough writes cost a round trip each (lead, from the 2026-10-17 review of 17.2b; done 2026-10-17, 4fb95db: V2 recommended, waits on russ)

17.2b's `dcfs_bench` `Write` case measured 9.1 ms per MiB through
passthrough against 0.14 ms on the backing (ext4 guest, 1 MiB in 64 KiB
writes, mean of 5). The reviewer's guess, unconfirmed: without
`HANDLE_KILLPRIV_V2` FUSE clears `SB_NOSEC` (fs/fuse/inode.c:1622), so
`backing_file_write_iter` calls `file_remove_privs` on every write
(fs/backing-file.c:266), costing a `GETXATTR security.capability` round
trip each. First count requests by type during `BM_Write`; if WRITE
requests appear, passthrough is not engaged at all. Then decide with
russ whether negotiating `HANDLE_KILLPRIV_V2` (and taking over privilege
removal) is worth it. dcfs-investigator.

## 17.2d Supplementary groups for credential-overriding callers (lead, from the same review)

`FuseRequest::Caller` (`dcfs/fuse_request.cc`) reads supplementary
groups from `/proc/<pid>/status`, which shows a task's real (objective)
credentials, while the request header's uid and gid are subjective. For
kernel callers that override credentials (nfsd once dcfs is exported,
ksmbd, overlayfs) the groups can be wrong. Confirm with a test (nfsd in
the guest exporting a dcfs mount), then decide the fix. Before dcfs is
NFS-exported in earnest. The same root cause, the request not carrying
the caller's full credentials, leaves one 17.2b edge: a uid 0 caller
without CAP_FSETID writing a setgid file outside its group keeps setgid
through dcfs where the backing clears it. dcfs-protocol.

## 17.3 Performance finding (done 2026-10-11, 313bff6)

29 metadata-heavy tests run 20-50x slower through dcfs (4-37 s natively,
200 s or more through dcfs), the daemon CPU-bound in the stall reports,
one test in a SQLite fsync until /cache moved to tmpfs (the per-create
fsync that 23.11 removes); host load 14-30 on 4 cores confounds it. First
a remeasure on a quiet host with the daemon's CPU seconds per test
(17.1's results line), then the `xfstests_slow` set's numbers after
23.11; the tmpfs cache understates production on an SSD (WAL fsync cost):
say so in the note. Owner: dcfs-investigator.
Status 2026-10-11: done, merged 313bff6. Full report in `docs/plan/notes/slowness-17.3.md`.
Measured side-by-side with `xfstests_costs.py`: daemon CPU accounts for 80-98%
of extra latency (single-threaded CPU bound at 100% of a core). Waiting/IO accounts
for only 2-20%; extra disk flushes are negligible (-4 to +21). Conclusion: the
slowness is an architectural design bottleneck (single-threaded FUSE event loop,
per-op SQLite transactions, 3-phase write-through protocol), not parameter tuning.
`test/qemu/guest/xfstests.slow` notes updated; `xfstests_dev_native_test_<fstype>`
added in `test/qemu/BUILD.bazel`.

## 17.4 Spike: In-daemon CPU breakdown under serial metadata storms (russ, 2026-10-11)

Step 17.3 proved that in-guest daemon CPU accounts for 80%–98% of the latency
difference between dcfs and native ext4 under metadata storms.
This spike profiles the daemon during a representative serial metadata storm
(e.g., `generic/007`'s 100k creates/unlinks or `generic/471`'s 10k creates)
to determine the exact fine-grained breakdown of daemon CPU consumption:
1. Instrument or sample the daemon using `perf` / Linux sampling / symbol-level
   profiling in the guest under the serial workload.
2. Attribute the ~312 µs/op daemon CPU budget into:
   - FUSE message handling: read/write on `/dev/fuse`, context switches, libfuse framing.
   - SQLite internals: statement preparation, B-tree traversal/balancing, pager, locks, WAL framing.
   - Backing filesystem interaction: `AsCaller` credentials switching (setfsuid/setfsgid/setgroups),
     `statx`, `openat`, `unlinkat`.
   - C++ runtime & bookkeeping: allocations, invariant checking/observers, hash table lookups.
3. Deliver report with flamegraph and symbol attribution table in `notes/metadata-storm-breakdown-17.4.md`.
Scheduled soon; owner: dcfs-investigator.

## 17.1b A glibc rootfs for xfstests (from the review; after 17.1, not instead)

Base the xfstests root on third_party/debian's rules_distroless snapshot
rootfs (the NFS test's chroot machinery: pinned snapshot.debian.org,
debs.lock, the ownership and rootfs-invariant gates), trixie so xfs_io is
new enough, not on 15.6's systemd image (xfstests needs no systemd). Gate,
lists, wrappers and guest scripts carry over; the image builder and the
package set change. Removes the strerror shim, the echo hunk, the musl
getopt artefacts (680, 749), the multi-call coreutils problem (452),
probably 478, and the image's ownership/mtime reproducibility problem;
adds libaio, liburing and libhandle (about 26 aio-dio and io_uring tests
that matter for O_DIRECT and AIO through FUSE), dbench and python3.
Owner: dcfs-implementer.

## 17.1c A sanitizer subset (from the review)

A curated set of plain-passing generic tests that finish natively in about
30 s, run under asan and ubsan in the weekly job: `asan_mem` measured with
reclaim_scans = 0, a scaled per-test limit, the helpers with
`fully_static_link` plus `-fno-sanitize=all` in their linkopts, the shim
(while it exists) with `-fno-sanitize=all`. Owner: dcfs-implementer, after
17.1b if it lands first (simpler without the shim).

Also from the review, later: the rootfs image reproducible across machines
(a sorted tar with uid 0 and a fixed mtime into `mke2fs -d`, or the
Debian builder's ownership gate); a one-hunk patch so `_scratch_mkfs` for
FSTYP=fuse runs a per-test hook (fresh mkfs and database), removing the
batch-sharing dependence; one shard per backing with `checked_dcfs`; a
tree digest of TEST_DIR through dcfs versus native at the end of each
shard.
