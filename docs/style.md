# dcfs style guide

This guide **extends Google's style guides**, and our code and docs must
conform to them: [C++](https://google.github.io/styleguide/cppguide.html),
[Python](https://google.github.io/styleguide/pyguide.html) (80 columns),
[Shell](https://google.github.io/styleguide/shellguide.html) and, for
documents,
[Markdown](https://google.github.io/styleguide/docguide/style.html).
Bazel files follow Bazel's BUILD style guide as buildifier enforces it. A
section here says only what adds to or narrows Google's rules, or covers
what Google does not (tests, Bazel, docs, commits). Where the tree breaks
Google style or a rule below, Appendix A counts the sites and gives a
command that finds them, for a later mechanical step. Nothing here is open:
russ settled the questions on 2026-10-07.

Derived from the tree at e680508 (2026-10-07) with step 25.1 applied (rows
it touched say so). Counts are of production
code (`dcfs/`, `bench/`, no tests) unless stated. File:line references
drift. Each rule is what most of the code does, or is in `AGENTS.md`,
`docs/design.md` or `docs/plan/process.md`, or is russ's decision.

## Precedence, and the Abseil Tips of the Week

When rules disagree, the first applies: (1) `AGENTS.md` and this guide's own
rules; (2) the Google C++ style guide (and Google's other style guides, as
above); (3) Abseil's Tips of the Week (https://abseil.io/tips/) as design
guidance. russ, 2026-10-09: "we adopt all of https://abseil.io/tips/. It's
a 'rule', but not as strong of a rule as the Google style guide or other
style rules I've established in the past. Coding and review agents should
use it as design guidance rather than firm rules."

- Coding and review agents use a tip when a design question matches one (the
  index page lists the titles; read the tip online). Nothing here vendors the
  tips.
- A review finding that rests on a tip cites its number (`TotW #NNN`) and is
  **advisory** unless it also breaks a rule above; the author may decline it
  with a reason.

## 1. C++

### 1.1 Enforced mechanically

- **Formatting is enforced, not requested, and it is Google's, with one
  kept deviation.** `.clang-format` is `BasedOnStyle: Google` plus
  `PointerAlignment: Right` (with `DerivePointerAlignment: false`, which it
  needs) and nothing else (russ, 2026-10-11: "When we do a codebase-wide
  reformat, I want Google's format rules to apply", then "keep the
  PointerAlignment: Right"); so pointers stay `int *p`. The pinned LLVM's
  clang-format runs behind `bazel run //tools:format` and a `small`
  `//tools:format_test` checks every tracked C/C++ file in `--dry-run
  -Werror` mode; buildifier for Bazel files and shfmt/shellcheck for shell
  (Google shell style) follow in 7.6b with their pins. The one-time
  reformat is one commit, listed in `.git-blame-ignore-revs`, made when no
  lane holds an unmerged code branch, since it touches most lines.
- **Structural style rules are checked by the pinned LLVM's own tools, in a
  test of tier `small`** (25.21): `//tools:style_checks_test` runs
  clang-tidy (`.clang-tidy`: the `google-`, `abseil-`, `bugprone-`, `cert-`,
  `concurrency-`, `misc-`, `modernize-`, `performance-`, `portability-` and
  `readability-` groups minus a deny-list in which every entry has a reason)
  and clang-query (one file per rule in `tools/style_matchers/`, each
  beginning with the style section it enforces, with a known-bad and a
  known-good fixture that `//tools:style_matchers_test` checks) over `dcfs/`,
  `bench/` and `tools/` with the build's own flags (`tools/style_checks.bzl`:
  Bazel actions, cached per file). So `bazel test --config=fast //tools/...`
  finds a violation in a diff with no one reading it. The findings that
  existed when a check was switched on are in `tools/style_checks_allow.txt`,
  keyed by file, function and check (never a line): a new finding fails, a
  fixed one must be taken off the list, and the list only shrinks.
  `status_uninitialized` and `pointer_without_nullability` are report-only
  (printed in the test log). Fix a finding in the code you touch; do not add
  a line for new code.
- **Include what you use, enforced**: a target includes only headers of its
  direct deps (`layering_check`) and clang-tidy's `misc-include-cleaner`
  finds missing and unused includes, both with phase 7's clang toolchain
  (F6). Until then, state each direct dep in BUILD and add no include that
  nothing uses.
- **No copies of UAPI definitions.** The pinned sysroot's kernel headers
  (`<linux/fs.h>` and the rest of trixie's linux-libc-dev, step 7.1b) are
  the only source of a kernel constant, struct or ioctl number: no
  `#ifndef FS_IOC_...` shim, no local `struct` mirroring a UAPI one. A
  definition that is missing means the pin is too old: raise the pin, do
  not paste the definition (step 7.1d removed `dcfs/fsuuid_compat.h` and the
  `FS_CASEFOLD_FL` shim of `casefold_helper.c`). One shim remains: the
  `FS_IOC_SHUTDOWN` definition in `tools/testutil.c`, because the pinned
  sysroot's `<linux/fs.h>` (6.12) predates it; it goes when the pin is
  raised.
- Include blocks are what clang-format's Google style produces: own header,
  C headers, C++ headers, then every other header in one sorted block
  (`"absl/..."`, `"dcfs/..."`, `"fuse_lowlevel.h"`).
- Warnings: `-Werror`, `-Wimplicit-fallthrough`, `-Wno-sign-compare`
  today (`.bazelrc`); from phase 7 `-Weverything -Werror` with every
  disabled warning listed in `.bazelrc` with a reason. A suppression
  carries a comment saying why.

### 1.2 Narrowing Google's rules

- **`std::string_view`, `std::optional`**, not the `absl::` aliases (none
  left in production code).
- **Function names**: `syscalls::` wrappers carry their libc/manpage names
  in lower case (`syscalls::setxattr`); every other function is CamelCase
  (`SetXattr`). The two layers therefore never clash by construction.
- **Every pointer says whether it may be null; a pointer that may not be
  null is usually a reference** (russ, 2026-10-10: "all pointers need
  either `absl_nullable` or `absl_nonnull`, including both raw pointers
  and smart pointers (e.g. `unique_ptr`). If you have a non-nullable
  pointer, prefer a reference instead."). `absl/base/nullability.h`
  (Abseil 20260817, pinned): the qualifier follows the pointer type,
  `Backing* absl_nonnull b`, `const char* absl_nullable name`,
  `std::unique_ptr<Connection> absl_nonnull db`,
  `absl::StatusOr<Foo* absl_nonnull>`. Order of preference for a
  parameter or member that is never null: a reference (`Backing &b`,
  `const Credentials &caller`); a pointer only where a reference cannot
  go (a member that is reseated, an optional-out parameter that is
  present, an owning `unique_ptr`), and then `absl_nonnull`. A pointer
  that may be null is `absl_nullable`, and the reader expects a null
  check before every dereference of it. For an optional, non-owning,
  read-only argument, `absl::optional_ref<const T>`
  (`absl/types/optional_ref.h`) is allowed and preferred over
  `const T* absl_nullable` (russ, 2026-10-10: "Allow it"): it cannot be
  dereferenced without a check and says "optional" in the type. (A
  `std::optional<T>` parameter copies; `optional_ref` does not.)
  `absl_nullability_unknown` is
  not used: it is the annotation that says nobody decided. Mechanically:
  clang's `-Wnullability-completeness` and
  `-Wnullable-to-nonnull-conversion` as errors (7.3's `-Weverything`
  set: they stay off the deny-list), which refuse an unannotated pointer
  in any file that annotates one; `pointer_without_nullability.query`
  (25.21) reports a raw pointer or `unique_ptr`/`shared_ptr` parameter,
  field or return type without a qualifier, as a report-only count until
  plan step 25.16 annotates the tree and turns it into a gate (today 0
  annotations; the report counts them).
- **Flags are defined only in a program's main file** (TotW #103 "Flags Are
  Globals"; russ, 2026-10-11, on `IsDcfsFlagFile` claiming every file
  under `dcfs/`: "per https://abseil.io/tips/103 flags should only live in
  main.cc anyway. So this is overly broad."). `ABSL_FLAG` appears in
  `dcfs/main.cc` (and a tool's or bench's own main file), nowhere else;
  code reads configuration through parameters and structs built in
  `main`, never `absl::GetFlag` in a library. The usage-config predicate
  that tells Abseil's `--help` which files hold flags names exactly the
  main file, not a directory. Mechanically: `repo_shape.py` refuses
  `ABSL_FLAG(` and `absl::GetFlag(` outside files named `main.cc` (or the
  binary's own `<name>.cc`), no allowlist (25.20).
- **Banned: `std::function`, `std::unordered_map`/`std::unordered_set`,
  `std::chrono`** (russ, 2026-10-10: "These classes / functions are
  banned. Abseil's versions are always better."). In their place:
  `absl::AnyInvocable` (owning) or `absl::FunctionRef` (a parameter that
  is only called); `absl::flat_hash_map`/`flat_hash_set`
  (`node_hash_*` when pointers into the table must stay valid);
  `absl::Time`/`absl::Duration`/`absl::Now()` for time arithmetic (never
  for waiting: 1.11). Everywhere, tests included. Mechanically:
  `tools/style_matchers/banned_std.query` (25.21) reports every use of the
  three names in `dcfs/`, `bench/` and `tools/`; the sites that existed are
  in the shrinking `tools/style_checks_allow.txt`.
- **Reach for Abseil before writing a helper**: `docs/abseil-utilities.md`
  (25.19) catalogues the pinned Abseil (20260817.0) with a swaps table
  (hand-written pattern → utility, each marked with the rule that demands
  it), a section per directory, the sites in our tree that still hand-roll
  one, and a ten-question reviewer checklist. Coders read the swaps table
  before writing string, container, function-object or status code;
  reviewers run the checklist on every diff.
- **Abseil's container algorithms, not iterator pairs** (russ, 2026-10-10,
  on `std::copy(in.begin(), in.end(), buf.begin())` in `backing.cc`:
  "There are helper functions in absl/algorithm/container.h that let you
  do e.g. `absl::c_copy(in, buf.begin());`. Use those instead of functions
  like std::copy."). `absl::c_copy`, `absl::c_sort`, `absl::c_find`,
  `absl::c_any_of`, `absl::c_count`, `absl::c_equal` and the rest of
  `absl/algorithm/container.h` (`@absl//absl/algorithm:container`) take
  the range; `std::copy(x.begin(), x.end(), ...)` says the same thing
  twice and lets the two ends disagree. A `std::` algorithm is written
  only where no `absl::c_` form exists (a sub-range on purpose, which the
  comment then names, or an algorithm Abseil does not wrap), and
  `std::begin`/`std::end` only to build such a sub-range. Applies to
  tests too. Mechanically: `tools/style_matchers/iterator_pair_algorithm.query`
  (25.21) reports a `std::` call whose first two arguments are `begin()` and
  `end()` (or `std::begin`/`std::end`) of one variable or member, in
  `dcfs/`, `bench/` and `tools/`; the sites that existed are in the
  shrinking `tools/style_checks_allow.txt`.
- **`absl::FixedArray`, not `std::vector`, for a buffer whose size is known
  when it is made** (russ, 2026-10-10). A `std::vector<T> v(n)` that is
  never pushed to or resized says the wrong thing: it advertises growth
  that never happens and pays for it (a heap allocation even for a few
  elements, a capacity field, the push_back surface). `absl::FixedArray<T>
  v(n)` says "n elements, decided once", keeps small arrays inline
  (`absl/container/fixed_array.h`; `@absl//absl/container:fixed_array`),
  and has no `push_back` to misuse. Rule of thumb: the size comes from a
  syscall's answer, a header field or a count argument, and the array is
  filled once and read: FixedArray. The size changes after construction,
  or the container is returned to a caller who expects a vector: vector
  (an `absl::InlinedVector` where the usual size is small and known).
  `std::string(n, '\0')` as a byte buffer handed to a syscall is the same
  smell when the result is not a string: a `FixedArray<char>` or
  `FixedArray<uint8_t>`. The example that set the rule: `GetGroups`
  (`backing.cc`, `std::vector<gid_t> groups(n)` for `getgroups(n, ...)`).
  Mechanically: a repo-shape check refuses `std::vector<T> name(expr);`
  in `dcfs/*.cc` and `dcfs/*.h` outside an allowlist that only shrinks
  (plan step 25.12).
- **ADL hooks** (`AbslStringify`, `AbslHashValue`, `operator<<`, `swap`)
  are hidden friends: defined inside the class they belong to, in that
  class's namespace, never in another namespace or file
  (`FileHandle::AbslHashValue`, `file_handle.h:33`).
- **Test-only code** goes in `*_test.cc` or `dcfs/testonly/` (BUILD
  `default_testonly = 1`), never in a production file; production code has
  no test hooks.
- **A module** is `foo.h` + `foo.cc` + `foo_test.cc` and BUILD targets
  `foo` and `foo_test`; header-only modules have a test only if they have
  logic. Unpaired by design: `main.cc`, `errno.cc` (the errno table behind
  `status.h`), `protocol_events_main.cc`. A module's header comment says
  what it is for and which layering rule it obeys (`dcfs/backing.h:27-52`).
- **ASCII only** in code and prose (one micro sign in `docs/design.md`); a
  comment's dash is ` -- ` (219 uses).
- **No exceptions** (0 `throw`/`catch`); a status dropped on purpose is
  `.IgnoreError()` (15).
- **`[[nodiscard]]` goes on a function whose return type is not already
  must-use, and never on one that returns `Status` or `StatusOr`** (russ,
  2026-10-10, on `[[nodiscard]] absl::StatusOr<FsckArgs> ParseFsckArgs`:
  "StatusOr already is marked [[nodiscard]], so putting it on the return
  type of the method is redundant"). `absl::Status` and `StatusOr` carry
  `ABSL_MUST_USE_RESULT` on the type, so every function returning them is
  checked already; the attribute on such a function says nothing and is
  removed (25.17: 38 sites). It stays, and is expected, where the type
  itself is not must-use and dropping the value is a bug: an owned
  `FileDescriptor` (a leak or a lost error), a report struct the caller
  must act on (`CheckCacheDatabase`). Mechanically: `repo_shape.py`
  refuses `[[nodiscard]]` (and `ABSL_MUST_USE_RESULT`) immediately before
  `absl::Status` or `absl::StatusOr` in our C++ (25.17).
- **Integers**: `int64_t` row ids (`using InodeId = int64_t;`,
  `metadata_cache.h:56`), `uint64_t` node ids, generations and backing
  inode numbers, `size_t` sizes, `off_t` offsets, `int` for descriptors,
  flags and errnos.
- **Ownership**: an owned descriptor is a `FileDescriptor` (`dcfs/fd.h`), a
  borrowed one an `int fd` parameter; scope-exit work is `absl::Cleanup`
  (6 uses). In `dcfs/` only `syscalls.cc` calls `close()` by hand.
- `TODO(topic): ...` names what the work waits for (4 uses, all
  `TODO(coroutines)`).
- **`ReadOne` is for unique lookups only** (`metadata_cache.cc`): a primary
  or unique key, an aggregate, or `LIMIT 1`. It stops after the first row
  (no step to find the end: one statement step less per lookup, and
  `test/qemu/guest/syscall_budgets.txt` counts steps), so a second row would
  go unnoticed. A query that could return several and must check uses
  `ForEachRow` with `LIMIT 2` (`ParentOf`). There is no debug-only second
  step: it would restore the step the budgets count in the fastbuild; the
  checking build (26.2) is the place for one.

### 1.3 Namespaces

All code is in `namespace dcfs`, flat: no nested namespaces, with two
exceptions, `dcfs::syscalls` (its wrappers keep their libc names and so need
the qualifier) and `dcfs::sqlite3` (the SQLite wrapper layer). Call them as
`syscalls::open(...)` and `sqlite3::Connection`, never
`dcfs::syscalls::open` or `dcfs::sqlite3::Connection` and never with a
`using`. The other sub-namespaces (`cache`, `backing`, `testonly`, `events`,
`internal`) go: name things so that a flat `dcfs::` stays unambiguous (N1,
N2, N3, N5, N6). `backing` folds into `dcfs` as free functions (russ,
2026-10-07: no exception); only the three names that exist in both `cache`
and `backing` (`ParentOf`, `SetXattr`, `RemoveXattr`) get distinguishing
names (e.g. `BackingSetXattr`). A wrapper class is allowed only if the
clashes turn out to be more than those three and renaming reads worse.

**Inside `namespace dcfs`, names of `dcfs` are not qualified** (russ,
2026-10-10, on `dcfs::DcfsErrnoToStatus(...)` in `backing.cc`: "We're
already inside the dcfs namespace, so unless ambiguous, you should not
name it"). `ErrnoToStatus(errno, "...")`, not `dcfs::ErrnoToStatus(...)`.
The two exceptions: a macro body, which expands anywhere and so qualifies
fully (`::dcfs::ErrnoToStatus`, as `status_macros.h` does), and a real
ambiguity, which is named in a comment at the call. Unqualified lookup
inside `dcfs` finds `dcfs::ErrnoToStatus` before `absl::ErrnoToStatus`
regardless, so the qualifier never did anything there. Mechanically:
`tools/style_matchers/dcfs_qualified_inside_dcfs.query` (25.21) finds a
`dcfs::` qualifier with no leading `::` inside `namespace dcfs` in `dcfs/`,
`bench/` and `tools/`, from the AST (it replaced the regex of
`tools/repo_shape.py`, which guessed the namespace from brace depth; today
0 sites).

**A name says what distinguishes the thing from its siblings, and never
repeats the namespace.** The function now called `ProducedErrnoToStatus`
was `DcfsErrnoToStatus` until 25.14. It exists beside `ErrnoToStatus`
because the two differ in origin: `ErrnoToStatus` forwards a syscall's
answer, the other builds an errno dcfs itself chose (25.3). "Dcfs" named
neither; inside `namespace dcfs` it was the namespace said twice. The
distinguishing word goes in the name: `ProducedErrnoToStatus` beside
`ErrnoToStatus` (matching the predicate `ProducedByDcfs`, where "Dcfs"
is the contrast with the backing filesystem, not a prefix). Rule: an
identifier in `dcfs/` production code does not begin with `Dcfs`; a
project-name word elsewhere in a name is allowed only as a contrast with
something that is not dcfs (`ProducedByDcfs`, the `fuse.dcfs` mount type)
and the comment says what it contrasts with. Mechanically: `repo_shape.py`
refuses identifiers matching `^Dcfs[A-Z]` in `dcfs/*.h` and `dcfs/*.cc`
(25.14; the two names it was written against, `DcfsErrnoToStatus` and
`DcfsMountDevice`, are renamed).

### 1.4 Enums

`enum class`, nested inside the type it belongs to when it belongs to one
(`struct LookupResult { enum class Kind { kFound, ... }; Kind kind; };`),
converted when touched. The four nested unscoped `enum Kind`s were
converted in step 25.1. A scoped enum has no `operator<<`, so
`RET_CHECK_EQ/NE` print its underlying value (`RetCheckStreamable`).

### 1.5 `syscalls.h`

One thin wrapper per documented Linux syscall or libc call, named for its
manpage in lower case (`man 2 openat`, `man 3 ...`; `syscalls::setxattr`),
returning `absl::Status` or `StatusOr` through `dcfs::ErrnoToStatus`. One
call, no composition (no retry loops, no decoding into containers, no
policy). **The wrapper's return type is the one place that records
whether a call can fail** (russ, 2026-10-10: "for the most part all
syscalls will return a Status indicating failure (that's _why_ we have
syscalls.h)"): a wrapper returns `Status`/`StatusOr` unless the manpage
says the call cannot fail, in which case it returns the value or nothing
(`umask`), and the special cases where failure comes back inside the
result (a struct with a per-item error, a partial count, or `setfsuid`'s
silent refusal that only a read-back with the -1 sentinel detects, which
the wrapper does and returns as a `Status`, 25.15) are decoded by the
wrapper into a `Status`/`StatusOr` or documented at the wrapper. Callers never need to know which
is which: `Status` is `[[nodiscard]]`, so a call that can fail cannot be
dropped, and a bare call compiles only for one that cannot (1.6a). Anything else is a helper in `backing.cc` built on the plain
wrappers: the `/proc/self/fd/N` trick is a backing helper calling
`syscalls::getxattr(path, ...)`. A wrapper that is thin but has a
non-manpage name is renamed to its manpage name, with a comment on any
per-thread raw-syscall detail (`setgroups_thread` becomes `setgroups`).
Nothing that is not a wrapper lives in `dcfs::syscalls`, so no ADL hook can
be found through it: checked by grep, the only template there is `ioctl`,
and `LogOpenFlags`/`AbslStringify` live in `dcfs/log_open_flags.h` as a
hidden friend at `dcfs` scope. Two documented irregulars: `dup` is
`fcntl(fd, F_DUPFD_CLOEXEC, 0)` under the name of the manpage's `dup(2)`,
and `linux_dirent64` is a type (glibc has no `struct linux_dirent64`; the
layout is from `man 2 getdents`), not a wrapper. The process-control wrappers
(`fork`, `execv`, `waitpid`, `kill`, `dup2`, `_exit`) are in
`dcfs/syscalls_process.h`, a separate library for `bench/` and the
`mount.dcfs` wrapper (its daemon fork and the capture helper's mount(8)),
the only one `tools/banned_symbols.txt` lets reference `execv`. Why three
libraries: `syscalls_backing.h` is its own target so that
`//tools:syscalls_backing_users_test`'s golden list enforces, in the build
graph, that every backing-reaching syscall is made in `backing.cc`, which
keeps the fault sweep and the trace recorder complete, since both hook
`backing.cc`; `syscalls.h` is the process-local set anyone may call; and
`syscalls_process.h` exists so that `bench/` and the tools get wrappers
without dcfs's libraries.

**No raw syscalls anywhere** (russ, 2026-10-07: a firm rule). Syscalls go
through `dcfs/syscalls.h` and `dcfs/syscalls_backing.h`, where the failure
is handled and converted into a `Status`: no call of a libc syscall wrapper
outside `dcfs/syscalls.cc`, `dcfs/syscalls_backing.cc` and
`dcfs/syscalls_process.cc`, in production code, tests, `dcfs/testonly/`,
`bench/` and any C++ in `tools/`; and no `std::filesystem` or file stream
(`std::ifstream`, `std::ofstream`, `std::fstream`), which open and walk files
behind the wrappers (`dcfs/testonly/files.h` has `ReadFileToString`,
`ListDirectory`, `ListTree`, `RemoveAll` and `FileSize` over them). A test
calls the `syscalls::` wrappers and checks them with
`ASSERT_OK`/`ASSERT_OK_AND_ASSIGN`, or `.IgnoreError()` where failure is
irrelevant; a missing wrapper is added to `syscalls.h`, not worked around.
The `-Wl,--wrap` fakes of a `*_test.cc` define `__wrap_name` and call
`__real_name`, which are not calls of the libc name. Two exceptions, not
scanned: `tools/fhtest.c` is a copy of fuse-generation-qemu's
`guest/fhtest.c` kept in sync by hand (third-party code, never restyled) and
`tools/testutil.c` is a C program for the guest, kept in C alongside it;
neither can use the C++ wrappers; and `tools/banned_symbols_fixture.cc`
makes banned calls on purpose. `//tools:raw_syscalls_test` enforces the rule
over `dcfs/`, `dcfs/testonly/` and `bench/` (names in
`tools/raw_syscalls_names.txt`). It cannot catch a call through a macro or
function pointer, a name not in the list, a stream or filesystem type named
without `std::`, a raw string literal containing a quote, or a local
function that shares a name (a false positive: rename it); `remove` is not a
listed name, because it would flag the `std::remove` algorithm (the file
calls are `unlink` and `unlinkat`).

### 1.6 Errors

**`absl::Status`/`StatusOr<T>` are the only error channel**: no error
codes, no `errno` outside the syscall wrappers. `std::optional<T>` means
"absent is normal"; `StatusOr<std::optional<T>>` when it can also fail.

A function returning an owned descriptor carries `[[nodiscard]]`; one
returning `Status` or `StatusOr` does not, the type already is (1.2).

**Propagate with Abseil's macros, by their short names** (russ,
2026-10-10): `RETURN_IF_ERROR` and `ASSIGN_OR_RETURN`, which
`absl/status/status_macros.h` defines as aliases of `ABSL_RETURN_IF_ERROR`
and `ABSL_ASSIGN_OR_RETURN` when `ABSL_DEFINE_UNQUALIFIED_STATUS_MACROS`
is set; `//dcfs:status` sets it as a Bazel `defines` so every dependent
gets it, and `dcfs/status.h` fails the build if it is missing (25.15).
Never the `ABSL_` form (25.17 swept the 835 uses on 2026-10-10; the
repo-shape rule `absl_prefixed_status_macros` keeps them out). The tree defines no propagation macros. Its
own macros are `RET_CHECK`, `RET_CHECK_EQ/NE/GT/OK` (`dcfs/ret_check.h`, 68
uses): they return a `kInternal` `StatusBuilder` and take `<<` context. Use
`RET_CHECK` where a `Status` can be returned. Where it cannot (a libfuse
callback, startup), a crash is still not the answer: the no-intentional-
crashes rule applies (1.6b; `CHECK`, `LOG(FATAL)`, `abort`
are out of production code, each existing one needs russ's case-by-case
approval), so the 23 `CHECK_NE(ptr, nullptr)` in `fuse_ops.cc` and the
`CHECK` in `RestoreRoot` (`backing.cc`) are on plan step 25.9's list, not
a pattern to copy. Never `assert`.

**Build a status one of three ways (russ: StatusBuilder, not `StrCat`).**

1. A failed syscall or libc call: `dcfs::ErrnoToStatus(errno, what)`,
   immediately after the call, never `absl::ErrnoToStatus`: only the dcfs
   wrapper attaches the `kErrnoTypeUrl` payload that `GetErrnoFromStatus`
   and `StatusToErrno` read (`status.h:15-19`). An error the kernel should
   see as a given errno is built the same way (`ErrnoToStatus(ENOTDIR,
   "the source is not a directory")`, `backing.cc:462`).
2. Any other new error: a builder helper from `dcfs/status.h`. Abseil has no
   `absl::InternalErrorBuilder` and the like, so `dcfs/status.h` provides
   `InternalErrorBuilder()`, `FailedPreconditionErrorBuilder()`,
   `NotFoundErrorBuilder()`, `InvalidArgumentErrorBuilder()`,
   `AbortedErrorBuilder()`, `UnimplementedErrorBuilder()`,
   `AlreadyExistsErrorBuilder()` and `ResourceExhaustedErrorBuilder()`, each
   returning `absl::StatusBuilder(absl::StatusCode::kX, loc)` with `loc` a
   defaulted `absl::SourceLocation::current()` parameter, so the status
   records the caller's line:

   ```
   return InternalErrorBuilder()
          << "fuse_session_mount(" << mountpoint << ") failed";
   ```

   Not `absl::InternalError(absl::StrCat(...))` or any `absl::XError(...)`
   in production code. A code with no helper uses
   `absl::StatusBuilder(absl::StatusCode::kX)` directly.
3. Context on a status from elsewhere: `absl::StatusBuilder(status) <<
   "..."` or `RETURN_IF_ERROR(expr) << "..."` (tested,
   `status_test.cc`; used in `main.cc` and `device_id.cc`).

**StatusBuilder keeps the errno payload.** Verified three ways. The header:
`StatusBuilder(const Status &)` wraps the original status and `SetPayload`
only adds (`status_builder.h:162-175`, `:290-297`). The source: the built
result is made by `StatusRep::Clone(message, include_payloads=true, ...)`
(`status_builder.cc` `SetMessage`; `internal/status_internal.cc:216`). The
tests: `ErrnoPayloadTest.SurvivesStatusBuilderAnnotation` and
`...SurvivesAbslReturnIfErrorAnnotation` (`status_test.cc:86,94`) recover
`ENOENT` from an annotated `ErrnoToStatus`. So add context to an errno
status with `StatusBuilder`, never by rebuilding it with
`absl::Status(code, message)` (that drops the payload and `StatusToErrno`
falls back to its code table). Not verified: `.SetCode()` on an errno
status (nothing does it). The streamed text is joined as `original;
added`, so start it with words, not `;` or a space.

**Codes** (production constructor counts):
- `kFailedPrecondition` (16): a startup check failed (`main.cc:144,186,300`),
  the cache database is corrupt or belongs to another filesystem
  (`migrate.cc:54,75,301`, `backing.cc:483`), an object used in a state it
  cannot be (`sqlite.cc:335`).
- `kNotFound` (10): the cache has no such row; callers use
  `absl::IsNotFound` (`metadata_cache.cc:88`, `mount_fds.cc:25`).
- `kInternal` (5, and every `RET_CHECK`): a broken invariant, or a libfuse
  call that failed without an errno (`main.cc:483-531`): a bug in dcfs.
- `kInvalidArgument` (4): malformed input (`file_handle.cc:83`).
  `kAborted` (2): the inode changed since the mutation resolved it; the
  caller retries three times, then `EAGAIN` (`metadata_cache.cc:1413`,
  `dir_cache_fs.cc:824-831`). `kUnimplemented`, `kAlreadyExists`,
  `kResourceExhausted`: as named.
- `kUnavailable` is BUSY and LOCKED from SQLite (`EAGAIN`: retry). A
  failure of the cache database's storage (`SQLITE_IOERR`,
  `SQLITE_READONLY`) keeps the code and carries an errno payload `EIO`
  (`sqlite.cc`, item 1: an error the kernel should see as a given errno is
  built with that errno), so that a caller is not told to retry it.
- A status that reaches a FUSE reply and means something to the client
  carries an errno (item 1); without one `StatusToErrno` uses its code
  table (`kInternal` becomes `ELIBBAD`, `kFailedPrecondition` `EBUSY`): a
  diagnostic, not an answer.

**Messages (russ's rules for accumulating Status messages).** A status
gathers text as it travels up the call chain, joined as `first; added;
added`, so each function adds only what the reader above it cannot know.

1. **The function that creates the error says what it was operating on,
   not why.** `ErrnoToStatus(errno, "openat(5, <name>)")` or
   `NotFoundErrorBuilder() << "No cached inode " << id`; never `"... while
   reading the directory"`: the callers add that.
2. **A function that passes a status through adds what it asked the
   failed callee to do, not what it is itself doing.** Describe the call
   (`"while opening the backing file"`), not the enclosing function (not
   `"while in Release"`), and not the arguments it passed: the callee
   names them if they matter. Describe an argument the function was given
   only if it did not pass it to the callee that failed and naming it
   makes the message easier to read.
3. **No terminal punctuation**, so that a caller can append.
4. **Capitalise the first error, not added context.** The first word of
   the first error is capitalised (`No cached inode 7`) unless it is an
   identifier: a syscall, function or flag name (`fstat`, `DeviceId::Parse:
   expected a 24-byte value`, `--cache_db is a symlink`). Context added
   later starts in lower case (`while opening the backing file`) and does
   not begin with `;` or a space.
5. **Not every function needs to add context**: a bare
   `RETURN_IF_ERROR(Foo())` is right when the callee's message
   already says it. Do not add a line that repeats the callee's.

A syscall failure names the call and the arguments that identify the
object: `openat(%d, <name>)` (`syscalls.cc:39`); a bare name when every
argument is a descriptor (`"fstat"`). A name, symlink target or xattr name
from the backing filesystem goes through `EscapeBytes` (`dcfs/escape.h`)
in every message and log line (`backing.cc:387`; `docs/design.md`, "File
names are bytes"). Paths dcfs builds (`/proc/self/fd/N`) and flag values
are text.

Example, with the helpers of this tree (`Seek` and `ReadContents` are
illustrative):

```
absl::StatusOr<std::string> Open(std::string_view path) {
  return NotFoundErrorBuilder() << "Could not find " << EscapeBytes(path);
}
absl::StatusOr<std::string> ReadAtOffset(const BackingFile &file,
                                         off_t offset) {
  RETURN_IF_ERROR(Seek(file, offset)) << "while seeking the file";
  ASSIGN_OR_RETURN(
      std::string contents, ReadContents(file),
      // `offset` is named because it was not passed to ReadContents
      _ << "while reading the file contents at offset " << offset);
  return contents;
}
```

A failure of `Open` reads `NOT_FOUND: Could not find <path>` plus whatever
its callers added; of `ReadContents`, `<callee's message>; while reading
the file contents at offset 4096`. The text recurses readably without
repeating itself, because each added piece says something new.

**Return a failed status or log it, never both** (1.7).

### 1.6b No intentional crashes

russ, 2026-10-09: "no intentional crashes. E.g. no use of `LOG(FATAL)` or
`CHECK` (`RET_CHECK` is ok because it doesn't crash). I understand the
necessity sometimes, but try hard to avoid it (for instance, another
style rule that's in an Abseil TOTW somewhere: don't call methods that
can fail inside constructors, instead add a static Create method). If
you _really_ feel you need a crash, bring it to me for review and I'll
consider on a case-by-case basis." (The rule lived only in plan step 25.9
until 2026-10-10; this section is it.)

- **Production code** (`dcfs/`, `bench/`, the shipped `tools/*.c`) has no
  `LOG(FATAL)`/`LOG(QFATAL)`, `CHECK*`, `QCHECK*`, `DCHECK*`, `abort()`,
  `assert()`, `std::terminate`, or an `exit()` standing in for an error
  return. A failure is a `Status`; an invariant is a `RET_CHECK`, which
  returns `kInternal`. Fallible construction is a static `Create`
  returning `StatusOr` (TotW #42, and the Google style guide's "Doing Work
  in Constructors").
- **Every remaining crash site is one russ approved**, listed in the
  repo-shape allowlist with his approval date and the reason the process
  could not continue correctly; nothing else. Today's sites (the 23
  `CHECK_NE(ptr, nullptr)` in `fuse_ops.cc`, `RestoreRoot`'s `CHECK` in
  `backing.cc`, `fuse_request.cc`'s double-reply `CHECK`, `fork_split.h`'s
  `abort()`, one `assert`) await his ruling in 25.9.
- **Tests may crash on what is not under test, and must not ASSERT it**
  (russ, 2026-10-09): "crashes are permitted in tests when invariants
  fail that aren't under test ... The inverse applies too. Don't
  ASSERT/EXPECT properties that aren't under test ... there's a clear
  distinction between 'the test failed' and 'the infrastructure failed to
  run the test'." Setup that is not under test uses Abseil's plain
  `CHECK`/`CHECK_OK` and, for a `StatusOr`, `CHECK_OK_AND_ASSIGN(lhs,
  expr)` in the style of `ASSIGN_OR_RETURN` (russ: no custom helper
  beyond that macro; "googletest understands the difference between 'the
  process crashed' and 'ASSERT/EXPECT failed'"). `ASSERT_*`/`EXPECT_*` are
  for the property the test is about.
- **Mechanically** (25.9): `tools/banned_symbols.txt` bans `abort`,
  `__assert_fail` and Abseil's fatal-log internals in the shipped
  binaries; `tools/style_matchers/check_in_production.query` (25.21)
  reports a `CHECK`, `QCHECK`, `DCHECK`, `LOG(FATAL)`, `assert` or `abort`
  in any file that is not `*_test.cc` or under `testonly/`; the sites that
  existed are in the shrinking `tools/style_checks_allow.txt`.

### 1.6a Shape of a function that does several things

russ, 2026-10-10, rewriting `SwitchTo` in `backing.cc`. The shape he
wants, as the worked example:

```c++
absl::StatusOr<SavedGroups> SwitchTo(const Credentials &caller) {
  RET_CHECK_EQ(FsUid(), 0u) << "credential switch already active";
  RET_CHECK_EQ(FsGid(), 0u) << "credential switch already active";
  ASSIGN_OR_RETURN(SavedGroups saved, GetGroups());
  absl::Cleanup restore_root = [&saved] { RestoreRoot(saved); };
  RETURN_IF_ERROR(syscalls::setfsgid(caller.gid));
  RETURN_IF_ERROR(syscalls::setgroups(caller.groups));
  RETURN_IF_ERROR(syscalls::setfsuid(caller.uid));
  std::move(restore_root).Cancel();
  return saved;
}
```

Why `setfsuid` returns a `Status` here when the syscall reports no error:
russ, 2026-10-10, once the reviewer showed an unmapped id is possible
under idmapped mounts: "Ok, so it's possible then. In that case, do the
check in syscalls.h and make the setfsgid/setfsuid functions return a
Status." The wrapper refuses the query sentinel (-1) with EINVAL, calls
the syscall, reads the id back with the sentinel form, which setfsuid(2)
documents as the only way to detect failure, and returns EPERM if it did
not take. That is 1.5's "failure comes back inside the result, decoded by
the wrapper" case, and the caller is back to the general form with no
guard and no comment.

(russ's original had `absl::Cleanup restore_root([&saved]() {...});` and
a `// ... is guaranteed to never fail` comment on each bare call; the
form above applies the rulings of 2026-10-10 below.)

What it replaced had an `absl::Status status;` filled by three
`if (status.ok())` steps, a nested block for the last one, a hand-written
`RestoreRoot` in the error branch, read-back checks for a case no FUSE
request can produce (the kernel never sends a request whose uid or gid
is unmapped in the connection's user namespace: `fuse_simple_request`
fails the caller with EOVERFLOW first, fs/fuse/dev.c), and
`dcfs::`-qualified names.

- **Do the thing; if it broke, handle and return; do the next thing**
  (rule). russ, 2026-10-10: "The general form for code should be:

  ```c++
  do thing;
  if (thing broke) {
    handle error
    return error code;
  }
  do next thing;
  ```

  So the happy path is the unindented one. Then, if you don't need
  special logic for handling the error, you can get rid of the `if` using
  RETURN_IF_ERROR or ASSIGN_OR_RETURN. But the 'do thing, handle error,
  do next thing' pattern is the pattern to follow." Preconditions and
  invariants are `RET_CHECK`s at the top, which are the same shape with
  the handling folded in. After them the body reads top to bottom as the
  sequence of effects; every failure is handled right after the call that
  can produce it, in an `if` that ends with a `return`, so there is no
  `else`, no `if (ok) { next thing }` nesting, and no `status` carried
  across statements to be checked later. A happy path that is indented
  is failure handling written in the wrong place: move it.
- **Undo is an `absl::Cleanup` declared right after the thing it undoes,
  and success cancels it** (rule). Each early return then undoes exactly
  what happened before it with no code of its own, and the commit point
  is the one `std::move(x).Cancel()` line. Hand-written undo in an error
  branch (`if (!status.ok()) { RestoreRoot(saved); return status; }`) is
  the shape to refuse.
- **The signature says whether a call can fail; nothing else needs to**
  (russ, 2026-10-10: "If the syscall can fail it returns a Status, and
  Status cannot be silently ignored (it has `[[nodiscard]]`)"). A
  `syscalls::` wrapper for a call that can fail returns `absl::Status` or
  `StatusOr`, which the compiler refuses to drop; one for a call that
  cannot fail returns its value or nothing (`syscalls::setfsuid` returns
  the previous uid, `syscalls.h:34`). So a bare call is correct by
  construction and needs no comment, no result check, no read-back and no
  branch (1.10a). The one thing to get right is the wrapper's signature
  (section 1.5): a syscall that can fail must not be wrapped as if it
  could not.
- **Comments are facts at the line that needs them** (rule). The header
  says what the function does and the one non-obvious constraint (here:
  fsgid and groups before fsuid, and why); it does not narrate the
  statements.
- **Prefer a `Status` consumed by the statement that makes it**
  (preference, not a rule; russ: "Sometimes you need to do something in
  between, but prefer other constructs if possible").
  `RETURN_IF_ERROR(f())`, `ASSIGN_OR_RETURN`, `RET_CHECK`. A
  `Status` variable that outlives one statement, and a ladder of
  `if (status.ok())`, is the failure path written by hand and hides which
  call failed; reach for it only when something must happen between the
  failure and the return that a `Cleanup` cannot express, and say what.
- **Rulings from the first style review (25.15; orchestrator's rulings,
  2026-10-10, under russ's "use your judgement"; russ may override any):**
  1. When one function handles the same failure after several calls, the
     calls move into a helper returning `Status`/`StatusOr` written with
     `RETURN_IF_ERROR`/`ASSIGN_OR_RETURN`, and the caller handles the
     failure once. A lambda that captures and changes the caller's
     locals, or a copied handler, is the sign.
  2. A type whose destructor does the scope-exit work (`cache::Mutation`
     ends itself) is the Cleanup: no explicit call before an early
     return. An explicit call only where the work must happen before
     later code in the same scope, with a comment saying what must
     follow it.
  3. A conditional second step (`if (s.ok()) s = Next();`) becomes a
     helper written with `RETURN_IF_ERROR`, even for one use: a free
     function in the `.cc`'s anonymous namespace when the public
     interface suffices.
  4. Two statuses that may both fail are never joined (russ,
     2026-10-10: "If two statuses may fail, it's often ok to just return
     the first. Usually, the second is a consequence of the first so is
     less useful anyway. Don't try to join Statuses."). Return the first;
     the second is returned only when the first succeeded. No
     "additionally, ... failed" builders, no `Status::Update`.
  5. A bare call of a cannot-fail wrapper carries no "cannot fail"
     comment (1.5 says why). Where such a call silently ignores some
     inputs, the first question is whether the input can arrive at all;
     if a configuration we may adopt makes it possible, the wrapper
     checks and returns a `Status` (russ, 2026-10-10: `setfsuid`,
     `setfsgid`); if it is a true invariant, it is asserted as a guard
     (`RET_CHECK_NE`), never described in a comment (russ: "This kind of
     case deserves a RET_CHECK_NE rather than a comment").
  6. A Cleanup is declared `absl::Cleanup name = [captures] { ... };`
     (Abseil's documented form, no empty parameter list).
- **Program output is `absl::PrintF`/`FPrintF`/`SNPrintF`/`StrFormat`,
  never iostreams and never the C printf family** (russ, 2026-10-10: "I
  agree", after asking what the 19 sites do: `--version` lines,
  usage, fsck.dcfs's report routed to stdout or stderr by status, bench's
  messages and its `snprintf` into dm ioctl structs' fixed `char[]`
  fields). The output is interface, not logging, so it keeps its bytes and
  its stream; only the formatting call changes to the type-checked one
  (`absl::SNPrintF` takes the same buffer and size). Log lines are `LOG`.
- **Mechanically** (25.21, 1.1): clang-tidy's `readability-else-after-return`
  (which covers `return`, `break`, `continue` and `throw`),
  `readability-misleading-indentation` and
  `readability-function-cognitive-complexity` with `Threshold: 15` (tighten
  as the tree allows, never loosen without russ; a finding reads "break up
  this function"; the complexity of a macro's expansion is not counted).
  `status_uninitialized.query` reports (does not fail on) a `Status` or
  `StatusOr` local declared without an initializer, and
  `happy_path_nested.query` an `if (x.ok())` with three or more statements in
  its block, directly in a function body. The accumulator preference is not
  enforced.

### 1.7 Logging

Abseil logging, `absl/log/log.h` and `absl/log/check.h`, with Abseil's own
semantics and flags and nothing on top: no wrapper macros, no `DLOG` (the
shipped binary is the tested one), no `--log_dir`. `main.cc` sets the
default stderr threshold to WARNING before flag parsing (russ: errors and
warnings are visible by default); `--stderrthreshold=0` adds INFO,
`--minloglevel` drops levels everywhere, `--v` and `--vmodule` turn on the
verbose levels below. Rate-limit a site with `LOG_EVERY_N`,
`LOG_EVERY_N_SEC` or `LOG_FIRST_N` where it can fire per request; there is
no blanket rule. A daemonised dcfs's stderr is `/dev/null`: it logs to
syslog, at or above the same `stderrthreshold` (docs/design.md,
"Daemonization").

**Levels (russ, 2026-10-08).**
- `FATAL` (`CHECK`): dcfs cannot continue safely: a violated invariant, or
  a cache whose schema or identity cannot be reconciled.
- `ERROR`: the caller got an error that dcfs produced rather than one it
  forwarded from the backing filesystem, or dcfs refused to do its job:
  startup refusals, a failed reply or close, a cache-disk I/O error shown
  as `EIO`, a backing change that could not be recorded, a failed recovery
  probe.
- `WARNING`: nothing failed for the caller but state is degraded or
  surprising: an out-of-band change, recovery after an unclean shutdown, a
  loose cache mode, a missing kernel capability with a fallback.
- `INFO`: the lifecycle narrative: start (source, cache, mount point,
  options), the recovery summary with counts, each sync point with rows
  cleared and duration, shutdown clean or unclean with the reason, and the
  first backing access after an idle period.
- `VLOG(1)` (`--v=1`): one line per request that reached the backing
  filesystem, and why. `VLOG(2)`: every request with its reply. `VLOG(3)`:
  SQL statements and step counts. A message that builds strings is guarded
  with `VLOG_IS_ON(n)`: the hot paths stay cheap at the default level.

**Return a failed Status or log, never both.** A function that returns a
non-OK status does not also log it: its caller, or that caller's caller,
logs it with more context, so logging in both places prints the same
failure twice, the inner line with less context. Two exceptions:
1. The outermost caller of a request, the FUSE operation handler that
   turns a status into an errno reply (`FuseRequest::ReplyFailure`), logs
   `ERROR` for an error dcfs produced, and does not for one forwarded from
   the backing filesystem (`ENOENT` from the backing is the answer, not a
   dcfs failure; `--v=2` shows the reply).
2. A fire-and-forget path with no caller to return to (a `Release` after
   the reply, `main`'s shutdown, a destructor) logs what it could not do
   and returns nothing.

A status that is consumed (retried, turned into "unknown", or ignored on
purpose) is logged where it is consumed, once.

### 1.8 The dcfs invariants as code rules

The review checklist of `docs/plan/process.md`; the reviewer greps for
each.
- **`Context &ctx` first** in every function that reads or writes the
  cache database, mount table, fill guards or dirty set (`const Context &`
  to only read, `cache::BeginFill`). Backing helpers that touch only a
  descriptor take none (`backing::ReadFile(int fd, ...)`). Exception: the
  protocol-event scopes take the recorder first (`protocol_events.h:381`).
- **No globals, singletons or `thread_local`** (0 of each). Shared state
  lives in `Context` (`context.h:84`) or in an object the caller owns.
- **No paths after startup.** Objects are reached by descriptor or file
  handle (`FileHandle::Open`); a child is `openat(dir_fd, name, ...)`, never
  a joined path. Only startup uses a path (`--source`, the cache database).
- **Syscalls that can reach the backing filesystem, i.e. anything given a
  backing fd, file handle or name, are made only in `backing.cc`**, where
  the idle guarantees are reasoned about, and in the lower layers it is
  built from and that only it calls: `file_handle.cc` (`name_to_handle_at`,
  `statx`, `openat`, `open_by_handle_at`) and `device_id.cc` (`fstatfs`,
  `ioctl`); plus startup in `main.cc` (`openat` of `--source`, `fstat`).
  Process-local syscalls (resource limits, credentials, `/proc` reads, the
  cache database file, mount tables) may call the `syscalls::` wrappers
  directly from any file. The build graph enforces the split: the
  backing-reaching wrappers are `//dcfs:syscalls_backing`
  (`syscalls_backing.h`), the process-local ones `//dcfs:syscalls` (`syscalls.h`), and
  `//tools:syscalls_backing_users_test` compares the targets that depend on
  the former with `dcfs/syscalls_backing_users.txt` (a new dependent is a
  reviewed edit; `dir_cache_fs` and `metadata_cache` are not on the list).
  That is why the wrappers are three libraries (section 1.5): the split
  makes the invariant a property of the build graph, which keeps the fault
  sweep and the trace recorder, both hooked at `backing.cc`, complete.
  `mounts_below` is on it for its two `/proc/self/mountinfo` reads (`openat`,
  `read`: procfs, no backing disk). The mount.dcfs wrapper's modules are on
  it too: `OpenBacking` (`backing_capture.cc`) opens SOURCE for the `bind`
  form and, in `CaptureBacking`, calls `openat(tree, ".")` for a real
  directory descriptor on the root of the filesystem it just captured, both
  once at startup before dcfs serves anything (what `main.cc` did before);
  `absolute_paths`, `remount` and `startup_channel` read procfs and a
  socket.
  Only the three wrapper files call libc
  directly. `cache::` is pure SQLite: it never sees a descriptor.
- **No transaction spans a backing syscall.** Backing I/O first, then one
  short synchronous transaction (`ctx.db.Transaction(...)`); no statement
  cursor held across a syscall; `AsCaller` wraps one syscall.
- **Tri-state records.** A record mirroring the backing filesystem is
  present, absent or unknown; a mutation sets it unknown before the
  backing syscall and present or absent after; a failure leaves it unknown.
- **A protocol change updates the TLA+ model** in the same change
  (section 6).
- **Names are bytes**: `std::string`/`string_view`, `char *` only at the
  syscall and libfuse boundary, `EscapeBytes` when printed, `memcmp` order.
- **Requests are cancellable** (from phase 22): an operation over about
  100 ms checks for interruption at safe points and has a cancellation
  test.

### 1.9 Comments

Google asks for comments on non-obvious declarations; the norm here is
more: about one line in four (3,706 of 15,580), saying why, not what.
- Code added for a reason names its source: the plan step (`step 23.2`,
  `dir_cache_fs.cc:532`), the audit finding (`audit-tristate F1`,
  `context.h:37`), the conformance case ("found via pjdfstest's chmod/02.t:
  a name one byte past NAME_MAX got ENOENT ... instead of ENAMETOOLONG",
  `backing.cc:1185`), the model step (`// Model: UnlinkPhase1's ENOENT
  branch`, `dir_cache_fs.cc:836`) or the commit (`fuse_request_channel_
  test.cc:1`: "Regression test for 0795d12").
- Length: one to three lines for a block, up to about ten for a non-local
  invariant; longer reasoning goes in `docs/design.md` and the comment
  points to it. No restating the code, no changelog ("fixed in step N" is
  the commit's).
- `/* */` only for argument names (`/*is_dir=*/false`) and in `tools/*.c`.

### 1.10 One abstraction over variants; branching is discouraged

- **One abstraction over variants, not an `if` at every use.** When a value
  can come from two sources or a behavior has two forms (a filesystem's UUID
  or the mount-point-plus-fsid fallback; relatime or noatime; a kernel
  capability present or absent), build one object once from whichever
  applies and give the rest of the code a single interface; never branch on
  the variant at each use site. In the tree: `Context::atime` is an
  `AtimePolicy` chosen once from the mount options (`context.h`), and the
  read path asks the policy, it does not re-read the options; the Phase 14
  source identity will be one object built at start-up from the UUID or the
  fallback.
- **Branching is discouraged, not forbidden**: sometimes a branch is
  necessary. Every branch is another path to understand and to test
  separately, so look for the shape that needs none before writing one:
  - **Inject dependencies and use small fakes** instead of test-only
    conditionals: never `if (!in_test) talk_to_db()`. Production has zero
    test-only branches (`AGENTS.md`: fakes, not mocks; no code path exists
    only so a test can run outside the guest; the tests section, "Fakes, not
    mocks" and the 25.4 rules). `Context::clock` is an `absl::Clock *`: the
    real clock in production, a fake in a test, and no code asks which.
  - **Use the numeric or structural properties of values** so the absent or
    "off" case needs no test: `int num_foos = 0` rather than
    `optional<int>` when zero is the right absent value (it is not always:
    say so when zero means something else); `options.quota = INT_MAX` to turn
    a quota off with no `if` anywhere; an empty set that every loop handles
    for free; a no-op implementation of an interface instead of a null
    check: `Context::events` is `&NoProtocolEvents()` (every method does
    nothing) in production, so no call site tests it for null.
  - **A table over an if-chain** where a table fits (the `SWAPS` and
    `REPLACEMENTS` dictionaries of `tools/mutation/operators.py`), and one
    object built once over variants (the first rule).
- `Status` returns with `RETURN_IF_ERROR` are the normal shape of error
  handling and are not what this rule is about (section 1.6).

### 1.10a Every branch names its reachable cause; no code for the impossible

russ, 2026-10-10, on `GetGroups`'s "another thread cannot change ours, but
be exact anyway" retry: "The loop in this method is obviously wrong and the
comment even says so ... it's unnecessary code, and writing unnecessary
code causes unnecessary cognitive overhead in comprehension of the code."

- **A branch exists for an input or event the author can name.** Every
  `if`, `else`, `continue`, early return and error path handles a case
  that some concrete input, syscall answer or concurrent event reaches, and
  where that is not obvious the comment says which. Code for a case that
  cannot happen is not defensive; it is false documentation that every
  reader must disprove for themselves. Delete it. If the impossibility is
  an invariant worth asserting, assert it: a `RET_CHECK` that names the
  invariant reports a violation as the bug it is, instead of handling it
  as if it were expected (section 1.6; never a crash, docs/style.md
  "No intentional crashes").
- **A confession in a comment marks a defect.** "Cannot happen", "can't
  happen", "shouldn't happen", "just in case", "be exact anyway",
  "defensive", "paranoia", "for safety" next to a branch mean the author
  knew the branch was unreachable and wrote it anyway. Mechanically:
  `tools/repo_shape.py` refuses those phrases in comments of production
  files (`dcfs/*.cc`, `dcfs/*.h`, `tools/*.cc`) outside an allowlist that
  only shrinks (plan step 25.13).
- **A surviving mutant is unnecessary code or a missing test, and either
  blocks the merge.** The per-push mutation job (`mutation-changed`,
  26.5b/26.5d) deletes and flips the code a push touches; a mutant that no
  test kills is, from 25.13 on, a failure of that job, not a summary line.
  It is resolved by a test that kills it, by deleting the code, or by an
  entry in `tools/mutation/equivalent.txt` with a reason (the existing
  mechanism for mutants that change no behaviour). An unreachable branch
  is exactly a mutant that survives forever, so this is the check that
  finds what nobody noticed.
- **Reviewers ask the reachability question.** For every branch in a diff,
  name the input that reaches it; a branch the reviewer cannot reach is a
  finding, whatever the comment says.

### 1.11 No timers

russ, 2026-10-09, on the restart race: "I don't like timers in our code.
They're necessary sometimes ... Timers are inherently brittle. If things are
slow, the system breaks. This system should work on everything ranging from an
idle 256 core supercomputer to a 1 core raspberry pi under 40 loadavg."

- **Wait on the event, and let the caller cancel.** To wait for another
  process, the kernel or a filesystem, block on what signals the change: a
  `pidfd` for a process's exit, a lock (`flock`, which the kernel releases
  when its holder dies), a `read` or `poll` with no timeout, an inotify watch.
  The wait ends when the thing happens or when the caller cancels it (a signal:
  Ctrl-C, systemd killing the helper); it never ends because a number of
  seconds went by. A slow machine then makes everything slower and nothing
  wrong.
- **A timeout is permitted only where the thing waited for cannot signal**, and
  then it is named and justified in a comment: say what cannot signal and what
  the duration is a bound on. In the daemon each use is an `allow` line of
  `tools/banned_symbols.txt` that names it.
- **Mechanically:** `tools/banned_symbols.txt` bans `sleep`, `usleep`,
  `nanosleep`, `clock_nanosleep`, `alarm`, `timer_create`, `timerfd_create`,
  `sqlite3_busy_timeout` and `sqlite3_busy_handler` in the shipped binary
  (`//tools:banned_symbols_test`). A SQLite lock that is not free is an
  immediate, clear error: one daemon owns a cache database (the `flock`
  `main.cc` takes), so a conflict is that check or a bug, never something to
  retry. `tools/repo_shape.py` refuses a bare `sleep` in a `test/qemu/guest`
  script (`//tools:repo_shape_test`), except `justified_sleep` in `lib.sh`
  for a test whose subject is time itself, and the ones still listed in
  `tools/repo_shape_sleeps.txt`, a list that only shrinks (plan step 6.5
  removes them). A guest script waits for a process with `wait`, a fifo read, a
  lock (`flock FILE true`) or a pidfd, not a loop of `sleep 0.1`.

**While working, anything goes; what is submitted obeys the rules** (russ,
2026-10-11: "timers _are_ allowed at all times while working on code.
_Submitted_ code must avoid them per our current rules, but _unsubmitted_
code can do arbitrary hacks"). A `sleep` to see whether a race is timing,
a retry loop to reproduce, a `CHECK` to find a state, a printf: fine in a
working tree, in a scratch branch, in a measurement. None of it lands:
the mechanical checks (banned symbols, repo_shape, the matchers) refuse
them in a commit, and a commit that carries one is a step not done.
"Anything" means any *code shape* in the working tree, not any *action*:
the operating rules stand at all times (russ, 2026-10-11, "I don't want
an agent doing like `rm -rf /`"): nothing destructive outside the lane,
nothing to Docker, no pushing, no working around a permission denial,
no Bazel without KVM, no edits outside the checkout, no background
polling loops, and no hack that could escape the guest or the lane (a
timer in a guest script is fine; a script that deletes host directories
is not, submitted or not).

### 1.12 No retry loops

russ, 2026-10-10: "no retry loops. I understand they may sometimes be
necessary, but if you think one is necessary, come to me."

- **An operation that failed is reported, not tried again.** A retry loop
  (an attempt counter, "try again on EAGAIN/EINTR/EBUSY/ERANGE", a wrapper
  that repeats a syscall or a transaction until it succeeds) hides the
  cause of the failure, turns a bug into a slowdown, and is the no-timers
  rule (1.11) in another shape: a retry budget is a timer counted in attempts
  instead of seconds. Prefer waiting on the event that makes a second attempt
  succeed (a lock, a `wait`, a read with no timeout), reporting the failure
  with the errno the kernel should see (`EAGAIN` once, `EINTR` once), or a
  design in which the first attempt cannot fail that way.
- **A retry loop is added only with russ's approval, case by case.** Say what
  fails, why it cannot be waited on and what bounds the loop; record the
  approval in a comment at the loop that names it ("retry loop approved,
  russ, YYYY-MM-DD: ...") and in `docs/plan/log.md`. The same holds for a
  loop proposed by a reviewer or an agent: the agent stops and reports, it
  does not add one.
- **Contention between our own threads is a mutex or unbounded optimistic
  retry, never a bounded one.** russ, 2026-10-10: "We'll either use mutexes
  or infinitely retry (e.g. optimistic locking with rollback). As a general
  pattern, optimistic locking with rollback is allowed, but it must be
  unbounded in retries and we have to be certain that another thread is
  making progress (e.g. via the TLA+ model) to unblock the retrying thread."
  So: a loop that re-reads, re-validates and tries again has no attempt
  counter and no EAGAIN fallback; it ends when it succeeds. What makes that
  safe is a progress argument: the thread whose change invalidated ours
  finishes in a bounded number of its own steps, so our next attempt sees a
  settled state. That argument is a liveness property of the model
  (`formal/`), checked by TLC with the fairness the code provides, and the
  loop's comment names it. Without such an argument, use a mutex (wait on
  the other thread's completion) instead. A bounded retry is the worst of
  both: it fails under load exactly where it was meant to help.
- **Each FUSE operation keeps the backing's contract.** russ, 2026-10-10:
  "for each fuse operation, I prefer you implement the same contract that
  the backing fs implements. If it's ERANGE-on-races with the backing store,
  then ERANGE-on-races for us. If the syscall is supposed to be atomic (like
  `rename`), then ours _must_ be atomic (even if that means an infinite retry
  loop internally)." So the question for any race is: what does the syscall
  promise its caller on a native filesystem? `listxattr`/`getxattr` promise
  nothing across a concurrent writer and answer ERANGE; dcfs answers ERANGE.
  `rename`, `unlink`, `readdir` promise atomicity (a listing is a consistent
  snapshot, a rename either happened or did not); dcfs's own bookkeeping may
  never turn that into an EAGAIN or a partial answer, so an internal conflict
  is resolved by waiting or by retrying without bound, never reported. A
  comment at each such site names the contract it keeps.
- **Not retry loops:** restarting a syscall the kernel asked to restart
  (`EINTR` when the caller has not cancelled: `umount_helper.cc`'s blocking
  lock); the kernel's own revalidation retry of a failed open; an
  application retrying a request dcfs answered `EAGAIN` (that is the client's
  choice); a test that repeats an operation because repetition is its
  subject.
- **Existing loops** are listed in `docs/plan/notes/retry-loops-2026-10-10.md`
  for russ's ruling; the list only shrinks.

## 2. Tests

- **Test first.** Write the test, run it on the unchanged code, quote the
  failure in the commit message, then fix. Never weaken a test to make it
  faster or pass (`AGENTS.md`).
- **Every test of dcfs runs in a QEMU guest as root**: `qemu_cc_test` for
  GoogleTest binaries, `qemu_test`/`qemu_test_matrix` for guest scripts
  (`test/qemu/README.md`). Host-side tests need neither root nor a kernel
  (Python, shell smoke tests of pinned tools, TLC, `//tools:format_test`).
- **GoogleTest.** `TEST_F(<Subject>Test, ...)` with a fixture (220 of 299
  tests; plain `TEST` 79); a `PascalCase` sentence for the behavior
  (`RecycledInodeNumberGetsANewRow`). No `DISABLED_` (0 uses): fix or
  delete. Check with matchers and `EXPECT_*` (the next three bullets).
  `GTEST_SKIP() << reason` when the guest filesystem lacks a capability (23
  uses).
- **Extend an existing test or write a new one.** The cost model: a guest
  boot plus setup is the expensive unit, an assertion is free, a harness
  test without a fresh mount is cheap. So write a *new* test when the
  behavior is new (an operation, a failure mode, an invariant) or the setup
  differs (fixture state, fault injection, mount options, a guest image);
  its name states the behavior. *Extend* an existing test when the gap is an
  unasserted consequence of a scenario it already runs (most mutation
  survivors are this): `ReleaseReportsWhetherTheOpenCouldWrite` checks the
  `kReleased` argument of three releases in one scenario, and a fourth
  consequence of those releases is another expectation there, not another
  test. In a guest script that is another `TEST <name>` check of the same
  scenario (`check_cold` in `write.sh` adds a `-mnt` check to a scenario
  that is already mounted), never another boot. What limits extending is
  debuggability: a failure must say what broke from the test's name plus the
  expectation's message, so an added expectation carries a matcher or a
  message that names the property, and unrelated scenarios never share a
  test.
- **`EXPECT_*`, not `ASSERT_*`**, unless continuing would use an invalid
  value or make the later checks meaningless: `ASSERT_OK_AND_ASSIGN(InodeId
  f, Id("f"))` for a value the test goes on to use, and `ASSERT_EQ(ro.error,
  0)` on the opens whose handles every later line releases. Everything else
  (`EXPECT_EQ(Release(f, ro_fh).error, 0)` and the lines after it) is
  `EXPECT_*`, so that one failure does not hide the next. (Today
  `ASSERT_THAT` is used about as often as `EXPECT_THAT` (854 and 860):
  conversion of the existing tests is 7.5/7.5b.)
- **Matchers over booleans.** `EXPECT_THAT(value, Matcher)` with gMock
  (`Contains`, `ElementsAre`, `UnorderedElementsAre`, `HasSubstr`, `Field`,
  `Property`, `Pointee`) and the status matchers `IsOk()`, `IsOkAndHolds(m)`
  and `StatusIs(code, message_matcher)`, over `EXPECT_TRUE`/`EXPECT_EQ` on a
  computed boolean or a hand-formatted string, because the failure prints
  the whole value and the expectation: `EXPECT_THAT(LinkDentry(ctx_, dir,
  "s", old.id), IsOk())` shows the status and its message,
  `EXPECT_TRUE(s.ok())` shows `false`. The status matchers exist today:
  `absl/status/status_matchers.h` (21 files include it; `IsOk()`,
  `IsOkAndHolds` and `StatusIs` are used about 900, 460 and 130 times) and
  `dcfs/testonly/assert_ok_and_assign.h`, which adds `ASSERT_OK_AND_ASSIGN`
  (the pinned Abseil has none). Three `EXPECT_TRUE(x.ok())` remain, for 7.5
  to convert. Shell guest tests do the same by hand: a FAIL line prints the
  observed and the expected value (`fail "$1-mnt" "backing reads: vdb $b_vdb
  -> $a_vdb"` in `check_cold`).
- **A debug or log string is tested for its important fields, never
  against a hard-coded whole.** One substring (or regex) per field the
  reader of the string depends on: for a file handle's `ToString`, the
  filesystem uuid, the subvolume, the handle type and the handle bytes; for
  a node's, the node id, the generation and the handle length. Neither the
  separators between them nor the order is part of the contract, so a
  reformatting that keeps every field does not break the test, and a field
  that goes missing does (`FileHandleValueTest.ToStringHasEveryField`). A
  string that is a format other code parses (a wire or database key) is a
  different thing, and its exact bytes are tested.
- **Fakes, not mocks**: no `MOCK_METHOD` (0 uses; matchers are fine). A
  dependency is injected and a small fake stands in (`ProtocolEvents`,
  `FakeChannel` in `fuse_request_channel_test.cc`). Syscall faults are
  link-time `-Wl,--wrap=<symbol>` fakes in a test target of their own, so
  the wrap affects no other test (`dcfs/BUILD.bazel:225-246`).
- **The forged-request harness** (`dir_cache_fs_test.cc`,
  `fuse_request_channel_test.cc`): a real libfuse session with
  `fuse_session_custom_io()` fed hand-built requests through
  `fuse_session_process_buf()`: a real `FuseRequest`, no kernel mount. Use
  it for request-layer behavior and the interleavings coroutines will
  produce.
- **Guest scripts** (`test/qemu/guest/*.sh`, section 4): one line per check,
  `TEST <name> PASS`, `TEST <name> FAIL (<why>)` or `TEST <name> SKIP
  (<why>)` (`lib.sh:17-19`; names lower case with hyphens: 768 vs 0
  underscores); a FAIL sets `FAILED=1`; the script ends `exit "$FAILED"`.
  There is no `DISABLED` state. Shared helpers are in `lib.sh`
  (`start_daemon`, `restart_daemon`, `drop_caches_quiesced`,
  `backing_fstype`, `sectors_read`). "Served from the cache" is checked by
  dropping every kernel cache and comparing the backing device's sectors
  read before and after (`check_cold`, `write.sh:101`; `warm-metadata-vdb`
  in `copy.sh`). A test about filesystem-dependent behavior is a
  `qemu_test_matrix` (ext4, xfs, btrfs) and branches on `backing_fstype`
  only for a genuine per-filesystem difference.
- **Tiers**: Bazel `size`. `small` (about a minute in all,
  `--config=fast`), `medium` (`--config=presubmit`), `large`/`enormous`
  (CI). Every test declares `size` and `timeout` (the QEMU macros refuse a
  missing one); pick the tier from the measured duration. The soak test is
  `manual`.
- New code arrives covered, error paths included (from phase 7/8).

## 3. Bazel

- Bazel's BUILD style guide, as buildifier enforces it (1.1).
- Targets are `snake_case` after the file stem (`metadata_cache`,
  `metadata_cache_test`); a matrix test generates `<name>_ext4`, `_xfs`,
  `_btrfs`. Load rules explicitly (`dcfs/BUILD.bazel:1-3`). `testonly = 1`
  on test-only libraries.
- Comment a target when its name and attributes do not say why it exists
  (about 64 of 149 targets have a comment; 25 name a plan step). Always
  comment a non-obvious flag (`copts = ["-O2"]`, `dcfs/BUILD.bazel:119-121`),
  a `-Wl,--wrap`, and a list shared by several targets (`MAIN_DEPS`). An
  obvious `cc_library` needs none; a plan-step reference helps.
- Dependency lists: one per line, sorted (`:local`, `//`, `@`), elements
  indented 4 spaces (buildifier, enforced by `//tools:format_test`).
- `third_party/<name>/` holds only what we write for a pin (BUILD overlay,
  patches, config, lock files) and a `README.md` with a "Pin" section
  (version, URL, sha256, how the sha256 was obtained, date) and an
  "Updating the pin" procedure (`third_party/tlaplus/README.md`).
  Third-party code is fetched by Bazel: the Bazel Central Registry
  (`bazel_dep`), else `http_archive`/`http_file` with a sha256.
  The exception is Alpine's packages (Phase 24), which have no sha256 and no
  Pin section: they are pinned to a stable release branch and verified by
  Alpine's signatures, and `third_party/alpine/README.md` says how.
- `MODULE.bazel`: a comment above a pin says what it is for and where its
  README is (7 of 13 `http_archive`s, 4 of 14 `bazel_dep`s; the others are
  self-evident). `MODULE.bazel.lock` is never rewritten silently
  (`--lockfile_mode=error`). `.bazelrc` flags carry the reason.

## 4. Shell

Google's shell guide applies: bash, 2-space indent (no tabs), 80 columns,
`[[ ]]`, `local`, `lower_snake` functions, `UPPER_SNAKE` constants, a file
header comment, `$(...)`, quoted expansions, and a `main` function in a
script that defines any other function. shfmt enforces the layout:
`shfmt -i 2 -ci -bn` (2 spaces, indented `case` alternatives, a pipe or
`&&` that wraps starts the next line, as the guide's pipeline and `case`
sections show; check the flags against the guide's examples when 7.6 pins
shfmt), run by `//tools:format_test`; shellcheck is pinned in the same step
(7.6). Our additions and narrowings:
- **Host-side scripts are bash**: `#!/bin/bash`, then `set -euo pipefail`
  (our narrowing; the guide only says to use `set` flags sparingly). 8 do
  today (`.github/ci/*`, `formal/trace_validate.sh`,
  `third_party/alpine/tools_test.sh`, `third_party/alpine/busybox_test.sh`).
- **One documented deviation: scripts that run inside the busybox guest**
  (`test/qemu/guest/*`, `guest/init`) are POSIX `sh` (`#!/bin/sh`: no
  `[[`, arrays, `local` or `function`; `[ ]`, `$(...)`), because the guest
  has no bash. They still follow the guide's layout, naming and quoting.
  Revisit when the Alpine work (phase 24) can put bash in the guest.
- **Guest test scripts do not use `set -e`**: a failed check is a `TEST ...
  FAIL` line and the script goes on. They set `FAILED=0`, source `lib.sh`,
  `trap cleanup EXIT` (dumping the daemon log when something failed) and
  `exit "$FAILED"`. The line protocol is in section 2.
- **Length.** The guide says a script over 100 lines should be rewritten in a
  structured language. 37 scripts are over 100 lines (27 guest, 10 host;
  the guest scripts total 8,418 lines). russ accepted shell for guest tests
  (2026-10-07): they stay, with shared helpers in `lib.sh` instead of
  copies (C15). New host-side tooling over 100 lines is Python (section 5).
- A comment right after the shebang says what the script does and how it
  runs (63 of 63 scripts). Check names are lower case with hyphens. The zsh
  caveats belong to `CLAUDE.md`, not scripts.

## 5. Python

- Google Python Style Guide, 80 columns; a formatter pinned through Bazel
  joins `//tools:format_test` in phase 7. Python is host-side tooling and
  tests, 13 files: `tools/sbom/` (2), `tools/man/` (1), `man/` (2),
  `third_party/alpine/` (2: `apk.py`, `signature_test.py`),
  `third_party/debian/scripts/` (3: `mkrootfs.py` and its two tests),
  `third_party/linux/kernel_config_test.py` and `test/qemu/scripts/`
  (`mkmodules.py` and its test). Python 3.12 from the hermetic
  `rules_python` toolchain, standard library only.
- A module docstring says what the file does and its plan step
  (`tools/sbom/sbom.py:1`); `argparse` for a command line; `main(argv)`
  returns the exit code, called as `sys.exit(main(sys.argv[1:]))`.
- Tests are `unittest` classes in `<name>_test.py`, built as `py_test` with
  an explicit `size`, data passed as `$(location ...)` arguments
  (`tools/sbom/BUILD.bazel`). They run on the host.

## 6. TLA+

`formal/README.md` ("Changing the model", "Trace validation") has the
conventions; follow it. In short: a protocol change edits the step in
`formal/dcfs.tla` that stands for the code and keeps its comment naming the
dcfs function; a fixed bug the model can express gets a `known_bugs/`
variant whose test expects the counterexample; a changed step gets its
protocol event (`dcfs/protocol_events.h`) and `Trace.tla` action;
`bazel test //formal/...` passes. Comments are boxed `(* ... *)` blocks
(`formal/dcfs.tla:1-20`).

## 7. Docs and commits

- **Where things go.** Decisions and reasons: `docs/design.md`. User-facing
  behavior and limitations: `README.md` (also the source of the `dcfs(8)`
  man page). Test usage: `test/qemu/README.md`. A pin: its
  `third_party/<name>/README.md`. Plan, log, status: `docs/plan/` (the
  orchestrator edits it; an agent reports what it needs).
- **Prose.** Full sentences, plain and specific, no marketing; say what the
  program does and does not do. Measured numbers carry units, date and
  environment ("about 1 minute (KVM)", "released 2026-05-13; the latest
  stable release as of 2026-10-05"). A decision names its maker and date
  ("(russ, 2026-10-06)"). Headings are sentence case; a list item stating
  a rule may open with it in bold (`docs/design.md`, "Architecture and
  layering").
- **Markdown follows Google's docguide**: ATX headings (`#`), one H1 per
  file, `-` bullets, 80-column wrap (tables, links and code excepted), no
  trailing whitespace, and fenced code blocks with the language declared
  (`bash`, `text`, `c++`, `python`, `starlark`; `text` for output). The tree
  already uses ATX only (0 setext), `-` only (0 `*`), no trailing
  whitespace and no tabs in 24 docs outside `docs/plan/`; D1, D2 are the
  rest.
- **Commit subject**: `N.M: area: what changed` for a plan step (`23.4:
  copy_file_range, the ioctl allowlist and O_TMPFILE`); `plan:`, `notes:`
  and `agents:` for orchestrator edits, notes and agent instructions. No
  trailing period; about 72 columns (62 of 323 are longer).
- **Commit body**: wrapped at 72, says why. A bug fix or behavior change
  quotes the failing-first run under a "Failing first" heading (about 30
  bodies), then the model and test results. It ends with the model's
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` trailer
  (history: Opus 5.5 x103, Sonnet 5 x90, Fable 5.1 x82, Sonnet 5.5 x34,
  Haiku 4.5 x5). Never push.

## 8. No test-only things in production code

russ, 2026-10-09: "don't add test-only things to prod code. If a test needs
a knob in the prod code, make that knob a real feature of the class /
method. There should be zero `// only use in tests` style comments." This is
stronger than `AGENTS.md`'s "test-only code never lives in production
files": a seam a test needs (an injected clock, an observer, a fault point,
a size limit) is designed, named and documented as a feature of the class,
with its production default, and nothing in production code says it exists
for tests.

- **The pattern.** Constructor injection of an interface that has a
  production implementation (`Context::clock`, `Context::interrupts`,
  `SessionLoop`'s callbacks); options with production defaults
  (`FillGuards::max_touched`); observers such as `ProtocolEvents`, whose
  default is a no-op that the daemon runs with and that the trace tests and
  the formal-verification builds replace. russ: "Some fakes are meant to be
  used in production (such as the no-op trace logger that we use in tests
  for formal verification)." A no-op default that production runs is the
  pattern, not a violation. Fault injection is link-time (`-Wl,--wrap`) and
  needs no seam at all.
- **The anti-pattern.** A method, flag, branch, accessor, friend or comment
  whose only reason is a test: `ErrnoNameTable()` (only tests read it; step
  25.7 removed it), a `...ForTest` or `..._for_test` name, "only in tests",
  "so that a test can". Describe a seam by what it does in production
  ("the bound trades memory against how often a prune happens"), never by
  who calls it.
- **Enforced** by `tools/repo_shape.py` (`no_test_only_comments`, run by
  `//tools:repo_shape_test` over `dcfs/` and `bench/` minus `*_test.cc` and
  `testonly/`): a comment that gives tests as the reason for code
  (`only in/for/by tests`, `for (the) tests`, `test-only`, `for testing`,
  `so (that) a test can`, `a test that`) or a `ForTest`/`_for_test` name
  fails the build. Sentences about the suite itself ("the test suite", "the
  testonly builds") pass. An allowlist entry (`TEST_REASON_ALLOWLIST`) needs
  a reason; there are none. The check cannot see a seam with a neutral
  name: the reviewer asks of every new knob "what does this do in
  production?".
- **No test peers** (russ, 2026-10-09: "A test peer allows test code to
  reach into the private internals of a class. Don't do that."): no
  production class declares a `friend` of anything in a `testonly`
  namespace, and a test never reaches a private member. What a check or a
  test reads is a feature of the class: `DirCacheFS::bookkeeping()` hands
  the invariant checks' hooks an `events::Bookkeeping` view
  (`protocol_events.h`), and a test that needs a state the class never
  reaches edits a copy (`testonly::FakeBookkeeping`,
  `InvariantChecker::TamperBookkeeping`). `tools/repo_shape.py`
  (`no_testonly_friends`) fails the build on such a `friend`.

## Appendix A: Convergence

Sites that break Google style or a rule above, as of e680508 plus step 25.1. Run each
command from the repository root in bash. Rows marked (new) come from the
2026-10-07 decisions. Google rules surveyed: formatting, includes,
`using namespace`, `typedef`, `thread_local`, exceptions, casts, naming,
header guards, `explicit` constructors, macros; for shell, indent, line
length, shebang, `main`, `[[`, backticks; for Markdown, headings, bullets,
fences, line length, whitespace. Other rules (the Google Python rules beyond
line length, shellcheck findings, quoting) were not surveyed.

| # | Rule | Count | Find them |
|---|---|---|---|
| C3 | Google: 80 columns | 92 lines in 28 files (`backing.cc` 12, `metadata_cache_test.cc` 9, `dir_cache_fs.cc` 7), as of e680508 plus step 25.1 | `grep -rnE '^.{81,}$' dcfs bench tools --include='*.cc' --include='*.h' --include='*.c'` |
| C4 | Google/clang-format include blocks and order | 16 out-of-order lines in 10 files (`backing.cc`, `errno.cc`, `fd.cc`, `main.cc`, `syscalls.cc`, `syscalls.h`, 4 tests); 37 files have one `<...>` block where Google has C and C++ headers apart (26 files have two or more); clang-format settles both | `LC_ALL=C awk 'FNR==1{p=""} /^#include/{if(p!=""&&$0<p)print FILENAME":"FNR": "$0;p=$0;next}{p=""}' $(git ls-files 'dcfs/*.cc' 'dcfs/*.h')` |
| C11 | Google shell: 2-space indent, no tabs (4) (7.6) | 48 scripts use tabs (all guest scripts but a few wrappers, most host scripts); 4 use spaces and conform (`formal/trace_validate.sh`, `tools/smoke_readonly.sh`, `tools/format.sh`, `.github/ci/osv.sh`), as do the new `third_party/alpine/*.sh` | `grep -lP '^\t' $(git ls-files '*.sh' test/qemu/guest/init)` |
| SH1 | Host-side scripts are bash (4) (7.6) | 19 of 26 host-side scripts are `#!/bin/sh` (`third_party/*` build and smoke helpers, `test/qemu/scripts/`, `tools/`); each becomes `#!/bin/bash` with `set -euo pipefail` (all 19 already have `set -eu`) | `grep -L '^#!/bin/bash' $(git ls-files '*.sh' \| grep -v test/qemu/guest/)` |
| SH2 | Google shell: 80 columns (4) (7.6) | 246 lines over 80 in 48 scripts (measured with a tab as 2 columns; 375 lines in 54 scripts with a tab as 8) | `grep -nE '^.{81,}$' $(git ls-files '*.sh')` after expanding tabs (`expand -t2`) |
| SH3 | A script with functions has `main` (4) (7.6) | 40 scripts define functions, 0 define `main` | `grep -L '^main()' $(grep -lE '^[a-z_]+\(\) \{' $(git ls-files '*.sh'))` |
| SH4 | Bash scripts use `[[ ]]`, not `[ ]` (4) (7.6) | 9 `[ ... ]` tests in 2 bash scripts: `.github/ci/prepare.sh` 7, `.github/ci/test.sh` 2 | `grep -nE '(^\|[^[])\[ ' .github/ci/*.sh` |
| SH5 | shfmt formatting and shellcheck clean (4) (7.6) | not pinned, not run; the number of findings is unknown (no backtick command substitution: 0 of 63 scripts) | after pinning: `bazel test //tools:format_test` |
| D1 | Markdown: fenced blocks declare a language (7) | 37 bare fences in 12 files (`README.md` 11, `test/qemu/README.md` 8, `tools/sbom/README.md` 3, `third_party/*` 12, `docs/design.md` 2, others) | `grep -rnE '^[`]{3}$' $(git ls-files '*.md' \| grep -v docs/plan/)` (opening and closing fences both match: halve) |
| D2 | Markdown: 80-column wrap (7) | 21 prose lines over 80 in 9 files outside `docs/plan/` (`formal/README.md` 11, `test/qemu/README.md` 2, 7 `third_party` READMEs/`tools/sbom/README.md` 1 to 2 each); tables, headings, links exempt | `grep -nE '^.{81,}$' $(git ls-files '*.md' \| grep -v -e docs/plan/ -e .claude/) \| grep -v -e '\|' -e http -e '^[^:]*:[0-9]*:#'` |
| C13 | BUILD list elements indented 4 (buildifier) | 292 lines at 6 spaces, all in `dcfs/BUILD.bazel` | `grep -cP '^      \S' dcfs/BUILD.bazel` |
| C15 | Guest helpers shared in `lib.sh` | duplicated: `cleanup` 25, `normalize_stat` 5, `populate_tree`/`run_pass`/`expect_fail` 4 each, `start_daemon` 3, six more 2 each | `grep -hE '^[a-z_]+\(\) \{' test/qemu/guest/*.sh \| sort \| uniq -c \| sort -rn` |
| P1 | Google Python: 80 columns (5) | 47 lines over 80: `sbom.py` 24, `sbom_test.py` 23 | `grep -nE '^.{81,}$' $(git ls-files '*.py')` |
| N1 | Flat `dcfs`: remove `dcfs::cache` (1.3) (new) | 3 declarations (`metadata_cache.h/.cc/_test.cc`); 673 `cache::` uses (407 production) in 20 files. Clash if flattened: `ParentOf` (same parameters as `backing::ParentOf`, differing only in return type), `SetXattr`, `RemoveXattr` all also exist in `backing` (3 names: rename one side first) | `grep -rn 'namespace cache\|cache::' dcfs bench \| wc -l` |
| N2 | Remove `dcfs::backing`: it folds into `dcfs` as free functions; the three clashing pairs get distinguishing names (e.g. `BackingSetXattr`); a wrapper class only if the clashes prove to be more than those three and renaming reads worse | 3 declarations; 147 uses (138 production) in 15 files; clashes: `ParentOf`, `SetXattr`, `RemoveXattr` (same 3 names as N1) | `grep -rn 'namespace backing\|backing::' dcfs bench \| wc -l` |
| N3 | Remove `dcfs::testonly` | 8 declarations (all in `dcfs/testonly/`); 4 uses | `grep -rn 'namespace testonly\|testonly::' dcfs bench` |
| N4 | `sqlite3::` never `dcfs::sqlite3::` or `using` (1.3) | `dcfs::sqlite3` stays (exception): 3 declarations (`sqlite.h/.cc`, `sqlite_test.cc`), 104 `sqlite3::` uses (62 production) in 13 files are fine; 1 `using sqlite3::Statement;` to drop (`metadata_cache.cc:37`) | `grep -rn 'dcfs::sqlite3::\|using .*sqlite3' dcfs bench` |
| N5 | Remove `dcfs::events` | 2 declarations (`protocol_events.h`); 182 uses (164 production) in 7 files; generic names (`Request`, `Op`, `Ino`) become dcfs-wide; no clash found | `grep -rn 'namespace events\|events::' dcfs bench \| wc -l` |
| N6 | Remove `dcfs::internal` | 2 declarations (`ret_check.h`, `sqlite.h`); 9 uses; helpers would need distinct names (`RetCheck*`, `IsOptional`) | `grep -rn 'namespace internal\|internal::' dcfs` |
| N7 | `syscalls::` never `dcfs::syscalls::` or `using` | 0 code sites (1 in a comment: `backing.h:33`); the 188 `syscalls::` uses (98 production) are fine | `grep -rn 'dcfs::syscalls::\|using .*syscalls' dcfs bench` |
| F1 | `//tools:format_test` (small) in check mode (1.1) (new) | does not exist | `bazel query //tools:format_test` |
| F2 | Pinned clang-format, buildifier, shfmt, shellcheck | none in `MODULE.bazel`, none on the host (phase 7 LLVM toolchain; shfmt and shellcheck in 7.6) | `grep -n 'clang\|buildifier\|shfmt\|shellcheck' MODULE.bazel` |
| F3 | `bazel run //tools:format` | `tools/format.sh` is a host script that skips missing tools | `cat tools/format.sh` |
| F4 | `.githooks/pre-commit` (opt-in) | does not exist | `ls .githooks` |
| F5 | `tools/*.c` reformatted | 2,537 tab-indented lines (`fhtest.c`, `testutil.c`) | `grep -lP '\t' tools/*.c` |
| F6 | `layering_check` and `misc-include-cleaner` | no `.clang-tidy`, no `layering_check` in `.bazelrc` (phase 7) | `ls .clang-tidy; grep -n layering .bazelrc` |
