# SibylFS trace checker as a second oracle: Spike report (2026-10-11, dcfs-investigator)

Spike report for **Step 12.18**: investigating whether the SibylFS trace checker (OCaml) can be built hermetically through Bazel (or pinned as a prebuilt binary) and used as a second oracle to validate dcfs FUSE reply traces.

All empirical observations below were measured in `lane-3` on branch `step-12.18`.

---

## Verdict

**Do not adopt SibylFS's trace checker as an oracle.**

SibylFS's checker should not be incorporated into dcfs's test suite or CI trace validation pipeline. Instead, SibylFS should remain strictly a **normative reading reference and specification source** as planned in **Step 12.16** (where its Lem definitions are hand-translated into TLA+ for dcfs's supported operations and checked via TLC refinement mapping).

The primary reasons:
1. **Toolchain from source is intractable under Bazel**: SibylFS is an academic artifact from 2015 (Ridge et al., SOSP 2015, repo `sibylfs/sibylfs_src`) written in Lem and OCaml ~4.02. Building from source requires `lem`, `cppo`, and legacy OCaml. Modern OCaml (4.08+, 5.x) breaks the codebase. The Bazel Central Registry (BCR) has no working rules for legacy OCaml or Lem.
2. **Prebuilt binaries are hermetically runnable but architecturally rigid**: Prebuilt x86_64 binaries (`check.native`, `posix.native`, `fs_test`) exist in GitHub release `sibylfs/sibylfs_binaries` (tag `2015-08-20`, SHA-256 `5a41b87c...`) and execute cleanly against host glibc. However, they are static 2015 snapshots (no aarch64, no modifiability).
3. **Severe semantic impedance mismatch with FUSE**:
   - dcfs speaks FUSE (`FUSE_LOOKUP`, `FUSE_GETATTR`, `FUSE_CREATE`, `FUSE_OPEN`, `fh`, parent `nodeid` + name), not POSIX libc syscalls with path strings and process file descriptors.
   - Kernel VFS caching absorbs and deduplicates operations before they reach FUSE; FUSE events do not map 1:1 to POSIX syscall histories.
   - FUSE requests arrive concurrently; linearizing interleaved asynchronous requests into a valid sequential trace is NP-complete.
4. **SibylFS enforces an artificial metadata abstraction**: In empirical tests against real Linux filesystems, `check.native` rejects real Linux `stat` outputs because SibylFS's abstract specification expects synthetic metadata: `st_dev=2049`, `st_ino` numbered from 0, directory `st_size=9999`, and root directory `st_nlink=1` (Linux ext4 gives `st_nlink=2+`). Checking a real filesystem trace requires a lossy, brittle normalization layer that strips actual inode and link count behaviors.
5. **Redundant with existing test suites**: Black-box POSIX conformance testing against backing filesystems is already handled directly and comprehensively by `pjdfstest` (Phase 16) and `xfstests` (Phase 17).
6. **Irrelevant to dcfs's core correctness guarantees**: SibylFS has zero crash-consistency semantics (no fsync prefix modeling, no power-loss states) and zero awareness of dcfs's write-through cache protocol, SQLite tri-state records (`present`, `absent`, `unknown`), or mutation phases. Step 12.11's TLA+ trace validator already checks the internal protocol soundly.

---

## 1. Feasibility: OCaml Toolchain vs. Prebuilt Binary under Bazel

### 1.1 Building from source under Bazel
- **Upstream source**: `https://github.com/sibylfs/sibylfs_src` (dormant since 2016, last commits ~2020).
- **Source compilation graph**:
  1. `*.lem_cppo` preprocessed by `cppo` -> `*.lem`.
  2. Lem tool (`lem`, Cambridge formal specification language compiler) compiles `t_fs_spec.lem` (274 KB of Lem code) into OCaml modules `t_fs_spec.ml`.
  3. Generated OCaml files are patched with `sed` (`patch_lem.sed`).
  4. OCaml native compiler (`ocamlopt` / `ocamlbuild` / `findlib`) links with OPAM dependencies: `ctypes`, `sexplib`, `cmdliner`, `unix`, `bigarray`.
- **Portability & Toolchain roadblocks**:
  - The source requires OCaml 4.02.x / 4.03. Modern OCaml (4.08+, 5.x) introduced incompatible standard library renames (`Pervasives` -> `Stdlib`), mutable string removal (`-safe-string` mandatory), and breaking AST revisions in `ppx_sexp_conv` and `ctypes`.
  - The Bazel Central Registry (BCR) has no mature or hermetic OCaml 4.02 toolchain rules, let alone rules for Lem. Building Lem and OCaml 4.02 from C source within Bazel would require thousands of lines of custom repository rules and toolchains.
  - **Conclusion**: Hermetic source compilation under Bazel is cost-prohibitive and unmaintainable.

### 1.2 Hermetic prebuilt binary pinned by hash
- **Upstream binary distribution**: GitHub repository `sibylfs/sibylfs_binaries` holds release archives uploaded by maintainer Tom Ridge.
- **Pin candidate**:
  - Release tag: `2015-08-20` (and `2015-10-05`)
  - URL: `https://github.com/sibylfs/sibylfs_binaries/releases/download/2015-08-20/sibylfs_linux_amd64.tar.gz`
  - Archive size: 5,994,337 bytes
  - SHA-256: `5a41b87c74765dabac3dd93a9de2bd7714c1075db7d69d6096a6f205eddb7ee2`
- **Binary characteristics**:
  - Contains: `check.native` (3.6 MB), `posix.native` (4.1 MB), `fs_test` (6.8 MB), `run_trace` (2.5 MB), `tgen` (3.5 MB).
  - ELF 64-bit LSB executable, x86-64, dynamically linked against system glibc (`libc.so.6`, `libm.so.6`, `libdl.so.2`, interpreter `/lib64/ld-linux-x86-64.so.2`).
  - Required symbols are baseline GLIBC 2.2.5+; zero additional dynamic library dependencies (`libz`, `libfuse`, etc. are not needed).
- **Execution verification**:
  - We executed `check.native` directly on Ubuntu 24.04 (Linux 6.6 x86_64, glibc 2.39). It started immediately, accepted CLI arguments, parsed trace files, and executed the specification checker without compatibility errors.
- **Bazel packaging feasibility**:
  - A Bazel rule could pin this archive via standard `http_archive`:
    ```python
    http_archive(
        name = "sibylfs_bin",
        urls = ["https://github.com/sibylfs/sibylfs_binaries/releases/download/2015-08-20/sibylfs_linux_amd64.tar.gz"],
        sha256 = "5a41b87c74765dabac3dd93a9de2bd7714c1075db7d69d6096a6f205eddb7ee2",
        strip_prefix = "sibylfs",
        build_file_content = """
    filegroup(
        name = "checker",
        srcs = ["bin/check.native"],
        visibility = ["//visibility:public"],
    )
    """,
    )
    ```
- **Operational limitations of the prebuilt binary**:
  - Platform support: strictly x86-64 Linux (no aarch64; Apple Silicon / ARM runners in CI would need emulation or fail).
  - Exit code behavior: `check.native` exits with status `0` even when a trace is rejected (it prints `# trace not accepted` to stdout or writes diagnostic S-expressions with `--sexp`). Any Bazel test rule would need a wrapper script to parse stdout or the S-expression.
  - Frozen model: Cannot patch spec rules or add missing syscall definitions.

---

## 2. Trace Format Mapping: dcfs FUSE Replies to SibylFS Traces

### 2.1 SibylFS trace syntax
SibylFS traces (`@type trace`) record an interleaved sequence of POSIX system call invocations and observed results:
```text
@type trace
1: mkdir "/foo" 0o777
2: Tau
3: RV_none
4: open "/foo/bar.txt" [O_CREAT;O_RDWR] 0o644
5: Tau
6: RV_num 3
7: close 3
8: Tau
9: RV_none
```
When an error occurs, the return line holds the errno symbol (e.g. `EEXIST`, `ENOENT`).
Evaluation:
- Valid trace: `check.native` prints `# trace accepted`.
- Invalid trace (e.g. duplicate `mkdir` returning `RV_none` instead of `EEXIST`): `check.native` prints:
  ```text
  Error: The spec permitted: EEXIST
  Error: 7: RV_none
     unexpected results: RV_none
     allowed are only: EEXIST
  # trace not accepted
  ```

### 2.2 Impedance mismatches between FUSE replies and SibylFS traces

1. **Path hierarchy vs. Node IDs**:
   - FUSE communicates using inode numbers (`nodeid`) and entry names relative to parent directories (e.g., `LOOKUP(parent=1, name="file")`).
   - SibylFS expects canonical absolute pathname strings (`open "/dir/file"`).
   - *Translation difficulty*: A tracer would need to reconstruct full pathname resolution statefully for every FUSE request.

2. **File Descriptors vs. FUSE File Handles**:
   - FUSE `CREATE` and `OPEN` return an internal 64-bit uint file handle (`fh`), which dcfs uses to index open files in its SQLite and memory cache.
   - SibylFS models per-process file descriptor tables where `open` returns a small integer (`3`, `4`, ...) that subsequent `read`/`write`/`close` calls reference.
   - *Translation difficulty*: Requires synthesizing a virtual client file descriptor table.

3. **VFS Caching and Request Elision**:
   - The Linux kernel VFS layer sits between user-space applications and the FUSE driver. The kernel dentry cache (dcache) and page cache absorb repeated calls:
     - Sequential `stat()` calls by an application do not generate `FUSE_GETATTR` if attributes are within `attr_timeout`.
     - Read/write operations may be merged, reordered, or satisfied from page cache.
     - Lookups precede almost every mutating FUSE operation.
   - A trace of FUSE replies does not match any valid user-space POSIX application syscall trace.

4. **Empirical Measurement: Inode and Device Number Divergence (`RV_stat`)**:
   We ran `check.native` on an execution trace recorded from a real Linux filesystem (`adhoc_mkdir_link_count-int.trace`). The checker **rejected** the real Linux trace:
   ```text
   # Real ext4 trace:
   stat "/" -> RV_stat { st_dev=1792; st_ino=32644; st_kind=S_IFDIR; st_perm=0o777; st_nlink=2; st_uid=0; st_gid=0; st_rdev=0; st_size=4096; }

   # Spec rejection:
   Fatal error: RV_stat { st_dev=1792; st_ino=32644; ... }
   The spec permitted:
   RV_stat { st_dev=2049; st_ino=0; st_kind=S_IFDIR; st_perm=0o777; st_nlink=1; st_uid=0; st_gid=0; st_rdev=0; st_size=9999; }
   ```
   **Key Finding**: SibylFS does not check equivalence classes of POSIX attributes; it checks against a rigid deterministic internal state machine where:
   - Root device is hardcoded to `2049`.
   - Inode numbers are sequentially numbered starting at `0` or `1`.
   - Directory sizes are hardcoded to dummy value `9999`.
   - Root directory link count is modelled as `1` (whereas POSIX/Linux root directory with `.` and `..` has `st_nlink=2` or higher).
   To feed real filesystem or dcfs traces into `check.native`, an extensive artificial normalization script must sanitize and rewrite all device IDs, inode numbers, directory sizes, and link counts.

5. **Concurrency and Asynchrony**:
   - dcfs processes concurrent FUSE requests asynchronously with unique request identifiers (`unique`).
   - SibylFS evaluates strictly sequential per-process traces (`Pid 1`). Interleaved concurrent operations cannot be fed into SibylFS without computing an explicit linearization order.

6. **Missing POSIX Features in SibylFS**:
   - Extended attributes (`xattr`): Not in SibylFS (dcfs supports xattrs).
   - Creation/birth times (`btime` / `statx`): Not in SibylFS (dcfs models birth times in `formal/ident.tla`).
   - `fallocate`, `io_uring`, `flock`, `readdirplus` cookies: Not in SibylFS.
   - Crash consistency: SibylFS has no model of `fsync` barriers, WAL commits, torn writes, or crash states.

---

## 3. Assessment and Recommendation

### 3.1 Comparison with TLA+ Trace Validation (Step 12.11)
| Dimension | TLA+ Trace Validator (12.11) | SibylFS Checker (12.18) |
|---|---|---|
| **What it tests** | White-box internal write-through cache protocol and state invariants | Black-box sequential POSIX syscall return values |
| **Protocol coverage** | SQLite tri-state records (`present`, `absent`, `unknown`), 3-phase mutations, fills, dirty sets | None (unaware of cache, database, or FUSE) |
| **Crash & Durability** | Durability levels (`kNormal`, `kSync`), WAL commit ordering, crash recovery prefixes (`formal/Trace.tla`) | None (SibylFS has no crash model) |
| **Concurrency** | Models multi-threaded interleavings, cancellation (`checkpoint.h`), held file descriptor table | Strictly sequential per-PID |
| **Hermeticity** | Fully hermetic: Java + TLC pinned via Bazel (`@tlaplus`) | Prebuilt binary pinned by hash (x86_64 only) |

### 3.2 Comparison with Conformance Suites (Phases 16 and 17)
- Black-box POSIX verification against both dcfs and backing filesystems (ext4, xfs, btrfs) is already established through:
  - `pjdfstest` (Phase 16): exercises POSIX syscall correctness directly inside the QEMU guest.
  - `xfstests` (Phase 17): exercises filesystem regression and stress tests.
- Testing a workload through dcfs and backing, converting to SibylFS trace syntax, normalizing artificial attributes, and running `check.native` adds significant pipeline complexity and flakiness while providing strictly less coverage than `pjdfstest`.

### 3.3 Final Recommendation
1. **Drop SibylFS as an active trace validation oracle**: Do not implement a FUSE-to-SibylFS trace converter or integrate `check.native` into Bazel test targets.
2. **Retain SibylFS as the normative specification reference in Step 12.16**:
   - Use SibylFS's Lem definitions (`t_fs_spec.lem`) as the formal definition of POSIX operations.
   - For Step 12.16, hand-translate SibylFS's operational semantics restricted to dcfs's operation set into TLA+ (`Ideal.tla`), explicitly citing the Lem definitions and per-filesystem Linux flags (ext4/xfs/btrfs).
   - Verify refinement with TLC (`Spec => Ideal!Spec`).
