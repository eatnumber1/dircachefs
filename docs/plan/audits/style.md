# dcfs style/readability/dead-code audit (main @ 8d545ef)

Scope reviewed: every file listed in the task (dcfs/*.h, dcfs/*.cc incl.
tests, tools/*.c, test/qemu/*.bzl, test/qemu/BUILD.bazel,
test/qemu/scripts/*.sh, test/qemu/guest/*.sh, dcfs/BUILD.bazel). Read-only;
nothing in the repo was modified.

Overall impression: the code is unusually well-commented for something
written across many agent steps, and the stated invariants (Context& first,
dcfs::ErrnoToStatus, RET_CHECK, no exceptions) are followed almost
everywhere. The real problems cluster into a handful of categories: (1) the
codebase never had clang-format run on it, so it visibly diverges from
.clang-format; (2) the "only backing.cc calls syscalls::" rule has real,
grep-able exceptions; (3) a handful of genuinely dead functions/files; (4)
heavy, exact copy-paste across the QEMU guest test scripts.

---

## 1. Convention violations

### 1.1 `syscalls::` called outside `dcfs/backing.cc` (judgment, but real)
backing.h:24-26 states plainly: "The backing layer: the only code that talks
to the backing filesystems (dcfs::syscalls::) ... ". That is violated in:

- `dcfs/file_handle.cc:48,139,143,157,183` -- `FileHandle::FromDirEntry`,
  `NameToHandle`, `FileHandle::Open` all call `syscalls::name_to_handle_at`,
  `syscalls::statx`, `syscalls::openat`, `syscalls::open_by_handle_at`
  directly. This is the biggest exception: file_handle.{h,cc} is a full
  second module that does its own I/O and its own `ReconcileAttrs`-adjacent
  logic (mount-boundary detection via statx) independent of backing.cc.
  Needs judgment: either (a) fold file_handle's I/O into backing.cc and make
  FileHandle a pure data type, or (b) amend the file comment in backing.h to
  say the invariant is "backing.cc and file_handle.cc", since the latter is
  effectively a peer low-level I/O module, not application logic.
- `dcfs/fd.cc:15,37` -- `FileDescriptor::~FileDescriptor()` and `::Close()`
  call `syscalls::close`. This is arguably a structural necessity (fd.h is
  included by syscalls.h itself; FileDescriptor is the RAII primitive the
  syscall layer is built on), so it reads as an intentional exception, but
  it is not documented as one anywhere. Judgment: add a one-line note in
  backing.h's file comment carving out fd.cc/syscalls.cc as "the syscall
  layer itself," or the rule looks broken to the next contributor.
- `dcfs/main.cc:123-124` -- `syscalls::openat(AT_FDCWD, source, ...)` opens
  `--source` before any `Context` exists. This is unavoidable at that point
  (Context needs the MountFds/db, and backing:: functions all take
  `Context&`), but it is still I/O outside backing.cc. Judgment: consider a
  tiny `backing::OpenSourceRoot(std::string_view path)` wrapper in
  backing.cc that main.cc calls, purely to keep the invariant textually
  true and grep-able.
- `dcfs/device_id.cc:126,140,144,148,150` -- `GetFsType`/`IoctlAllowingOPath`
  call raw `fstatfs`, `ioctl`, `open`, `close` directly (not even through
  the `syscalls::` namespace). This bypasses both the namespace and the
  "backing.cc only" rule. It's a defensible low-level primitive (peer to
  syscalls.cc, used by file_handle.cc and backing.cc alike), but as written
  it's a third place, besides syscalls.cc and fd.cc, that touches raw fds
  outside backing.cc. Judgment: either move `GetDeviceId`'s ioctl calls
  into dcfs/syscalls.cc as `syscalls::GetDeviceId`, or document device_id.cc
  as another accepted exception.

Mechanical part: exhaustively grep-able (`grep -rn 'syscalls::' dcfs | grep
-v backing`), so verifying any fix is trivial even though the fix itself
needs judgment.

### 1.2 `absl::ErrnoToStatus` -- clean
Only call site is `dcfs/status.cc:13`, which is `dcfs::ErrnoToStatus`'s own
implementation (the one sanctioned place). No violations. Mechanical check,
passes.

### 1.3 Raw `new`/`delete` -- one real hit
- `dcfs/errno.cc:25` -- `const static auto *kNamesToErrors = new
  absl::flat_hash_map<std::string, int>{...};` is a leaky-singleton
  Meyers-style pattern. It's a common idiom, but the project already
  depends on a recent Abseil (`abseil-cpp` 20260817.0 per MODULE.bazel:3),
  which has `absl/base/no_destructor.h`. Mechanical: replace with
  `static const absl::NoDestructor<absl::flat_hash_map<std::string, int>>
  kNamesToErrors(...)` -- same effect, no raw `new`, and fixes the
  `const static` (should be `static const`) ordering nit on the same line.
- `dcfs/memory.h:8` -- `RawMemoryDeleter::operator()` calls `::operator
  delete(ptr)`, pairing with a comment "Deletes memory allocated using
  ::operator new (size)". This is a deleter *type*, not a bare
  new/delete call site, and it's the standard way to hand raw
  `::operator new` memory to a `unique_ptr`. Not a violation, but see
  Dead Code below -- `memory.h`/`RawMemoryDeleter` has no users at all.

### 1.4 `LOG(INFO)`/`LOG(WARNING)`/`LOG(ERROR)` vs. "per-request logging at
VLOG(1) at most"
Grepped every `LOG(` call. All are either startup/lifecycle events (fine)
or anomaly/error paths (fine, not "per-request" in the sense the rule
means -- they don't fire on every successful request):
- `dir_cache_fs.cc:89` (Init, once per mount) and `backing.cc:879`
  (StartupPurge, once at startup) -- `LOG(INFO)`, one-time, fine.
- `backing.cc:422` (ReconcileAttrs out-of-band-change warning) -- fine,
  main.cc:86-88 explicitly documents that WARNING+ must reach stderr so
  these are visible; this is intentionally-designed operator-facing
  logging, not routine per-request noise.
- `dir_cache_fs.cc:789,793,872,879,891` (Release/RecordWrittenAttrs
  failure paths), `fd.cc:17`, `fuse_request.cc:68,191`, `main.cc:225,229`
  -- all failure/anomaly paths, not hit on the happy path. No change
  needed, but worth a note: if a backing filesystem is flaky, several of
  these (e.g. dir_cache_fs.cc:872) could become hot; nothing to fix now.
No VLOG(1)-violating call was found; VLOG(1)/VLOG(2) usage (backing.cc:226,
364, 654; sqlite.cc:204) is already the right level.

### 1.5 `Context&` first parameter -- clean
Every `cache::`/`backing::` free function in metadata_cache.h and backing.h
takes `Context &ctx` as its first parameter; grepped for any
non-conforming declaration and found none.

### 1.6 Pointer alignment vs. `.clang-format` (mechanical, codebase-wide)
`.clang-format` declares `PointerAlignment: Left` (i.e. `Type* name`), but
the code is written almost entirely `Type *name` (Right/Hungarian style),
e.g. `dcfs/memory.h:8` (`void *ptr`), `dcfs/dir_cache_fs.h:63,65,66`,
`dcfs/fuse_request.h:52,57,160,167`, `dcfs/mount_fds.h:37`, etc. -- 132
matches for the `Type *name` pattern across dcfs/*.{h,cc} vs. essentially 0
genuine `Type* name` declarations (the "Left" hits found are all comment
text like "no new *child*", not code). Since clang-format was never run
(per the task's own framing), this is a pure config/code mismatch, not
scattered author error -- it's uniform. Mechanical, but there's a real
choice: (a) run clang-format once the tool is available and reformat
everything to match `Left`, or (b) fix `.clang-format` to say
`PointerAlignment: Right` to match the ~100% existing convention and avoid
a huge one-off diff. (b) is far cheaper and is what I'd recommend.

---

## 2. Readability

### 2.1 Long functions (over ~60 lines)
- `dcfs/dir_cache_fs.cc:429-541` -- `DirCacheFS::Rename` (113 lines). Well
  organized around explicit phase-1/2/3 comments, but big enough that a
  reader has to hold a lot of state. Suggested split: extract the
  post-rename refresh block (lines 519-540: refreshing both parents' and
  both inodes' attrs, and the replaced-object row-lifetime decision) into
  a small `RefreshAfterRename(...)` helper. Judgment call -- the phases
  are tightly coupled through local state (`src`, `dst`, `same_inode`,
  `exchange`), so a naive split could just move the problem.
- `dcfs/dir_cache_fs.cc:821-896` -- `DirCacheFS::Release` (76 lines).
  Same shape: could extract the "should this row be deleted" logic
  (863-874) into a helper, e.g. `bool ShouldDeleteRow(...)`. Judgment call,
  low priority -- it's linear and well commented as-is.
- `dcfs/dir_cache_fs.cc:669-736` -- `DirCacheFS::Open` (68 lines),
  mostly from the writable-open error-rollback branch (711-720). Could
  extract `UndoWritableOpenRegistration(...)`. Judgment call.
- `dcfs/backing.cc:372-440` -- `ReconcileAttrs` (69 lines). Long because it
  enumerates 7 attribute fields via a local `check` lambda; the length is
  proportional to the number of fields, not complexity. Low priority.
- `dcfs/backing.cc:635-708` -- `PopulateDirectory` (74 lines). Phase B's
  per-child loop body (669-704, ~36 lines) reads like it wants to be its
  own function, e.g. `UpsertPopulatedChild(ctx, dir, child)`. This one is
  a fairly clean, low-risk extraction (the loop body is already
  self-contained; it just needs `dir`, `child`, and `ctx`). Judgment call,
  but the easiest of this group to do.
- `dcfs/metadata_cache.cc:443-523` -- `UpsertInode` (81 lines), naturally
  divides into "find existing/stale rows", "update existing", "insert new".
  Could become 3 small helpers called from one `ctx.db.Transaction`.
  Judgment call.
- `dcfs/main.cc:81-241` -- `Main()` (161 lines). Long but is literally
  "parse flags, open source, migrate, mount, run, shut down" in a strict
  sequence for a `main`-adjacent function; this is normal for this kind of
  entry point and I would not split it further than it already is.
- `dcfs/errno.cc:24-271` -- `NameToErrnoTable()` (248 lines) is almost
  entirely a single initializer list (one line per `#ifdef`-guarded errno).
  Not a real "long function" in the readability sense; no action needed.

None of these are alarming; this project's functions are already fairly
disciplined about phase comments. I would only actually split
`PopulateDirectory`'s per-child block and maybe `UpsertInode`; the rest are
optional polish.

### 2.2 Stale/misleading comments
- `tools/testutil.c:5-9` -- "dcfs currently refuses at open() (Setattr's own
  ftruncate is a separate step-4.4-independent path, but Open() still
  rejects any non-read-only open until step 4.4) -- so it never reaches
  Setattr(FUSE_SET_ATTR_SIZE) at all." This is **stale**: step 4.4
  (write-through file I/O) is already implemented and committed (see git
  log: `6a473b1 4.4: write-through file I/O...`), so Open() no longer
  universally rejects non-read-only opens. The surrounding rationale for
  *why* `testutil truncate` exists (to test truncate(2) without an
  intervening open()) is still valid and worth keeping, but the "currently
  refuses"/"until step 4.4" phrasing needs to be rewritten in the past
  tense or removed. Mechanical rewrite once someone confirms current
  Setattr/Open behavior.
- `test/qemu/guest/setattr.sh:27,220` and `test/qemu/README.md:324` --
  same "until step 4.4" framing, but in these files it's explicitly
  contextualized as "this is what used to prevent test X from reaching
  Setattr, before step 4.4 landed" -- i.e. it's already past-tense/historical
  narration explaining *why* a test exists in its current form, not a claim
  about current behavior. These read fine as-is; only testutil.c's comment
  reads as a present-tense claim. Low priority to fix them for consistency
  of tense, but not misleading today.
- `dcfs/errno.cc:290` -- `// TODO statusbuilder`. Terse, no context on what
  needs to change (probably: use `absl::StatusBuilder` for the NotFound
  return two lines above, consistent with main.cc's use of StatusBuilder).
  Mechanical to either resolve or delete/expand.
- `dcfs/fuse_request.cc:67` -- `// TODO imporve this warning` (typo:
  "imporve"). Also vague. Mechanical: fix the typo; judgment: decide what
  "improve" means here (probably: include which op/ino, not just
  "Replying to FuseRequest in destructor").

I did not find any stale references to "EROFS for writes", "O_PATH mount
fds", "host-side tests", or "device_id_fn" describing *current* behavior
incorrectly -- the EROFS/O_PATH/host-side-test mentions I found are all
either still accurate (EROFS is still a real fallback-path errno check at
dir_cache_fs.cc:651) or explicitly historical ("before step 4.4..."/"no
host-side test execution" is still true today, per qemu_cc_test.bzl:4-9).
`device_id_fn` does not appear anywhere in the tree.

### 2.3 Dead/narrating-history comments
- The "step N.N" / "Phase N" comments throughout backing.h, metadata_cache.h,
  dir_cache_fs.{h,cc} and the guest test scripts are almost all still
  functioning as *design* references (phase 1/2/3 of the write-through
  rule is an active protocol, not history), so I would not mass-delete
  them. The one exception is `dcfs/status.cc:34` ("Moved here from
  dcfs/fuse.cc (step 1.1)") -- pure history, `dcfs/fuse.cc` doesn't exist
  any more, and the comment adds nothing for a new reader beyond "this
  code lives here now." Mechanical: delete the "(step 1.1)" /
  "Moved here from..." clause, keep the substantive part ("so that all
  status<->errno logic lives in one place").

### 2.4 Naming
- No inconsistent naming was found between parallel functions
  (Setattr/Getattr, Mkdir/Mknod/Symlink all follow the same
  parent_fd/name/mode shape; Reply* methods are uniformly named).
- `dcfs/fuse_request.h:158-168` / `.cc:250-251` -- `LogFuseFileInfo` is a
  fine name but see Dead Code (3.1): it's never actually used for logging
  anywhere.

### 2.5 Duplicated logic that should be a helper
- Within `dcfs/backing.cc`, the "reopen a regular file/directory via
  /proc, else use the `_opath` syscall for symlinks/specials" pattern
  appears independently in `ApplyXattrOp` (589-607), `ApplyMode` (902-918),
  `ApplyTimes` (935-947) -- each re-derives `S_ISREG(type) ||
  S_ISDIR(type)` and picks between a `ReopenPathFd`-based path and an
  `_opath` syscall. A small helper like `bool
  CanReopenForRealFd(mode_t type)` would remove three duplicated
  conditionals. Judgment call, low risk, mechanical once the helper exists.
- See Test Readability (5) for the much larger duplication in the guest
  scripts.

---

## 3. Dead code

### 3.1 `LogFuseFileInfo` -- fully dead (mechanical, safe to delete)
`dcfs/fuse_request.h:158-168,172-228` (struct + ~55-line `AbslStringify`
template) and `dcfs/fuse_request.cc:250-251` (both constructors). Grepped
the entire tree for `LogFuseFileInfo(` outside its own definition: zero
hits. Nothing ever constructs one. Safe to delete the whole type; if
logging `fuse_file_info` is wanted later it can be re-added when there's
an actual call site.

### 3.2 `dcfs/strong_number.h` -- entire file is dead (mechanical, safe to
delete)
Not `#include`d anywhere, not referenced in `dcfs/BUILD.bazel` (no
`cc_library` target for it at all -- it isn't even reachable from the
build graph), and `InodeId` (which looks like its obvious intended user)
is just a plain `using InodeId = int64_t;` in metadata_cache.h:49. Also
has its own hygiene problems if it were ever revived: no header guard at
all, zero `#include`s despite using `absl::Format`, `std::move`, and
`operator<=>` (relies entirely on whatever the includer happened to pull
in first), and `using RawType = RawType;` (line 6) is a confusing
self-referential alias of the template parameter. Recommendation: delete
the file outright; it is pure speculative infrastructure that was never
wired up.

### 3.3 Three `cache::` functions with no production caller (needs
judgment, mechanically verified)
Verified via grep that none of these is called anywhere except their own
definition/declaration and `metadata_cache_test.cc`:
- `cache::GetGeneration` (metadata_cache.h:105, .cc:229-241)
- `cache::UnlinkDentry` (metadata_cache.h:184, .cc:600-607)
- `cache::RenameDentry` (metadata_cache.h:189-190, .cc:608-625)

`DirCacheFS::Rename` (dir_cache_fs.cc:502-507) uses `LinkDentry` +
`SetNegative` directly rather than `RenameDentry`, and `RemoveChild`/
`SettleUnlinkedFile` don't call `UnlinkDentry` either (I did not find what
they use instead in the time available -- worth double-checking before
deleting `UnlinkDentry` specifically, since dentry removal happens
somewhere). `GetGeneration` looks like a plausible future API (paired with
`GetAttr`) that just never got a caller. Recommendation: either delete
these three plus their tests (project preference is "prefer deleting dead
code"), or -- if they're intentionally-exposed low-level primitives for a
near-future caller -- add a one-line comment saying so, so the next auditor
doesn't re-flag them.

### 3.4 `dcfs/smoke_test.cc` -- trivial, near-content-free test
`TEST(Smoke, Passes) { EXPECT_TRUE(true); }` (3 lines total). It doesn't
exercise any dcfs code; it verifies only that the `qemu_cc_test` toolchain
itself can build/boot/run a test binary. That's a legitimate purpose, but
nothing in the file says so. Mechanical: either add a one-line comment
explaining it's an infra/toolchain smoke check (not a dcfs behavior test),
or delete it if `boot_test`/`lifecycle_test` already cover "the test
infra works" well enough.

---

## 4. Header hygiene

- `dcfs/strong_number.h` -- no header guard at all (see 3.2). If not
  deleted, needs `#ifndef DCFS_STRONG_NUMBER_H_` / `#define` / `#endif`
  and a full set of includes (`<compare>`, `<utility>`,
  `"absl/strings/str_format.h"`, `"absl/hash/hash.h"`).
- `dcfs/metadata_cache.h:5` -- `#include <time.h>` (C header) instead of
  `<ctime>`; `dcfs/status.cc:4` and `dcfs/status_test.cc:3` -- `#include
  <string.h>` instead of `<cstring>`. Minor, mechanical, Google style
  prefers the `<cxxx>` form in C++. Low priority, but easy to batch-fix
  with the same commit as other include cleanups.
- `dcfs/dir_cache_fs.h:9` -- `#include <sys/types.h>` is listed after the
  C++ standard headers (`<cstdint>`...`<string_view>`) instead of grouped
  with C system headers first. Same pattern recurs in a few other files
  (e.g. `fuse_request.h:10-12` interleaves `<sys/stat.h>`/`<sys/statvfs.h>`
  between `<string>` and `<string_view>`). Purely a grouping/ordering nit
  per the Google style's "C system headers, C++ standard headers, blank
  line, other" convention; mechanical, but only worth doing as part of a
  broader clang-format-style pass since it's cosmetic.
- No missing-include (IWYU) problems found that would actually fail a
  strict build: every header I checked either includes what it uses or
  the symbols come from a header it does include. `strong_number.h` is
  the one exception (3.2), and it isn't built at all, so it currently
  can't be caught by a real IWYU/compile check.
- All header guards otherwise follow a single consistent convention:
  `DCFS_<PATH>_H_`. No inconsistent guard names found.

---

## 5. Test readability

### 5.1 Guest script duplication (biggest finding in this category)
Exhaustively grepped `test/qemu/guest/*.sh`. Four helpers are copy-pasted
essentially verbatim across most of the 11 scripts, with no shared file:

- `is_mounted() { grep -q " $1 " /proc/mounts; }` -- byte-identical in
  create.sh:37, handles.sh:49, crash.sh:41, passthrough.sh:37, nfs.sh:61,
  readonly.sh:29, rename.sh:35, setattr.sh:46, write.sh:45 (9 copies).
- `fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }` -- byte-identical in
  all 11 guest scripts (boot.sh:13, handles.sh:42, crash.sh:36,
  lifecycle.sh:21, readonly.sh:24, nfs.sh:57, rename.sh:29, setattr.sh:40,
  create.sh:32, passthrough.sh:32, write.sh:40).
- `sectors_read()` (4-line `/sys/block/$1/stat` reader) -- byte-identical
  in crash.sh:78, nfs.sh:130, rename.sh:70, setattr.sh:87, readonly.sh:71,
  create.sh:75, write.sh:83, passthrough.sh:77 (8 copies).
- `start_daemon()` -- identical (modulo the exact `$DCFS` argument list,
  e.g. nfs.sh adds `--allow_other`) in create.sh:83, passthrough.sh:97,
  rename.sh:76, write.sh:100, readonly.sh:79, crash.sh:89, nfs.sh:143,
  setattr.sh:100, handles.sh:96 (9 near-identical copies).
  `lifecycle.sh:73` has a genuinely different, more generic
  `start_daemon(log, target, args...)` that could be the basis for a
  shared version.
- `handles.sh:119` and `nfs.sh:168` also duplicate a `restart_daemon()`.

Recommendation: add `test/qemu/guest/lib.sh` with `is_mounted`, `fail`,
`sectors_read`, and a parameterized `start_daemon`/`restart_daemon`
(closer to lifecycle.sh's shape, taking the `$DCFS` args as `"$@"`), and
have every guest script `. "$(dirname "$0")/lib.sh"` (all guest scripts
already land together under `/tests/` in the initramfs, per
`mkinitramfs.sh`, and `guest/*.sh` is glob-matched into the initramfs
already, so `lib.sh` just needs to be added to that glob's inputs --
verified via `test/qemu/BUILD.bazel`'s `initramfs` genrule and
`scripts/mkinitramfs.sh:1-23`). It will sit unused as `/tests/lib.sh`
(harmless -- no `qemu_test` target names it as a `guest_script`). This is
~150+ duplicated lines across 8-11 files; needs judgment only in choosing
the shared `start_daemon` signature, otherwise mechanical.

### 5.2 Tests asserting too much in one case
- `dcfs/metadata_cache_test.cc:656-701` -- `TEST_F(MetadataCacheTest,
  Xattrs)` (46 lines) walks GetXattr/ListXattrs/SetXattr/ReplaceXattrs/
  RemoveXattr/MarkXattrsUnknown/the-missing-inode-case all in one test
  with a vague name that doesn't say which behavior is under test. A
  failure partway through only tells you "something about xattrs broke."
  Recommend splitting into e.g. `XattrGetSetIndividualValue`,
  `XattrReplaceMakesSetComplete`, `XattrRemoveKeepsSetComplete`,
  `XattrMarkUnknownClearsCache`, `XattrOpsOnMissingInode`. Judgment call
  (need to decide the right grouping), moderate effort.
- `dcfs/backing_test.cc:217-283` -- `PopulatedDirectoryIsServedFromTheCache`
  (66 lines) and a few `metadata_cache_test.cc` tests around 45-50 lines
  (`DeleteInodeRemovesEverything`, `PurgeFilesystem`,
  `GetAttrRoundTripsEveryField`) are long but each has a name that
  accurately scopes what it's testing, and they're exercising one
  cohesive scenario (not several unrelated ones) -- lower priority than
  `Xattrs` above; I would leave these as-is unless they start showing up
  as noisy failures in review.

### 5.3 Other
Test names elsewhere are already fairly descriptive
(`RecycledInodeInvalidatesRow`, `HandleSurvivesRestart`-style names in the
guest scripts' PASS/FAIL labels, `MetadataCacheTest.*` names in the unit
tests). No misleading test names found.

---

## 6. BUILD hygiene

- `dcfs/BUILD.bazel`: the `context` target's deps are not grouped/sorted
  the way every other target in this file is (`:targets` first, then
  `@external` deps, each group alphabetical):
  ```
  deps = [
      ":device_id",
      "@absl//absl/container:flat_hash_set",
      ":mount_fds",
      "@absl//absl/status:statusor",
      ":sqlite",
  ],
  ```
  (dcfs/BUILD.bazel, `context` target) interleaves them instead of
  grouping. Compare to `backing`, `metadata_cache`, `file_handle`, etc.,
  which are all `:targets...` then `@absl...`, alphabetized within each
  group. Mechanical, one-line fix (reorder to `:device_id, :mount_fds,
  :sqlite, @absl//absl/container:flat_hash_set,
  @absl//absl/status:statusor`).
- `MAIN_DEPS` (dcfs/BUILD.bazel:6-31): the abseil deps are *almost*
  alphabetical but `@absl//absl/base:log_severity` is out of order --
  listed after `@absl//absl/flags:usage` instead of before
  `@absl//absl/cleanup`. Mechanical, one-line reorder.
- No missing `size` attributes: every `qemu_cc_test` call explicitly
  passes `size = "small"` (redundant with the macro's own default in
  `qemu_cc_test.bzl:56`, but harmless/explicit) and every `qemu_test` gets
  `size = "large"` baked in by the macro itself
  (`test/qemu/qemu_test.bzl:59`). Not a real gap.
- No stale targets found: every `cc_library`/`cc_test`-equivalent target
  I checked has at least one dependent or is a top-level binary/test.
- `buildifier` is not installed in this environment, so the file has never
  been auto-formatted; the two hygiene items above are exactly the kind of
  thing `buildifier` would fix automatically once it's available.

---

## Prioritized top 15

1. **Delete `dcfs/strong_number.h`** (3.2) -- entirely dead, not in the
   build graph, has its own bugs (no guard, no includes). Mechanical.
2. **Delete `LogFuseFileInfo`** (3.1) -- dead struct + ~60 lines across
   fuse_request.h/.cc. Mechanical.
3. **Fix the stale "until step 4.4" claim in `tools/testutil.c:5-9`** --
   actively misleading about current Open()/Setattr behavior. Mechanical
   once someone confirms current behavior in one sentence.
4. **Extract a shared `test/qemu/guest/lib.sh`** for
   `is_mounted`/`fail`/`sectors_read`/`start_daemon`/`restart_daemon`
   (5.1) -- highest-value duplication removal in the whole review
   (~150+ duplicated lines across 8-11 files). Judgment on the shared
   `start_daemon` signature, otherwise mechanical.
5. **Resolve the `syscalls::` layering exceptions** (1.1) -- at minimum,
   document in backing.h's file comment which modules besides backing.cc
   are allowed to call `syscalls::` and why (fd.cc, arguably device_id.cc,
   arguably file_handle.cc), since right now the file comment states a
   rule the code doesn't follow. Judgment call, but low-risk (comment-only
   fix); a deeper fix (moving file_handle.cc's I/O into backing.cc) is a
   bigger, separate decision.
6. **Reconcile `.clang-format`'s `PointerAlignment: Left` with the
   codebase's actual `Type *name` style** (1.6) -- either flip the config
   to `Right` (cheap) or run clang-format once available (expensive,
   large diff). Mechanical either way, but needs a decision first.
7. **Replace `errno.cc:25`'s leaky `new` singleton with
   `absl::NoDestructor`** (1.3) and fix the `const static` ordering on the
   same line. Mechanical, self-contained.
8. **Delete/verify the 3 unused `cache::` functions** (`GetGeneration`,
   `UnlinkDentry`, `RenameDentry`, 3.3) -- verify `UnlinkDentry` really has
   no caller (double-check RemoveChild's actual call chain) before
   deleting; delete or justify the other two. Judgment call.
9. **Split `MetadataCacheTest.Xattrs`** (5.2) into named, focused tests.
   Judgment call on grouping, moderate effort.
10. **Extract `PopulateDirectory`'s per-child loop body** (2.1) into a
    named helper -- cleanest of the "long function" findings, low risk.
11. **Fix the two BUILD.bazel dep-ordering issues** (`context` target,
    `MAIN_DEPS`'s `base:log_severity`) -- trivial, mechanical, one commit
    covering the whole file once buildifier is available.
12. **Fix the two TODOs** (`errno.cc:290 "TODO statusbuilder"`,
    `fuse_request.cc:67 "TODO imporve this warning"`) -- small, mechanical
    once someone decides what "improve" means for the destructor warning.
13. **Delete the "Moved here from dcfs/fuse.cc (step 1.1)" clause** in
    `status.cc:34-35` -- pure dead history, one-line edit. Mechanical.
14. **Extract the reopen-vs-`_opath` dispatch helper** shared by
    `ApplyXattrOp`/`ApplyMode`/`ApplyTimes` in backing.cc (2.5). Judgment
    call, low risk.
15. **Batch include-hygiene pass**: `<time.h>`/`<string.h>` -> `<ctime>`/
    `<cstring>`, and group C system headers before C++ standard headers
    consistently (dir_cache_fs.h, fuse_request.h, others). Mechanical,
    cosmetic, best done together with #6.

## Commit-count estimate

Given the "small, reviewable" constraint, I'd split this into roughly
**9-11 commits**:

1. Delete `strong_number.h` + BUILD (if it had a target, it doesn't).
2. Delete `LogFuseFileInfo` from fuse_request.{h,cc}.
3. Fix the stale testutil.c comment + the two TODOs + the status.cc
   history clause (all small, unrelated-but-tiny doc/comment fixes --
   fine to batch into one "comment cleanup" commit).
4. `errno.cc`: `NoDestructor` + `const static` fix.
5. BUILD.bazel: fix `context`'s dep grouping and `MAIN_DEPS`'s ordering
   (and run buildifier if it becomes available, in the same commit).
6. Add `test/qemu/guest/lib.sh` and migrate all 11 guest scripts to use
   it (this one is bigger; could be split into "add lib.sh + migrate the
   3 scripts with identical start_daemon" then "migrate the rest," i.e.
   2 commits if reviewers want smaller diffs).
7. Investigate and resolve the 3 unused `cache::` functions (delete or
   justify).
8. Split `MetadataCacheTest.Xattrs` into focused tests.
9. Extract `PopulateDirectory`'s per-child helper.
10. Document/resolve the `syscalls::`-outside-backing.cc layering
    (comment-only fix is 1 commit; an actual code move of
    file_handle.cc's I/O would be its own, larger, separate effort not
    counted here).
11. (Optional, larger, do separately) Decide on and apply the
    clang-format pointer-alignment reconciliation + include-grouping
    pass across the whole tree -- this one is inherently not "small" once
    started, so it's better scoped as its own dedicated cleanup pass
    rather than folded into the list above.
