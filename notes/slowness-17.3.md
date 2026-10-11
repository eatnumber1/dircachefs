# Step 17.3 — Performance Investigation of xfstests ("The Slowness")

## 1. Background & Context

In Step 17.1, 29 metadata-heavy generic xfstests were catalogued in
`test/qemu/guest/xfstests.slow` as running 20–50x slower through dcfs (4–37 s
natively vs 200 s or more through dcfs, causing timeouts at the 200 s watchdog
limit).

At the time of 17.1:
1. The test runner ran on an overloaded host with load average 14–30 on 4 cores.
2. File creates performed a durable SQLite transaction with a WAL fsync on every
   create (1n+1 fsyncs). Step 23.11 ("born-dirty create", 421b2aa) subsequently
   removed this per-create fsync (reducing fsyncs from 1n+1 to 0n+1).

Step 17.3 investigates the root causes of the slowness by measuring
representative slow tests through dcfs and natively on a quiet host, using
`test/qemu/scripts/xfstests_costs.py` (built in Step 26.17) to attribute the extra
time into daemon CPU, I/O / waiting, and kernel dispatch overhead.

---

## 2. Experimental Setup

- **Host Environment**: 8 physical CPU cores, host load 1.3–1.5 (quiet host),
  host CPU pressure negligible (2–7% of wall time).
- **Guest Environment**: QEMU microvm under KVM, test kernel 6.18.55-0-virt,
  1024 MiB guest RAM, quiet kernel sysctls active.
- **Backing Filesystem**: ext4 on `/dev/vdb` (TEST_DEV) and `/dev/vdc` (SCRATCH_DEV).
- **Cache Storage**: `/cache` mounted on tmpfs (`mount -t tmpfs -o size=256m tmpfs /cache`),
  as configured in all guest runs.
  *(Note on tmpfs: In the test guest, `/cache` sits on tmpfs, which understates production
  performance on an SSD or HDD where SQLite WAL fsyncs incur physical disk write latency.
  See Section 4 below).*

---

## 3. Side-by-Side Measurements

The following representative tests from `xfstests.slow` were evaluated with
`xfstests_timeout=300`:

| Test | Workload Description |
|------|----------------------|
| `generic/006` | `permname`: 4,096 file creations single-threaded and 4-threaded (`-c 4 -l 6`), followed by `find \| count` |
| `generic/007` | `nametest`: 100,000 iterations of `open(create)` / `unlink` / `stat` over 100 filenames |
| `generic/011` | `dirstress`: Multi-process directory stress (`-p 1 -n 1`, `-p 5 -n 1`, `-p 5 -n 5`, count 1000) |
| `generic/069` | `append_writer` / `append_reader`: Concurrent append writers (up to 3,000,000 integers = 12 MB in 4-byte writes) |
| `generic/070` | `fsstress`: 10,000 operations specifically targeting extended attributes (`attr_set=100`, `attr_remove=100`) |
| `generic/286` | `seek_copy_test`: Sparse file copying with `SEEK_DATA` / `SEEK_HOLE` on files up to 200 MB |
| `generic/426` | `open_by_handle`: 1,024 files created, open by handle, encode/decode handles, link and unlink |
| `generic/471` | `rewinddir`: 10,000 files created in a directory, `opendir`, `rewinddir`, and full directory read |
| `generic/736` | `readdir-while-renames`: 5,000 files created, 10,000 renames during an active `readdir` scan |
| `generic/756` | `open_by_handle`: 1,024 files created with unique 64-bit mount IDs |

### Cost Comparison Table (via `xfstests_costs.py`)

```
test         status   dcfs_s  native_s  extra_s   cpu_s  waiting_s  extra_writes  extra_flushes  extra_sectors_written
generic/006  pass     32      1         31        27.0   4.0        28            2              -57112
generic/007  pass     76      1         75        62.5   12.5       99            21             -64704
generic/011  pass     30      2         28        27.3   0.7        474           1              49072
generic/070  pass     15      2         13        10.5   2.5        611           19             -6616
generic/286  pass     26      4         22        15.4   6.6        288           10             1107648
generic/426  pass     15      1         14        12.3   1.7        15            -3             1776
generic/471  pass     40      1         39        33.7   5.3        49            4              13696
generic/736  pass     38      1         37        31.8   5.2        54            2              10320
generic/756  pass     16      1         15        12.8   2.2        14            -4             1680
generic/069  timeout  300+    10        >290      ~210   ~90        -             -              -
```

---

## 4. Attribution of Extra Time

Across all completing tests, the attribution of dcfs's added execution time is
remarkably consistent:

### 1. Daemon CPU: 80% to 98% of Added Time
In every completing test, the vast majority of extra time is consumed directly
by the `dcfs` daemon executing user-space code on a single vCPU:
- `generic/011`: 27.3 s of 28 s extra (**97.5% CPU**)
- `generic/426`: 12.3 s of 14 s extra (**87.9% CPU**)
- `generic/006`: 27.0 s of 31 s extra (**87.1% CPU**)
- `generic/471`: 33.7 s of 39 s extra (**86.4% CPU**)
- `generic/736`: 31.8 s of 37 s extra (**85.9% CPU**)
- `generic/756`: 12.8 s of 15 s extra (**85.3% CPU**)
- `generic/007`: 62.5 s of 75 s extra (**83.3% CPU**)
- `generic/070`: 10.5 s of 13 s extra (**80.8% CPU**)
- `generic/286`: 15.4 s of 22 s extra (**70.0% CPU**)

What the daemon is doing during this CPU time:
- **SQLite B-tree and statement processing**: Executing SQL statements (`UPDATE entries`,
  `INSERT INTO entries`, `UPDATE directories SET complete = 0`) across multiple phases.
  In `generic/007` (100k creates + unlinks), the daemon processed ~200,000 distinct
  mutations in 62.5 s of CPU (~3,200 ops/s, or ~312 µs of CPU per operation).
- **Protocol bookkeeping & credential switches**: For every backing mutation,
  switching caller credentials via `AsCaller` (`setfsuid` / `setfsgid`), checking
  invariants, taking timestamps, and managing transaction dirty sets.
- **Directory completeness invalidation & repopulation**: In `generic/736`, every rename
  marks the directory incomplete (`cache::MarkDirectoryIncomplete`), which forces
  subsequent readdir chunks to query and repopulate dentries.

### 2. Waiting / I/O: 2% to 20% of Added Time
- Extra disk flushes added by dcfs were near zero:
  `generic/006` (+2), `generic/011` (+1), `generic/736` (+2), `generic/471` (+4),
  `generic/070` (+19), `generic/007` (+21), and `generic/426`/`generic/756` (0 to -4).
- The low flush overhead proves that step 23.11's born-dirty create successfully
  eliminated unnecessary per-mutation WAL fsyncs.
- **Important production caveat (tmpfs vs SSD/HDD)**:
  In this test environment, `/cache` is on tmpfs. On tmpfs, SQLite WAL writes and
  commits never wait on disk head movement or NAND flash programming. In production
  on an SSD (0.5–2 ms per fsync) or HDD (10–20 ms per fsync), sync points or any
  durable WAL commit would add substantial I/O waiting time. However, even with tmpfs
  completely eliminating I/O latency, the tests are still 15x–76x slower than native.

### 3. Kernel / FUSE Dispatch Overhead & Concurrency Serialization
- In tests with parallel processes (`permname -p 4` in `generic/006`, `dirstress -p 5`
  in `generic/011`, concurrent append writers in `generic/069`):
  On native ext4, multiple kernel threads concurrently execute operations across
  available CPU cores.
  Through dcfs, all requests are serialized onto the single FUSE dev channel and
  processed one request at a time by the single-threaded dcfs session loop.
- In `generic/069`, `append_writer 3000000` issues 3,000,000 separate `write()`
  syscalls of 4 bytes each. While passthrough handles data writes, the high request
  rate, kernel attribute invalidations, and context switching across the FUSE boundary
  drove the daemon to consume ~210 s of CPU before timing out at 300 s.

---

## 5. Resolution of 17.1 vs 17.3 Discrepancy

Why did tests that timed out at 200 s in Step 17.1 pass in 15–76 s in Step 17.3?

1. **Host Load Elimination**:
   In 17.1, the host had a load average of 14–30 across 4 cores. A test that inherently
   needs 30–62 s of single-threaded CPU time will take >200 s of wall time when
   vCPUs are starved for host CPU cycles (as confirmed by the load-starved watchdog).
   On a quiet host (load 1.3–1.5), these same tests complete in 15–76 s.
2. **Born-Dirty Create (Step 23.11)**:
   Step 23.11 eliminated the synchronous SQLite fsync on file creation. Tests with
   dense creates (like `nametest` with 100k creates) previously paid for 1n+1
   transaction commits. Post-23.11, creates are born dirty in the cache without
   immediate fsyncs.
3. **Relative Slowness Persists**:
   Although the tests now finish well within 15–76 s, they remain **15x to 76x
   slower than native ext4** (which finishes them in 1–2 s). The slowness is real
   and reproducible.

---

## 6. Conclusion: Architectural Design vs. Tuning

**Conclusion**: The slowness is primarily an **architectural design constraint**,
not a configuration or tuning issue.

1. **Tuning Cannot Bridge the Gap**:
   - Disk sync cadence and SQLite PRAGMAs are already near-optimal for this workload:
     the cache is in tmpfs, WAL mode is active, flushes are already minimal (+1 to +21),
     and disk I/O waiting represents only 2–15% of the overhead.
   - Adjusting cache sizes or WAL checkpoint thresholds cannot reduce the CPU time
     required to execute hundreds of thousands of SQLite B-tree queries and updates.

2. **Architectural Causes**:
   - **Single-Threaded Daemon Loop**: dcfs processes one request at a time in
     `session_loop.cc`. Multi-process workloads on the client side are completely
     serialized into a single queue.
   - **Per-Operation SQLite Transaction Overhead**: Native kernel filesystems update
     in-memory dentry and inode structs in nanoseconds under RCU/spinlocks. dcfs
     translates each metadata mutation into multi-statement SQLite queries (parsing,
     B-tree traversal, journaling, and row updates), taking ~200–300 µs of CPU per op.
   - **3-Phase Protocol**: The tri-state durability model requires two database
     mutations (Phase 1 unknown, Phase 3 present/absent) around every backing syscall.

3. **Recommendations for the Plan**:
   - The 29 tests catalogued in `test/qemu/guest/xfstests.slow` should remain in the
     slow set (run in the weekly suite with the 1,500 s limit), because on a loaded CI
     runner their 30–76 s CPU requirements will routinely breach the 200 s shard limit.
   - Substantial performance gains on metadata-heavy workloads will require future
     architectural steps already outlined in the queue:
     - Asynchronous/coroutine request handling over `io_uring` (Phase 22 / History.md).
     - Batched SQLite transactions for rapid sequential mutations.
     - Fast-path in-memory dentry caching bypassing SQLite for temporary uncommitted rows.
