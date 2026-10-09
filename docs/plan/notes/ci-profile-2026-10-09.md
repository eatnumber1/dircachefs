# Where CI time goes: profiles and the first local numbers (step 26.16, 2026-10-09)

What 26.16 added: every CI job that runs Bazel writes `--profile`, uploads it
as `bazel-profile-<job>[-<shard>]`, and puts `tools/ci_profile.py`'s table in
its job summary (README.md, "Continuous integration"). The first CI numbers
arrive with the next push; a `cold` dispatch gives the full picture. This note
records the only numbers available before that, measured in lane 7.

## Host load (read before the numbers)

A 4-core machine shared with the other lanes: load average 31 (1 min: 30.9,
5 min: 32.9, 15 min: 33.3 at 10:26) when the fast tier below started, 16 to 18
when it and the `@dcfs_llvm` fetch finished (11:13 to 11:15). Every wall time
here is inflated by that load, the extraction most (it is CPU and disk bound).

## The fast tier, warm disk cache (`.github/ci/test.sh --config=fast`)

The lane's disk cache was warm for compiles and links, but this checkout's
output base had never fetched the Alpine, Debian and gawk repositories, so
fetches and the third-party actions that consume them ran for real.
`@dcfs_llvm` was extracted beforehand (by `bazel fetch`, see below), so it is
absent here. 8 min 7 s by the clock; 17 of 231 tests ran, the rest were
cached results.

| What | Wall (s) | Busy (s) | n |
|---|---:|---:|---:|
| Total wall | 486.0 | | |
| Repository fetches (not @dcfs_llvm) | 82.1 | 253.7 | 1069 |
| @dcfs_llvm fetch and extraction | 0.0 | 0.0 | 0 |
| Third-party builds | 147.5 | 208.4 | 110 |
| Our compile and link | 75.9 | 91.2 | 174 |
| Other actions | 14.9 | 21.8 | 503 |
| Test execution | 152.6 | 228.5 | 31 |
| Critical path (Bazel's) | 137.5 | 137.5 | 6 |

Critical path: the test `//dcfs:dir_cache_fs_test` (120.7 s), then compiling
`absl/log/globals.cc` (15.6 s, a cold action), then a 0.7 s link. Test
execution is the long pole of a warm fast tier; the 1069 fetch events are
mostly short events of many repositories, not 1069 repositories (the profile
records a repository function again when it restarts).

## `@dcfs_llvm` extraction

`bazel fetch --force --repo=@dcfs_llvm --profile=...` (the repository
re-extracted from the archive in the repository cache, no network) took
1894.7 s of wall time, 31.6 minutes, at load 18 to 37; the tree it writes is
2.1 GB. The profile's event is named `@@+llvm_distribution+dcfs_llvm`. The first
fetch in the lane (an empty output base) ran from 09:37 to about 10:31, under
load 31 to 37 (the start and end are clock times, not a timed run). On a GitHub runner (4 cores, SSD, no neighbours) it will be
shorter, but it is the largest single item of a cold job: against it the
whole fast tier above is 8 minutes.

## Is the extracted `@dcfs_llvm` among the restored cache paths? No.

`ci.yml` restores and saves three paths: `~/.cache/bazel-repo-cache` (the
repository cache: downloaded archives, key `bazel-repo-<hash of
MODULE.bazel.lock>`), and `~/.cache/bazel-disk-cache` with `~/.cache/bazelisk`
(key `bazel-<job>-<hash of MODULE.bazel, the lock file and .bazelversion>-<sha>`).
The extracted repositories are in neither: `.github/ci/prepare.sh` writes
`build --repo_contents_cache=$HOME/.cache/bazel-repo-contents`, a directory
beside the repository cache that no `actions/cache` step names (so as to keep
the saved repository cache inside GitHub's 10 GB quota; the comment in
`prepare.sh` and the "Quota" paragraph at the top of `ci.yml` say so), and the
output base's `external/` (where `+llvm_distribution+dcfs_llvm` lives) is under
`~/.cache/bazel/_bazel_runner/<hash>`, also not cached. So every job
(fast, presubmit, coverage, each shard of full, asan and ubsan) downloads
nothing for LLVM (the archive is in the repository cache) but unpacks it again:
about 14 extractions per push (fast, presubmit, coverage, `reproducible`'s two
builds in two output bases, nine shards, `mutation-changed`).

What caching it would take, not done here (the profile numbers decide):

- Cache `~/.cache/bazel-repo-contents`, or only the `dcfs_llvm` entry of it.
  Bazel 9 names an entry by a hash of the repository rule's inputs (the
  attributes, `llvm.bzl`, `extract.py`, the watched files), so a stale entry
  is simply never matched and a restore is always safe: no key logic of ours
  has to be right for soundness, only for hit rate. The key would be the hash
  of `MODULE.bazel.lock`, `third_party/llvm/*` and `.bazelversion`, saved on
  a key miss only (as the repository cache is).
- Cost: the tree is 2.1 GB (the whole contents cache of this lane is much
  larger: 40 GB over all pins and checkouts, 13 GB for one job's set
  according to the README, which I did not re-measure), against GitHub's 10 GB per repository shared
  with the three per-commit disk caches (up to 4 GB each) and the repository
  cache (a few GB). A 2.1 GB entry (about 1 GB compressed, my estimate, not
  measured) fits only if the per-commit disk caches shrink or are evicted
  more often. Restoring it costs the download and zstd extraction of
  `actions/cache` (a minute or two on a runner, unmeasured) instead of
  the 10 to 30 minutes of extraction.
- Caching the output base's `external/dcfs_llvm` directly instead is the
  same data but unsound by comparison: Bazel validates `external/` entries by
  marker files and the repository's recorded inputs, and a restored directory
  whose marker disagrees is refetched anyway; the contents cache is the
  supported form.
- Not an option: `--repository_cache` only holds the archive (that is
  what it already caches), the expensive part is the unpacking and the patching
  of the shared libraries.

## Execution log size

`--execution_log_compact_file` for the fast-tier run above: 900,531 bytes (0.9
MB) next to a 266,351 byte gzipped profile, 128 logged spawns. Kept: the test
jobs write it beside the profile and upload both. Not measured on a cold run,
where every action is a spawn and the log is larger; if a cold run shows it
too large, drop it from `test.sh` and `coverage.sh`.

## Not covered

`mutation-changed` and the weekly `mutation.yml` run write no profile: each
mutant starts its own Bazel on a copy of the tree (`tools/mutation/mutate.py`),
and the run's own results already record each mutant's time. The jobs
that are not Bazel (`subjects`) have none. The fast job was not run under
`act` (an empty output base there would fetch and extract everything, about
an hour here).
