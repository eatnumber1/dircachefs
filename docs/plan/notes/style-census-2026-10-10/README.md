# Style census, function by function (plan step 25.22)

Date: 2026-10-10. Tree: `5fed48e` (`origin/main` when the census ran; step
branch `step-25.22`, no code changed).

Scope: production only. `dcfs/*.cc`, `dcfs/*.h`, `tools/*.cc`, `bench/*.cc`
minus `*_test.cc` and `dcfs/testonly/`. Not scanned: `tools/style_matchers/`
and `tools/mutation/` (fixtures that are wrong on purpose), and the test
pass (see "Second pass" below). `tools/banned_symbols_fixture.cc` and
`tools/repro_date_fixture.cc` are included because they match `tools/*.cc`.

## Method

- Enumeration: a line scan of each file, not clang-query. A definition is a
  line at indent 0 or 2 that opens a signature (joined over up to 8 lines)
  and ends in `{`, with the body closed by a `}` at the same indent. Lambdas,
  control statements and declarations without a body are excluded. A
  cross-check (every line at indent 0 to 2 that ends in `) {`) found eight
  friend functions and operators the first version missed; the scan was
  fixed for them, and the cross-check was not rerun afterwards. The
  clang-query route (`tools/style_checks.bzl` flags) was not tried; the
  line scan was used from the start.
- Body length: the lines between the braces. A function over 60 lines gets
  only `break-up`; its other items are dropped.
- Items: each card item is detected by a script that quotes the line it
  found. The script does not read each function, so the card's judgement
  items are not answered by a reader (see "What the card could not express").
  Where a pattern is ambiguous it is left out, not guessed.
- Each `.jsonl` line is `{file, function, line, quote, note}`; quotes are
  under 120 characters.

## Function count per file

713 definitions, 32 of them over 60 lines (break-up).

dcfs/backing.cc 101, dcfs/dir_cache_fs.cc 98, dcfs/metadata_cache.cc 95,
dcfs/fuse_ops.cc 43, dcfs/syscalls_backing.cc 40, dcfs/sqlite.cc 40,
dcfs/fuse_request.cc 24, dcfs/mount_dcfs.cc 22, dcfs/migrate.cc 20,
dcfs/syscalls.cc 27, dcfs/fsck.cc 12, dcfs/main.cc 11, dcfs/sqlite.h 11,
dcfs/mounts_below.cc 10, dcfs/backing_capture.cc 9, dcfs/file_handle.cc 9,
dcfs/status.h 9, dcfs/device_id.cc 7, dcfs/startup_channel.cc 7,
dcfs/status.cc 7, dcfs/umount_helper.cc 7, dcfs/protocol_events.h 6,
dcfs/session_loop.cc 6, dcfs/dir_cache_fs.h 5, dcfs/escape.cc 5, dcfs/fd.cc
5, dcfs/ret_check.h 5, dcfs/syscalls_process.cc 5, dcfs/device_id.h 3,
dcfs/errno.cc 3, dcfs/mount_fds.cc 3, dcfs/mount_options.cc 3,
dcfs/syslog_sink.cc 4, dcfs/context.h 2, dcfs/remount.cc 2,
dcfs/absolute_paths.cc 1, dcfs/checkpoint.cc 1, dcfs/file_handle.h 1,
dcfs/fork_split.h 1, dcfs/interrupts.h 1, dcfs/log_open_flags.h 1,
dcfs/session_loop.h 1, dcfs/syscalls_backing.h 1, bench/dcfs_bench.cc 19,
bench/process.cc 8, bench/dm_delay.cc 5, bench/tree.cc 5,
tools/banned_symbols_fixture.cc 1, tools/repro_date_fixture.cc 1 (49 files).

## Counts per rule (production)

| rule | items | functions with an item |
|---|---:|---:|
| break-up | 32 | 32 |
| confession | 1 | 1 |
| comment-restates-code | 0 | 0 |
| repeated-handler | 0 | 0 |
| hand-rolled-utility | 63 | 30 |
| name-repeats-scope | 2 | 2 |
| nested-happy-path | 6 | 6 |
| status-carried (advisory) | 8 | 8 |

A function over 60 lines has no other items (break-up only), so the
break-up functions are not counted in the rows above them.

## Ten functions with the most findings (excluding break-up)

1. `bench/dcfs_bench.cc` `BM_Recovery`: 6 (hand-rolled: `+` chains, `std::to_string`)
2. `bench/dm_delay.cc` `CreateDelayDevice`: 6 (`+` chains, `snprintf`, `std::vector<char>` buffer)
3. `dcfs/escape.cc` `AppendEscapedBytes`: 6 (`+=` of literals)
4. `bench/process.cc` `Start`: 4
5. `bench/dcfs_bench.cc` `ParseOwnFlags`: 4 (`strtoull`)
6. `bench/dcfs_bench.cc` `main`: 4 (`strtoull`, `atoi`, `printf`)
7. `bench/dcfs_bench.cc` `StartDcfs`: 3
8. `bench/process.cc` `IsMounted`: 3 (`find != npos`, `+`)
9. `bench/process.cc` `RssBytes`: 3 (`strtoull`, `std::to_string`, `+`)
10. `bench/tree.cc` `WriteFile`: 3 (`fprintf(stderr)`)

Nine of the top ten are in `bench/`; the other is `dcfs/escape.cc`
`AppendEscapedBytes`.

## Skipped (already covered by a mechanical check)

- clang-tidy and clang-query findings (`tools/style_checks_allow.txt`): the
  iterator-pair `std::` algorithms, `std::function`, `std::unordered_*`,
  `std::map`/`std::set` (banned_std covers the banned three),
  `CHECK`/`LOG(FATAL)` in production, `else-after-return`, the nested
  `if (x.ok())` with three or more statements (`happy_path_nested`, which
  this census reports in its wider form), `capturing_mutating_lambda`.
- repo_shape: sleeps, fixed arrays in `dcfs/` (`std::vector<T> name(n)` in
  `dcfs/` is allowlisted; the census keeps the `bench/` and `dcfs/` sites
  that the allowlist does not name), project prefix (`Dcfs[A-Z]`),
  `[[nodiscard]]` before Status, `ABSL_` status macros.
- Not counted because the card cannot answer them without judgement: the
  `std::map`/`std::set` where ordering is unused, a hand-written
  `find`/`substr` split (dcfs/backing.cc `SplitXattrList` and
  dcfs/mount_dcfs.cc split), and `absl::StatusOr` with an initializer in
  an `if` (`dcfs/status.cc` `StatusToErrno`, which is `if (... ; eno.ok())`).

## What the card could not express

- `comment-restates-code` needs a reader's judgement of "restates": the
  lexical rule (every content word of the comment appears in the statement
  below it) found none. A card example such as `// increment the counter`
  above `++n` is not caught.
- `repeated-handler` found none: no two identical `if (!x.ok())` handlers of
  two or more statements in one function.
- `name-repeats-scope` on a class method (a name beginning with its class's
  name) was not run; only the namespace and layer words were.
- `status-carried`: a declaration without an initializer (`absl::StatusOr<...>
  value;`) is not a carried status; these were dropped (one site,
  `dcfs/dir_cache_fs.cc` `Getxattr`).
- `hand-rolled-utility` `std::vector<T> name(n)` in `bench/` and `tools/`
  is reported here because repo_shape covers only `dcfs/`.

## Second pass (tests)

Not started. The card's first pass ran as scripts over production
functions only, so the test pass (`*_test.cc`, `dcfs/testonly/`) has not
been enumerated.

## Reproduce

Enumeration and detection were scripts in the agent's scratch directory,
not committed (the card asks for files under this directory only). The
rules above are the scripts' rules.
