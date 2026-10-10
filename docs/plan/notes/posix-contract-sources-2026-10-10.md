# External sources for the POSIX filesystem contract (2026-10-10)

russ: "We need to find an external source to tell us the contract of a POSIX
filesystem." Candidates, from memory of the literature (verify versions and
licences before adopting; nothing here was fetched today):

| Source | What it is | Covers | Form | Fit for dcfs |
|---|---|---|---|---|
| **SibylFS** (Ridge, Sheets, Tuerk, Giugliano, Madhavapeddy, Sewell; SOSP 2015; sibylfs.github.io) | An executable formal specification of POSIX file-system behaviour, written in Lem, validated against Linux (ext4, tmpfs, btrfs, ...), macOS and FreeBSD with tens of thousands of traces; per-OS/per-fs deviations are flags in the spec | path resolution, permissions, open/close/read/write/pread/pwrite/lseek, link/unlink/rename/mkdir/rmdir/symlink/readlink, stat, chmod/chown, truncate, readdir, umask; sequential, no crashes, no xattrs, no mmap | Lem (exports to HOL4, Isabelle, Coq, OCaml); an OCaml **trace checker** that says whether a syscall trace is allowed | The normative reference for 12.16's abstract spec: the allowed outcomes per operation, cited by Lem definition name. The checker could run on dcfs traces as an oracle (the 12.11 shape). Dormant since about 2016; OCaml through Bazel is the cost |
| **FSCQ / DFSCQ** (Chen, Ziegler, Chajed, Chlipala, Kaashoek, Zeldovich; SOSP 2015/2017) | A Coq-verified crash-safe file system; DFSCQ specifies fsync/fdatasync as "metadata-prefix" over tree sequences | the crash contract: which states may be on disk after a crash given the fsyncs issued | Coq | Reference for the crash regimes (12.8) and for what "durable after fsync" promises; not a POSIX-wide contract |
| **Ferrite** (Bornholt, Kaufmann, Li, Krishnamurthy, Torlak, Wang; ASPLOS 2016) | Crash-consistency models for file systems as litmus tests, with ext4's and others' models | ordering of writes across a crash | Rosette (Racket) | Litmus tests for the ACE/power-cut harness; the vocabulary for stating what dcfs preserves of the backing's model |
| **AtomFS** (Zou, Ding, Chen et al.; SOSP 2019) | A verified concurrent file system; the spec is linearizability of file-system operations | atomicity under concurrency (rename, link, path walks) | Coq | The notion 25.10 and the coroutine work need: each operation linearizable, which is what "rename must be atomic" means formally |
| **Yggdrasil / Yxv6** (Sigurbjarnarson, Bornholt, Torlak, Wang; OSDI 2016) | Crash refinement checked with Z3; a file system verified against an abstract spec by refinement | crash refinement | Python + Z3 | The methodology 12.16 (refinement target) follows; its spec is of its own FS |
| **Cogent / BilbyFS** (Amani et al.; ASPLOS 2016) | A verified flash file system | its own functional spec | Isabelle | Less relevant |
| POSIX.1-2024 (IEEE 1003.1), Linux man-pages, the VFS documentation | the prose contract and Linux's deviations (e.g. rename atomicity text in rename(2)) | everything, informally | prose | The source SibylFS formalised; cite alongside |
| pjdfstest, xfstests | conformance suites, black-box | the operations' observable behaviour | tests | already in use (phases 16, 17); they test, they do not state the contract |

## Proposal

1. **12.16's abstract spec is SibylFS's semantics restricted to dcfs's
   operation set**, written in TLA+ by hand with each action's allowed
   outcomes cited to the Lem definitions (the spec is large; only the
   operations and flags dcfs serves are translated). Deviations the
   backing itself makes (SibylFS's per-fs flags for ext4/btrfs/xfs) are
   deviations dcfs inherits, which is russ's rule stated formally.
2. **A spike (12.18): can SibylFS's checker build hermetically through
   Bazel and check a dcfs trace?** If yes, it joins 12.11 as a second
   trace oracle, independent of our model: one run of a workload through
   dcfs and the same workload on the backing, both traces checked. If the
   OCaml toolchain is too heavy, the spec stays a reading reference.
3. **The crash contract** stays ours (12.8's regimes), with DFSCQ's
   metadata-prefix and Ferrite's models as the reference vocabulary, since
   SibylFS has no crashes.
4. **Atomicity** is stated as linearizability (AtomFS's spec), which is
   the property 25.10's liveness work and the coroutine design must keep.

Not dispatched: the budget freeze. The quota resets Friday 2026-10-16.
