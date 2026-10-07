# Coverage baseline, 2026-10-08 (first report, step 7.2)

## Executive summary (orchestrator's review)

Numbers: `dcfs/*.cc` **92.4% lines, 73.3% branches** over the small and
medium tiers (large/enormous and the trace tests are not in it); bench/
84.9%, tools/ (the two guest C helpers) 76.2%. 30 of 723 dcfs functions
have zero hits. Branches, not lines, are the gap: `fuse_ops.cc` 31%,
`mounts_below.cc` 50%, `file_handle.cc` 57%, `backing.cc` 69%.

What the zero-hit functions say (the list at the bottom):
- **The non-passthrough data path is untested**: `DirCacheFS::Read`,
  `backing::ReadFile` and `fuse_ops` Read have 0 hits because every test
  kernel has FUSE passthrough, so reads never reach dcfs. That path runs
  on any kernel without passthrough. Needs russ (Q1).
- **Dead code under our configuration**: the FUSE `Access` handler (with
  `default_permissions` the kernel answers access(2) itself),
  `FuseRequest::operator=(&&)`, `syscalls::openat2` (no caller),
  `FileHandle::ToString` (no caller). Remove, or justify and test.
- **Real test gaps, cheap to close**: `FutimensOPath` (utimensat on a
  file dcfs has no open fd for: `touch -d` on a closed file),
  `RemoveXattrOPath`/`RemoveXattrOps` (removexattr is never tested),
  `status.cc`'s uncovered 24 lines (errno-name table edges),
  `device_id.cc` parse failures, `mounts_below.cc`'s mountinfo parsing
  edge cases, `file_handle.cc` decode errors (12.5 adds forged handles),
  `main.cc`'s startup failure modes (bad flags, cache-dir errors: a
  guest test that runs dcfs wrongly).
- **Error branches after syscalls and SQLite steps** (`backing.cc`,
  `sqlite.cc`, `fuse_ops.cc`'s error replies): hand-written tests would
  be hundreds; the right instruments are 26.6 (every backing call site
  failed once, with the invariant checks) and Phase 11's I/O-error
  injection on the cache disk. Recommendation: do not hand-write these.

Proposed Phase 8 plan from this: (a) decide Q1-Q3; (b) delete the dead
code; (c) one mechanical step for the cheap gaps above (unit tests +
one guest "misuse" test); (d) 26.6 and Phase 11 for the error branches;
(e) then the gate: no drop below the baseline for dcfs/ lines AND
branches, bench/ and tools/ reported only.

Questions that need russ:
- **Q1** Non-passthrough reads/writes: support kernels without FUSE
  passthrough (then test it: one e2e variant with passthrough disabled
  by a flag), or declare passthrough required and delete the fallback?
  Recommendation: keep and test; distro kernels before 6.9 exist.
- **Q2** Remove the dead `Access` handler (and the other three)? 
  Recommendation: yes, unless `default_permissions` may ever be dropped.
- **Q3** Phase 8 gate scope: `dcfs/` only, both lines and branches, no
  drop below this baseline (92.4 / 73.3), bench/ and tools/ reported
  but not gated. Confirm.

## The agent's table

- commit: 8548ad6 (step-26.12 tip, 7.2 included)
- date: 2026-10-07
- tiers: small + medium (`bazel coverage --config=presubmit -k //...`); large/enormous and trace-validation tests are not in it
- scope: our code with at least one measured line (dcfs, bench, tools); tests and `testonly/` excluded
- continuous-mode profiles; filtered report: 40 files with measured lines

### dcfs/*.cc (sorted by line %, ascending)

| file | lines hit/total | line % | branches hit/total | branch % |
|---|---|---|---|---|
| dcfs/status.cc | 37/61 | 60.7 | 31/44 | 70.5 |
| dcfs/syscalls_process.cc | 25/31 | 80.6 | 6/8 | 75.0 |
| dcfs/sqlite.cc | 282/342 | 82.5 | 158/208 | 76.0 |
| dcfs/device_id.cc | 106/128 | 82.8 | 51/70 | 72.9 |
| dcfs/main.cc | 272/321 | 84.7 | 139/198 | 70.2 |
| dcfs/file_handle.cc | 88/103 | 85.4 | 24/42 | 57.1 |
| dcfs/syscalls_backing.cc | 268/308 | 87.0 | 62/86 | 72.1 |
| dcfs/mounts_below.cc | 46/52 | 88.5 | 22/44 | 50.0 |
| dcfs/dir_cache_fs.cc | 1471/1609 | 91.4 | 867/1154 | 75.1 |
| dcfs/backing.cc | 1195/1306 | 91.5 | 550/802 | 68.6 |
| dcfs/fuse_request.cc | 184/201 | 91.5 | 98/110 | 89.1 |
| dcfs/syscalls.cc | 86/92 | 93.5 | 19/24 | 79.2 |
| dcfs/fd.cc | 29/31 | 93.5 | 5/8 | 62.5 |
| dcfs/fuse_ops.cc | 297/310 | 95.8 | 30/98 | 30.6 |
| dcfs/migrate.cc | 277/281 | 98.6 | 155/206 | 75.2 |
| dcfs/metadata_cache.cc | 1257/1261 | 99.7 | 530/666 | 79.6 |
| dcfs/errno.cc | 264/264 | 100.0 | 10/12 | 83.3 |
| dcfs/escape.cc | 87/87 | 100.0 | 58/60 | 96.7 |
| dcfs/mount_fds.cc | 22/22 | 100.0 | 6/6 | 100.0 |
| dcfs/protocol_events_main.cc | 1/1 | 100.0 | 0/0 | 100.0 |
| **total** | 6294/6811 | 92.4 | 2821/3846 | 73.3 |

### bench/

| file | lines hit/total | line % | branches hit/total | branch % |
|---|---|---|---|---|
| bench/dm_delay.cc | 60/91 | 65.9 | 11/34 | 32.4 |
| bench/tree.cc | 59/75 | 78.7 | 27/44 | 61.4 |
| bench/process.cc | 87/97 | 89.7 | 27/44 | 61.4 |
| bench/dcfs_bench.cc | 301/335 | 89.9 | 110/158 | 69.6 |
| bench/process.h | 2/2 | 100.0 | 0/0 | 100.0 |
| bench/tree.h | 1/1 | 100.0 | 0/0 | 100.0 |
| **total** | 510/601 | 84.9 | 175/280 | 62.5 |

### tools/ (fhtest.c and testutil.c are guest test helpers)

| file | lines hit/total | line % | branches hit/total | branch % |
|---|---|---|---|---|
| tools/fhtest.c | 106/152 | 69.7 | 49/76 | 64.5 |
| tools/testutil.c | 1346/1754 | 76.7 | 598/830 | 72.0 |
| **total** | 1452/1906 | 76.2 | 647/906 | 71.4 |

### Ten least-covered functions in dcfs/*.cc

The functions never called (0 hits) number 30 of 723; the ten shown are the ones with 0 hits that have the most lines (a function's line span is estimated from the next function in the file).

| function | file:line | hits | approx. lines |
|---|---|---|---|
| `backing.cc:dcfs::backing::(anonymous namespace)::FutimensOPath(int, timespec const*)` | dcfs/backing.cc:207 | 0 | 52 |
| `dcfs::DirCacheFS::Read(dcfs::FuseRequest&, unsigned long, unsigned long, long, fuse_file_info&)` | dcfs/dir_cache_fs.cc:1550 | 0 | 16 |
| `dcfs::backing::ReadFile(int, unsigned long, long)` | dcfs/backing.cc:864 | 0 | 15 |
| `backing.cc:dcfs::backing::(anonymous namespace)::RemoveXattrOPath(int, std::__1::basic_string_view<char, std::` | dcfs/backing.cc:185 | 0 | 13 |
| `dcfs::syscalls::openat2(int, std::__1::basic_string_view<char, std::__1::char_traits<char>>, open_how, unsigne` | dcfs/syscalls_backing.cc:68 | 0 | 10 |
| `dcfs::FileHandle::ToString() const` | dcfs/file_handle.cc:106 | 0 | 9 |
| `backing.cc:dcfs::backing::(anonymous namespace)::RemoveXattrOps(dcfs::Credentials const&, std::__1::basic_stri` | dcfs/backing.cc:1070 | 0 | 9 |
| `fuse_ops.cc:dcfs::(anonymous namespace)::Access(fuse_req*, unsigned long, int)::$_0::operator()(dcfs::DirCache` | dcfs/fuse_ops.cc:306 | 0 | 7 |
| `fuse_ops.cc:dcfs::(anonymous namespace)::Read(fuse_req*, unsigned long, unsigned long, long, fuse_file_info*):` | dcfs/fuse_ops.cc:186 | 0 | 7 |
| `dcfs::FuseRequest::operator=(dcfs::FuseRequest&&)` | dcfs/fuse_request.cc:60 | 0 | 6 |
