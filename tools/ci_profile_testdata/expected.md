#### fast_profile

| What | Wall (s) | Busy (s) | n |
|---|---:|---:|---:|
| Total wall | 2412.0 | | |
| Repository fetches (not @dcfs_llvm) | 53.9 | 107.8 | 9 |
| @dcfs_llvm fetch and extraction | 1894.7 | 1894.7 | 7 |
| Third-party builds | 87.0 | 102.6 | 5 |
| Our compile and link | 27.1 | 27.1 | 4 |
| Other actions | 0.0 | 0.0 | 0 |
| Test execution | 121.3 | 161.5 | 4 |
| Critical path (Bazel's) | 137.5 | 137.5 | 6 |

Longest critical path components:
- 120.7 s: action 'Testing //dcfs:dir_cache_fs_test'
- 15.6 s: action 'Compiling absl/log/globals.cc'
- 0.7 s: action 'Linking dcfs/dir_cache_fs_test_bin'

Wall is the time at least one such event was running (rows overlap); busy is the sum of the events' durations.
